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
            result = prepare({'HOME': directory}, '/opt/rill/rill')
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
            env = {'HOME': directory, 'SESSION_MANAGER': 'old', 'WAYLAND_DISPLAY': 'wayland-1'}
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

if __name__ == '__main__':
    unittest.main()
