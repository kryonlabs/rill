"""Session process and autostart regressions. Never reads real autostart data."""
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

BINARY = Path('build/linux-x86_64/rill-sessiond').resolve()


def wait_for(predicate, message, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.025)
    raise AssertionError(message)


def keyfile_list(arguments):
    return ';'.join(str(arg).replace('\\', '\\\\').replace(';', '\\;') for arg in arguments) + ';'


class SessionLifecycleTest(unittest.TestCase):
    def setUp(self):
        self.work = tempfile.TemporaryDirectory(prefix='rill-lifecycle-')
        self.root = Path(self.work.name)
        self.env = dict(os.environ, HOME=str(self.root), XDG_CONFIG_HOME=str(self.root / 'config'),
                        XDG_CONFIG_DIRS=str(self.root / 'system'), XDG_RUNTIME_DIR=str(self.root),
                        XDG_CACHE_HOME=str(self.root / 'cache'), XDG_CURRENT_DESKTOP='Rill:XFCE',
                        DBUS_SESSION_BUS_ADDRESS='unix:path=' + str(self.root / 'no-bus'),
                        DBUS_SYSTEM_BUS_ADDRESS='unix:path=' + str(self.root / 'no-system-bus'))
        # This manager may send close requests at logout; it must never see,
        # let alone touch, the developer's real desktop display.
        for leaked in ('DISPLAY', 'WAYLAND_DISPLAY'):
            self.env.pop(leaked, None)
        self.manager = None
        self.log = (self.root / 'log').open('w+')
        self.probe = self.root / 'probe.py'
        self.probe.write_text('import os,sys,time,json\n'
                              'from pathlib import Path\n'
                              'with Path(sys.argv[1]).open("a") as f:\n'
                              ' f.write(json.dumps([os.getpid(), *sys.argv[2:]])+"\\n")\n'
                              'if "--wait" in sys.argv: time.sleep(60)\n')

    def tearDown(self):
        if self.manager is not None and self.manager.poll() is None:
            self.manager.terminate()
            try:
                self.manager.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.manager.kill()
                self.manager.wait()
        self.log.close()
        self.work.cleanup()

    def start(self, options=(), body=None):
        if body is None:
            body = [sys.executable, str(self.probe), str(self.root / 'body'), '--wait']
        self.manager = subprocess.Popen([str(BINARY), *options, '--', *body], env=self.env,
                                        stdout=self.log, stderr=self.log)
        def ready():
            self.log.flush()
            content = (self.root / 'log').read_text()
            for line in content.splitlines():
                if line.startswith('control: '):
                    path = Path(line[9:])
                    if path.exists():
                        return path
            if self.manager.poll() is not None:
                raise AssertionError(content)
        self.control = wait_for(ready, 'session did not publish control endpoint')
        wait_for(lambda: (self.root / 'body').exists(), 'session body did not start')

    def logout(self):
        with self.control.open('w') as fifo:
            fifo.write('logout\n')
        self.assertEqual(self.manager.wait(timeout=5), 0, (self.root / 'log').read_text())
        self.assertFalse(self.control.exists())

    def entry(self, directory, name, arguments, extra=''):
        directory.mkdir(parents=True, exist_ok=True)
        # Desktop entry quoting, not shell syntax.
        command = ' '.join('"' + str(arg).replace('\\', '\\\\\\\\').replace('"', '\\\\"') + '"'
                           for arg in arguments)
        (directory / name).write_text('[Desktop Entry]\nType=Application\nName=Probe\n'
                                       'Exec=' + command + '\n' + extra)

    def test_autostart_precedence_and_exact_desktop_matching(self):
        user = self.root / 'config/autostart'
        system = self.root / 'system/autostart'
        def command(name):
            return [sys.executable, self.probe, self.root / name]
        self.entry(system, 'hidden.desktop', command('hidden'))
        self.entry(user, 'hidden.desktop', command('hidden'), 'Hidden=true\nOnlyShowIn=XFCE;\n')
        self.entry(system, 'override.desktop', command('wrong'))
        self.entry(user, 'override.desktop', command('right'))
        self.entry(user, 'substring.desktop', command('substring'), 'OnlyShowIn=NOTXFCE;\n')
        self.entry(user, 'tryexec.desktop', command('absolute'), 'TryExec=' + sys.executable + '\n')
        self.entry(user, 'missing.desktop', command('missing'), 'TryExec=/not/installed\n')
        (user / 'unquoted.desktop').write_text('[Desktop Entry]\nType=Application\nName=Script\nExec=' +
            ' '.join(map(str, command('unquoted'))) + '\n')
        self.start()
        wait_for(lambda: (self.root / 'right').exists() and (self.root / 'absolute').exists(),
                 'valid autostarts did not run')
        wait_for(lambda: (self.root / 'unquoted').exists(), 'another script using the desktop interpreter was suppressed')
        self.logout()
        for name in ('hidden', 'wrong', 'substring', 'missing'):
            self.assertFalse((self.root / name).exists(), name)

    def test_exec_arguments_are_not_shell_commands(self):
        user = self.root / 'config/autostart'
        self.entry(user, 'literal.desktop', [sys.executable, self.probe,
                   self.root / 'literal', 'two words', 'semi;colon', '100%%'])
        self.start()
        wait_for(lambda: (self.root / 'literal').exists(), 'literal autostart did not run')
        self.logout()
        values = json.loads((self.root / 'literal').read_text())
        self.assertEqual(values[1:], ['two words', 'semi;colon', '100%'])

    def test_services_restart_and_logout_does_not_respawn(self):
        services = self.root / 'services.ini'
        argv = [sys.executable, self.probe, self.root / 'service', '--wait']
        services.write_text('[Window manager]\nCommand=' + keyfile_list(argv) +
                            '\nRestart=true\nRequired=true\n')
        self.start(['--no-default-autostart', '--restart-body', '--services', str(services)])
        wait_for(lambda: (self.root / 'service').exists(), 'service not started')
        first_pid = json.loads((self.root / 'service').read_text().splitlines()[0])[0]
        os.kill(first_pid, signal.SIGKILL)
        wait_for(lambda: len((self.root / 'service').read_text().splitlines()) == 2,
                 'service did not restart')
        desktop_pid = json.loads((self.root / 'body').read_text().splitlines()[0])[0]
        os.kill(desktop_pid, signal.SIGKILL)
        wait_for(lambda: len((self.root / 'body').read_text().splitlines()) == 2,
                 'desktop did not restart')
        self.logout()
        time.sleep(.2)
        self.assertEqual(len((self.root / 'body').read_text().splitlines()), 2)
        self.assertEqual(len((self.root / 'service').read_text().splitlines()), 2)
        for path in ('service', 'body'):
            pid = json.loads((self.root / path).read_text().splitlines()[-1])[0]
            with self.assertRaises(ProcessLookupError):
                os.kill(pid, 0)

    def test_logout_cleans_children_after_the_service_leader_exits(self):
        service = self.root / 'parent.py'
        child = self.root / 'child.py'
        marker = self.root / 'child-pid'
        child.write_text('import os,signal,time\nfrom pathlib import Path\n'
                         'signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                         'Path(' + repr(str(marker)) + ').write_text(str(os.getpid()))\n'
                         'time.sleep(60)\n')
        service.write_text('import subprocess,sys,time\n'
                           'subprocess.Popen([sys.executable, ' + repr(str(child)) + '])\n'
                           'time.sleep(60)\n')
        services = self.root / 'services.ini'
        services.write_text('[Service]\nCommand=' + keyfile_list([sys.executable, service]) +
                            '\nRestart=true\n')
        self.start(['--no-default-autostart', '--services', str(services)])
        wait_for(marker.exists, 'service child did not start')
        pid = int(marker.read_text())
        self.logout()
        def stopped():
            try:
                return Path('/proc', str(pid), 'stat').read_text().split()[2] == 'Z'
            except FileNotFoundError:
                return True
        wait_for(stopped, 'service child survived logout')

    def test_launches_belong_to_session_instead_of_desktop(self):
        self.start(['--no-default-autostart', '--restart-body'])
        arguments = [sys.executable, str(self.probe), str(self.root / 'application'), '--wait', 'two words']
        with self.control.open('w') as fifo:
            fifo.write('launch ' + json.dumps(arguments) + '\n')
        wait_for(lambda: (self.root / 'application').exists(), 'session did not launch application')
        app = json.loads((self.root / 'application').read_text())
        self.assertEqual(app[1:], ['--wait', 'two words'])
        desktop_pid = json.loads((self.root / 'body').read_text().splitlines()[0])[0]
        os.kill(desktop_pid, signal.SIGKILL)
        wait_for(lambda: len((self.root / 'body').read_text().splitlines()) == 2,
                 'desktop did not restart')
        os.kill(app[0], 0)
        self.logout()
        with self.assertRaises(ProcessLookupError):
            os.kill(app[0], 0)

    def test_saved_commands_restore_without_shell_evaluation(self):
        saved = self.root / 'saved-session.ini'
        saved.write_text('[Client 0]\nCommand=' + keyfile_list(
            [sys.executable, self.probe, self.root / 'restored', 'saved argument']) + '\n')
        self.start(['--no-default-autostart', '--state-file', str(saved), '--restore'])
        wait_for(lambda: (self.root / 'restored').exists(), 'saved command was not restored')
        self.assertEqual(json.loads((self.root / 'restored').read_text())[1:], ['saved argument'])
        self.logout()

    def test_private_endpoints_do_not_replace_other_sessions(self):
        self.start(['--no-default-autostart'])
        mode = self.control.parent.stat().st_mode & 0o777
        self.assertEqual(mode, 0o700)
        self.assertEqual((self.control.parent / 'ICEauthority').stat().st_mode & 0o777, 0o600)
        self.logout()


if __name__ == '__main__':
    unittest.main()
