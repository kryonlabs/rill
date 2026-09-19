import runpy
import tempfile
import unittest
from pathlib import Path
import xml.etree.ElementTree as ET

prepare = runpy.run_path('scripts/rill-session')['prepare']

class SessionTest(unittest.TestCase):
    def test_existing_panel_layout_is_the_default(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            panel = home / '.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml'
            panel.parent.mkdir(parents=True)
            contents = '<channel name="xfce4-panel"><property name="panels" type="array"><value type="int" value="9"/><property name="panel-9" type="empty"/></property></channel>'
            panel.write_text(contents)
            result = prepare({'HOME': directory, 'RILL_SESSION': 'xfce'}, '/opt/rill/rill')
            imported = Path(result['XDG_CONFIG_HOME']) / panel.relative_to(home / '.config')
            self.assertEqual(imported.read_text(), contents)

    def test_import_and_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            original = home / '.config'
            panel = original / 'xfce4/panel/plugin.rc'
            panel.parent.mkdir(parents=True)
            panel.write_text('original settings')
            desktop = original / 'xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml'
            desktop.parent.mkdir(parents=True)
            desktop.write_text('<channel name="xfce4-desktop"/>')
            env = {'HOME': directory, 'RILL_SESSION': 'xfce', 'SESSION_MANAGER': 'old', 'WAYLAND_DISPLAY': 'wayland-1'}
            result = prepare(env, '/opt/My Rill/rill')
            profile = Path(result['XDG_CONFIG_HOME'])
            self.assertEqual((profile / desktop.relative_to(original)).read_text(), desktop.read_text())
            panel_xml = ET.parse(profile / 'xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml')
            self.assertEqual([v.get('value') for v in panel_xml.findall("./property[@name='panels']/value")], ['1'])
            self.assertIsNone(panel_xml.find(".//property[@name='panel-2']"))
            imported = profile / 'xfce4/panel/plugin.rc'
            self.assertEqual(imported.read_text(), 'original settings')
            imported.write_text('rill settings')
            prepare(env, '/opt/My Rill/rill')
            self.assertEqual(imported.read_text(), 'rill settings')
            self.assertEqual(panel.read_text(), 'original settings')
            self.assertNotIn('SESSION_MANAGER', result)
            self.assertNotIn('WAYLAND_DISPLAY', result)
            self.assertTrue(result['XDG_CONFIG_DIRS'].startswith(str(original) + ':'))
            xml = ET.parse(profile / 'xfce4/xfconf/xfce-perchannel-xml/xfce4-session.xml')
            command = xml.find(".//property[@name='Rill']/property[@name='Client4_Command']")
            self.assertEqual([item.get('value') for item in command],
                             ['/opt/My Rill/rill', '--desktop', '--external-panel'])
            wm = xml.find(".//property[@name='Rill']/property[@name='Client0_Command']/value")
            self.assertEqual(wm.get('value'), '/opt/My Rill/rill-wm')
            self.assertEqual(len(xml.findall(".//property[@name='Rill']")), 1)



class NativeSessionTest(unittest.TestCase):
    def test_native_session_has_no_required_xfce_programs(self):
        module = runpy.run_path('scripts/rill-session')
        with tempfile.TemporaryDirectory() as directory:
            env = {'HOME': directory, 'RILL_SESSION_SERVICES': 'none', 'PATH': '/nonexistent'}
            result = module['prepare'](env, '/opt/My Rill/rill')
            self.assertEqual(result['XDG_CURRENT_DESKTOP'], 'Rill:XFCE')
            self.assertEqual(module['required_commands'](env, '/opt/My Rill/rill'),
                             ['dbus-run-session', '/opt/My Rill/rill-sessiond', '/opt/My Rill/rill-wm'])
            command = module['session_command'](result, '/opt/My Rill/rill')
            self.assertEqual(command[-3:], ['/opt/My Rill/rill', '--desktop', '--external-panel'])
            self.assertNotIn('sh', command)
            services = Path(result['RILL_SESSION_SERVICES_FILE']).read_text()
            self.assertIn('Command=/opt/My Rill/rill-wm;', services)
            self.assertIn('Command=/opt/My Rill/rill;--panel;primary;', services)
            self.assertIn('Restart=true', services)
            self.assertIn('Required=true', services)

    def test_native_profile_preserves_preferences_and_named_panels(self):
        import json
        module = runpy.run_path('scripts/rill-session')
        with tempfile.TemporaryDirectory() as directory:
            original = Path(directory) / '.config'
            personal = original / 'rill'
            personal.mkdir(parents=True)
            (personal / 'settings').write_text('panel-height = 36\n')
            (original / 'user-dirs.dirs').write_text('XDG_DESKTOP_DIR="$HOME/Escritorio"\n')
            (original / 'editor').mkdir()
            (original / 'editor/preferences').write_text('font-size=18\n')
            (original / 'autostart').mkdir()
            (original / 'autostart/personal.desktop').write_text('[Desktop Entry]\nHidden=true\n')
            (personal / 'panels.json').write_text(json.dumps([
                {'id': 'primary'}, {'id': 'second', 'output': 'DP-2', 'edge': 'right',
                                     'size': 44, 'autohide': True},
            ]))
            result = module['prepare']({'HOME': directory, 'RILL_SESSION_SERVICES': 'none'}, '/opt/rill/rill')
            profile = Path(result['XDG_CONFIG_HOME'])
            self.assertEqual((profile / 'user-dirs.dirs').read_text(), (original / 'user-dirs.dirs').read_text())
            self.assertEqual((profile / 'rill/settings').read_text(), 'panel-height = 36\n')
            self.assertEqual((profile / 'editor/preferences').read_text(), 'font-size=18\n')
            (profile / 'editor/preferences').write_text('font-size=20\n')
            self.assertEqual((original / 'editor/preferences').read_text(), 'font-size=20\n')
            self.assertFalse((profile / 'autostart').is_symlink())
            self.assertEqual(list((original / 'autostart').iterdir()),
                             [original / 'autostart/personal.desktop'])
            services = Path(result['RILL_SESSION_SERVICES_FILE']).read_text()
            self.assertIn('--panel;second;--panel-edge;right;--panel-size;44;'
                          '--panel-autohide;1;--panel-output;DP-2;', services)
            (profile / 'rill/settings').write_text('panel-height = 28\n')
            module['prepare']({'HOME': directory, 'RILL_SESSION_SERVICES': 'none'}, '/opt/rill/rill')
            self.assertEqual((profile / 'rill/settings').read_text(), 'panel-height = 28\n')

    def test_disabled_autostart_does_not_return_as_supervised_service(self):
        module = runpy.run_path('scripts/rill-session')
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            user, system = work / 'user/autostart', work / 'system/autostart'
            user.mkdir(parents=True)
            system.mkdir(parents=True)
            (user / 'network.desktop').write_text('[Desktop Entry]\nHidden=true\n')
            (system / 'network.desktop').write_text('[Desktop Entry]\nExec=nm-applet\n')
            disabled = module['disabled_programs']({'XDG_CONFIG_HOME': str(user.parent),
                                                    'XDG_CONFIG_DIRS': str(system.parent)})
            self.assertIn('nm-applet', disabled)

    def test_service_overrides_preserve_argument_boundaries(self):
        import json
        module = runpy.run_path('scripts/rill-session')
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / '.config/rill'
            config.mkdir(parents=True)
            (config / 'services.json').write_text(json.dumps([
                {'name': 'Window manager', 'command': ['/some path/wm', 'semi;colon', 'two words'],
                 'required': True},
            ]))
            result = module['prepare']({'HOME': directory, 'RILL_SESSION_SERVICES': 'none'}, '/opt/rill/rill')
            services = Path(result['RILL_SESSION_SERVICES_FILE']).read_text()
            self.assertIn('Command=/some path/wm;semi\\;colon;two words;', services)
            self.assertNotIn('/opt/rill/rill-wm', services)


if __name__ == '__main__':
    unittest.main()
