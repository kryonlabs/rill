"""Run via xvfb-run; all session configuration and buses are private."""
import json
import re
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parent.parent
binary = Path(os.environ.get('RILL_BIN', root / 'build/linux-x86_64/rill')).resolve()
native = os.environ.get('RILL_TEST_NATIVE') == '1'


def wait_for(predicate, message, seconds=45):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.1)
    raise AssertionError(message)


with tempfile.TemporaryDirectory(prefix='rill-session-') as directory:
    work = Path(directory)
    runtime = work / 'runtime'
    runtime.mkdir(mode=0o700)
    ready = work / 'ready'
    env = dict(os.environ, RILL_SESSION='rill' if native else 'xfce', HOME=str(work), XDG_CONFIG_HOME=str(work / 'config'),
               XDG_CACHE_HOME=str(work / 'cache'), XDG_RUNTIME_DIR=str(runtime),
               DBUS_SYSTEM_BUS_ADDRESS='unix:path=' + str(work / 'no-system-bus'),
               GNUPGHOME=str(work / 'gnupg'), RILL_TEST_READY_FILE=str(ready),
               RILL_BIN=str(binary), PLAN9=str(root.parent / 'plan9port'),
               DEVDRAW=str(root.parent / 'plan9port/bin/devdraw'))
    for name in ('SESSION_MANAGER', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'DBUS_SESSION_BUS_ADDRESS'):
        env.pop(name, None)
    if native:
        env.update(RILL_SESSION_SERVICES='none', RILL_NO_AUTOSTART='1')
        (work / 'Desktop').mkdir()
        desktop_probe = work / 'desktop-opened'
        app_log = work / 'launched-applications'
        app_script = work / 'desktop-probe.py'
        app_script.write_text('import os,time\nfrom pathlib import Path\n'
                              'with Path(' + repr(str(app_log)) + ').open("a") as f:\n'
                              ' f.write(str(os.getpid())+"\\n")\n'
                              'Path(' + repr(str(desktop_probe)) + ').touch()\n'
                              'time.sleep(120)\n')
        (work / 'Desktop/probe.desktop').write_text(
            '[Desktop Entry]\nType=Application\nName=Desktop Probe\nExec="' +
            sys.executable + '" "' + str(app_script) + '"\n')

    def clients():
        found = []
        for item in Path('/proc').iterdir():
            if not item.name.isdigit():
                continue
            try:
                if (item / 'exe').resolve() == binary:
                    if b'--desktop' not in (item / 'cmdline').read_bytes().split(b'\0'):
                        continue
                    values = dict(part.split(b'=', 1) for part in (item / 'environ').read_bytes().split(b'\0') if b'=' in part)
                    if values.get(b'RILL_TEST_READY_FILE') == str(ready).encode():
                        found.append((int(item.name), {k.decode(): v.decode() for k, v in values.items()}))
            except (OSError, ValueError):
                pass
        return found

    if os.environ.get('RILL_TEST_XFCE_PROFILE'):
        shutil.copytree(os.environ['RILL_TEST_XFCE_PROFILE'], work / 'config/xfce4')
    plugins = []
    if os.environ.get('RILL_TEST_ALL_PLUGINS') == '1':
        from plugin_inventory import inventory, configure, loaded_modules
        plugins = inventory()
        assert plugins, 'no installed plugins found'
        configure(work / 'config', plugins)

    with (work / 'session.log').open('w+') as log:
        launcher = 'rill-window' if os.environ.get('RILL_TEST_NESTED') else 'rill-session'
        process = subprocess.Popen([str(root / 'scripts' / launcher)], env=env,
                                   stdout=log, stderr=log, start_new_session=True)
        try:
            wait_for(ready.exists, 'desktop did not become ready')
            pid, client_env = wait_for(clients, 'no session desktop')[0]
            wm_check = subprocess.check_output(['xprop', '-root', '_NET_SUPPORTING_WM_CHECK'], env=client_env, text=True)
            wm_id = wm_check.strip().split()[-1]
            wm_name = subprocess.check_output(['xprop', '-id', wm_id, '_NET_WM_NAME'], env=client_env, text=True)
            if not os.environ.get('RILL_WM'):
                assert 'Rill WM' in wm_name, wm_name
            tree = subprocess.check_output(['xwininfo', '-root', '-tree'], env=client_env, text=True)
            assert '"Rill"' in tree, tree
            assert ('xfce4-panel' in tree) == (not native), tree
            if native:
                assert 'Rill panel primary' in tree, tree
                panel = subprocess.check_output(['xdotool', 'search', '--name', '^Rill panel primary$'],
                                                env=client_env, text=True).splitlines()[0]
                strut = subprocess.check_output(['xprop', '-id', panel, '_NET_WM_STRUT_PARTIAL'],
                                                env=client_env, text=True)
                assert '= 0, 0, 26, 0,' in strut, strut
                # A launcher present only in Desktop must open on double click.
                time.sleep(.5)
                subprocess.run(['xdotool', 'mousemove', '65', '360', 'click', '--repeat', '2',
                                '--delay', '150', '1'], env=client_env, check=True)
                wait_for(desktop_probe.exists, 'desktop double click did not launch its entry', seconds=8)
                time.sleep(.6)
                # Move the icon and verify persistence before forcing a desktop crash.
                subprocess.run(['xdotool', 'mousemove', '65', '360', 'mousedown', '1',
                                'sleep', '.2', 'mousemove', '400', '410', 'sleep', '.2',
                                'mouseup', '1'], env=client_env, check=True)
                layout = Path(client_env['XDG_CONFIG_HOME']) / 'rill/desktop-layout'
                wait_for(lambda: layout.exists() and 'probe' in layout.read_text(),
                         'dragged icon position was not saved', seconds=8)
                desktop_probe.unlink()
                print('Desktop entry double click and persistent dragging passed')
                # An independent Settings process writes preferences; the dock
                # must reload them without overwriting desktop preferences.
                settings = subprocess.Popen([str(binary), '--settings'], env=client_env,
                                            stdout=log, stderr=log)
                settings_window = wait_for(lambda: subprocess.run(
                    ['xdotool', 'search', '--name', '^Rill Settings$'], env=client_env,
                    text=True, capture_output=True).stdout.strip(), 'settings window did not map')
                subprocess.run(['xdotool', 'windowactivate', '--sync', settings_window,
                                'mousemove', '--window', settings_window, '322', '364', 'click', '1'],
                               env=client_env, check=True)
                def panel_resized():
                    return '= 0, 0, 27, 0,' in subprocess.check_output(
                        ['xprop', '-id', panel, '_NET_WM_STRUT_PARTIAL'], env=client_env, text=True)
                wait_for(panel_resized, 'panel did not reload its height from Settings', seconds=10)
                if os.environ.get('RILL_TEST_SETTINGS_SCREENSHOT'):
                    subprocess.run(['xwd', '-silent', '-id', settings_window, '-out', str(work / 'settings.xwd')],
                                   env=client_env, check=True)
                    subprocess.run(['convert', str(work / 'settings.xwd'), os.environ['RILL_TEST_SETTINGS_SCREENSHOT']],
                                   check=True)
                settings.terminate()
                settings.wait(timeout=5)
                print('Standalone settings and live panel resizing passed')
                # Shift-drag moves a panel item without triggering its action.
                subprocess.run(['xdotool', 'mousemove', '1115', '13', 'keydown', 'Shift_L',
                                'sleep', '.2', 'mousedown', '1', 'sleep', '.2',
                                'mousemove', '1185', '13', 'sleep', '.2', 'mouseup', '1',
                                'keyup', 'Shift_L'], env=client_env, check=True)
                panel_config = Path(client_env['XDG_CONFIG_HOME']) / 'rill/panel'
                def reordered():
                    if not panel_config.exists():
                        return False
                    text = panel_config.read_text()
                    return text.index('\nvolume ') < text.index('\nclock ')
                wait_for(reordered, 'shift-drag did not reorder panel items', seconds=8)
                print('Persistent panel item drag reordering passed')
                subprocess.run(['xrandr', '--setmonitor', 'RillTestRight',
                                '640/170x800/210+640+0', 'none'], env=client_env, check=True)
                extra_panel = subprocess.Popen([str(binary), '--panel', 'secondary',
                                                '--panel-output', 'RillTestRight'],
                                               env=client_env, stdout=log, stderr=log)
                def second_panel_position():
                    window = subprocess.run(['xdotool', 'search', '--name', '^Rill panel secondary$'],
                                            env=client_env, capture_output=True, text=True).stdout.strip()
                    if not window:
                        return False
                    geometry = subprocess.check_output(['xdotool', 'getwindowgeometry', '--shell', window],
                                                       env=client_env, text=True)
                    return 'X=640\nY=0\nWIDTH=640\nHEIGHT=800' in geometry
                wait_for(second_panel_position, 'second panel did not select the requested monitor', seconds=10)
                extra_panel.terminate()
                extra_panel.wait(timeout=5)
                subprocess.run(['xrandr', '--delmonitor', 'RillTestRight'], env=client_env, check=True)
                print('Named monitor panel placement passed')
                subprocess.run(['xdotool', 'mousemove', '600', '600', 'key', 'Escape'],
                               env=client_env, check=True)
                time.sleep(.3)
                assert any(process_id == pid for process_id, _ in clients()), 'Escape terminated the desktop'
            if os.environ.get('RILL_TEST_NESTED'):
                outer = subprocess.check_output(['xwininfo', '-root', '-tree'], env=env, text=True)
                assert 'Rill desktop' in outer and 'xfce4-panel' not in outer, outer
                window = subprocess.check_output(['xdotool', 'search', '--name', '^Rill desktop$'], env=env, text=True).splitlines()[0]
                subprocess.run(['xdotool', 'windowsize', window, '1000', '700'], env=env, check=True)
                desktop = re.search(r'^\s*(0x[0-9a-f]+) "Rill":', tree, re.MULTILINE).group(1)
                def resized():
                    return 'WIDTH=1000\nHEIGHT=700' in subprocess.check_output(['xdotool', 'getwindowgeometry', '--shell', desktop], env=client_env, text=True)
                wait_for(resized, 'Rill did not follow nested server resize')
                subprocess.Popen(['xmessage', 'Application inside nested Rill'], env=client_env, stdout=log, stderr=log)
                wait_for(lambda: 'xmessage' in subprocess.check_output(['xwininfo', '-root', '-tree'], env=client_env, text=True), 'application did not launch inside nested display')
            if plugins:
                expected = {'lib' + plugin['module'] + '.so' for plugin in plugins}
                try:
                    wait_for(lambda: expected <= loaded_modules(ready), 'plugin modules did not all load', seconds=30)
                finally:
                    loaded = loaded_modules(ready)
                    report = [dict(plugin, loaded='lib' + plugin['module'] + '.so' in loaded) for plugin in plugins]
                    print(json.dumps(report, indent=2))
                    if os.environ.get('RILL_PLUGIN_REPORT'):
                        Path(os.environ['RILL_PLUGIN_REPORT']).write_text(json.dumps(report, indent=2) + '\n')
                print(f"Loaded all {len(plugins)} installed Xfce plugin modules")
                for panel in range(1, (len(plugins) + 9) // 10 + 1):
                    for name, value in [('size', '48'), ('mode', '1')]:
                        subprocess.run(['xfconf-query', '-c', 'xfce4-panel', '-p',
                                        f'/panels/panel-{panel}/{name}', '-n', '-t', 'uint', '-s', value],
                                       env=client_env, check=True, timeout=10, stdout=log, stderr=log)
                time.sleep(1)
                assert expected <= loaded_modules(ready), 'a plugin unloaded after resize/orientation'
                subprocess.run(['xfce4-panel', '--restart'], env=client_env, check=True,
                               timeout=15, stdout=log, stderr=log)
                time.sleep(2)
                try:
                    wait_for(lambda: expected <= loaded_modules(ready), 'plugins did not reload after panel restart')
                except AssertionError:
                    print('Missing after restart:', sorted(expected - loaded_modules(ready)))
                    raise
                print('Plugin resize, orientation and panel restart passed')
            if os.environ.get('RILL_TEST_REAL_APPS'):
                subprocess.Popen(['xterm', '-title', 'Rill terminal test', '-geometry', '60x16+80+100'],
                                 env=client_env, stdout=log, stderr=log)
                subprocess.Popen(['mousepad', '--disable-server'], env=client_env, stdout=log, stderr=log)
                subprocess.Popen(['Thunar', str(work)], env=client_env, stdout=log, stderr=log)
                def apps_ready():
                    windows = subprocess.check_output(['xwininfo', '-root', '-tree'], env=client_env, text=True)
                    return 'Rill terminal test' in windows and 'Mousepad' in windows and 'Thunar' in windows
                wait_for(apps_ready, 'real test applications did not map')
                # Titles can exist before MapRequest; let all three startup sequences finish.
                time.sleep(2)
                terminal = subprocess.check_output(['xdotool', 'search', '--name', '^Rill terminal test$'], env=client_env, text=True).splitlines()[0]
                subprocess.run(['xdotool', 'windowactivate', '--sync', terminal], env=client_env, check=True, timeout=10)
                # Type into a real terminal and verify that native keyboard delivery reaches its shell.
                probe = work / 'typed-command'
                subprocess.run(['xdotool', 'type', '--clearmodifiers', '--delay', '2', 'printf success > ' + str(probe)], env=client_env, check=True, timeout=10)
                subprocess.run(['xdotool', 'key', 'Return'], env=client_env, check=True, timeout=10)
                wait_for(probe.exists, 'typed terminal command did not execute')
                assert probe.read_text() == 'success'
                if os.environ.get('RILL_WM_SCREENSHOT'):
                    subprocess.run(['xwd', '-root', '-silent', '-out', str(work / 'desktop.xwd')], env=client_env, check=True)
                    subprocess.run(['convert', str(work / 'desktop.xwd'), os.environ['RILL_WM_SCREENSHOT']], check=True)
                print('Terminal, editor and file-manager startup plus native keyboard input passed')
            # An abruptly terminated desktop must be restarted by the real session manager.
            ready.unlink()
            os.kill(pid, signal.SIGKILL)
            wait_for(lambda: ready.exists() and any(p != pid for p, _ in clients()),
                     'session manager did not restart Rill')
            pid, client_env = clients()[0]
            if native:
                for app_pid in app_log.read_text().splitlines():
                    os.kill(int(app_pid), 0)  # desktop recovery must preserve launched applications
                time.sleep(.5)
                subprocess.run(['xdotool', 'mousemove', '400', '410', 'click', '--repeat', '2',
                                '--delay', '150', '1'], env=client_env, check=True)
                wait_for(desktop_probe.exists, 'desktop icon position was not restored', seconds=8)
                fd = os.open(client_env['RILL_SESSION_CONTROL'], os.O_WRONLY | os.O_NONBLOCK)
                os.write(fd, b'logout\n')
                os.close(fd)
            else:
                subprocess.run(['xfce4-session-logout', '--logout', '--fast'], env=client_env,
                               check=True, timeout=15, stdout=log, stderr=log)
            process.wait(timeout=30)
            wait_for(lambda: not clients(), 'Rill survived session logout', seconds=10)
            if native:
                for app_pid in app_log.read_text().splitlines():
                    assert not Path('/proc', app_pid).exists(), 'application survived session logout'
                print('Launched applications survive desktop crashes and exit on logout')
            print('Rill session startup, crash restart and logout passed')
        except BaseException:
            if 'client_env' in locals():
                try:
                    subprocess.run(['xprop','-root','_NET_ACTIVE_WINDOW','_NET_CLIENT_LIST'],env=client_env,stdout=log,stderr=log,timeout=3)
                    for item in Path('/proc').iterdir():
                        if not item.name.isdigit(): continue
                        try:
                            if (item / 'exe').resolve() == binary.with_name('rill-wm') and str(ready).encode() in (item / 'environ').read_bytes():
                                log.write('WM status: ' + (item / 'stat').read_text() + '\n')
                                log.write('WM wait: ' + (item / 'wchan').read_text() + '\n')
                        except OSError: pass
                except subprocess.TimeoutExpired:
                    log.write('X server did not answer diagnostic query\n')
            log.flush()
            print((work / 'session.log').read_text())
            for path in list(work.rglob('desktop-layout')) + list(work.rglob('panel')):
                print(str(path), path.read_text())
            raise
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            for pid, _ in clients():
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            # D-Bus-activated helpers may have detached from the session group.
            # Stop only processes carrying this test's unique environment marker.
            marker = b'RILL_TEST_READY_FILE=' + str(ready).encode()
            def remaining_helpers():
                found = []
                for item in Path('/proc').iterdir():
                    if not item.name.isdigit():
                        continue
                    try:
                        if marker in (item / 'environ').read_bytes().split(b'\0'):
                            found.append(int(item.name))
                    except OSError:
                        pass
                return found
            for sig in (signal.SIGTERM, signal.SIGKILL):
                for helper in remaining_helpers():
                    try:
                        os.kill(helper, sig)
                    except ProcessLookupError:
                        pass
                deadline = time.monotonic() + 2
                while remaining_helpers() and time.monotonic() < deadline:
                    time.sleep(.1)
