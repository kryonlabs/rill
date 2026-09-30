"""Desktop panel interactions in a supervised window on a private display."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
from window_input import wait_for, window_for, send_input, owns_window

ROOT = Path(__file__).resolve().parents[1]


def main():
    assert os.environ.get("RILL_PRIVATE_XVFB") == "1"
    assert int(os.environ["DISPLAY"].split(".")[0][1:]) >= 100
    assert "WAYLAND_DISPLAY" not in os.environ
    with tempfile.TemporaryDirectory(prefix="rill-desktop-window-") as directory:
        fixture = Path(directory)
        applications = fixture / "applications"
        applications.mkdir()
        desktop = fixture / "Desktop"
        desktop.mkdir()
        (desktop / "Folder").mkdir()
        (desktop / "Note.txt").write_text("keep this content")
        marker = fixture / "launched"
        (applications / "desktop-probe.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Desktop Probe\n"
            f'Exec=/bin/sh -c "printf launched > {marker}"\n'
            "Categories=Utility;\nX-Rill-Favorite=true\n"
        )
        env = os.environ.copy()
        env.update(RILL_APPLICATION_DIRS=str(applications),
                   RILL_DESKTOP_DIR=str(desktop),
                   XDG_CONFIG_HOME=str(fixture / "config"), HOME=str(fixture))
        process = subprocess.Popen([str(ROOT / "build/rill-desktop")], cwd=ROOT,
                                   env=env, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        try:
            window = wait_for(lambda: window_for(process), process)
            def input_command(*args):
                return send_input(window, process, *args)
            def click(x, y, button="1"):
                input_command("mousemove", "--window", window, str(x), str(y))
                input_command("mousedown", button)
                time.sleep(0.1)
                input_command("mouseup", button)
                time.sleep(0.2)
            input_command("windowsize", window, "960", "600")
            input_command("windowfocus", window)
            time.sleep(0.4)
            geometry = input_command("getwindowgeometry", "--shell", window)
            assert "WIDTH=960\n" in geometry and "HEIGHT=600\n" in geometry
            # Exercise actual desktop context menus and foreground text input.
            click(430, 240, "3")
            click(470, 240 + 4 + 26 + 13)
            input_command("type", "--window", window, "--clearmodifiers",
                          "--delay", "40", "Recovered Folder")
            input_command("key", "--window", window, "Return")
            wait_for(lambda: (desktop / "Recovered Folder").is_dir(), process)
            click(65, 455, "3")
            click(100, 384 + 4 + 3 * 26 + 13)
            input_command("type", "--window", window, "--clearmodifiers",
                          "--delay", "40", "Renamed.txt")
            input_command("key", "--window", window, "Return")
            wait_for(lambda: (desktop / "Renamed.txt").exists(), process)
            assert not (desktop / "Note.txt").exists()
            assert (desktop / "Renamed.txt").read_text() == "keep this content"
            click(50, 15)
            input_command("type", "--window", window, "--clearmodifiers",
                          "--delay", "60", "desktop probe")
            input_command("key", "--window", window, "Return")
            wait_for(marker.exists, process)
            assert marker.read_text() == "launched"
            assert process.poll() is None, "Desktop exited after application launch"
            # The trailing group is anchored at x=622; clock starts at x=780.
            click(810, 15)
            input_command("key", "--window", window, "Page_Down", "Page_Up", "Home", "Escape")
            time.sleep(0.2)
            assert process.poll() is None, "Escape closed the desktop instead of its calendar"
            # Context menu row 7 chooses the bottom edge and persists it.
            click(50, 15, "3")
            click(80, 15 + 4 + 6 * 26 + 13)
            settings = fixture / "config/rill/settings"
            wait_for(lambda: settings.exists() and "panel-side = bottom" in settings.read_text(), process)
            assert "recents = desktop-probe" in settings.read_text()
            # Repeat the launcher through the relocated panel.
            marker.unlink()
            click(50, 575)
            input_command("type", "--window", window, "--clearmodifiers",
                          "--delay", "60", "desktop probe")
            input_command("key", "--window", window, "Return")
            wait_for(marker.exists, process)
            assert process.poll() is None
            # A separate Settings process shares preferences with the running
            # desktop. Its edge/clock changes must preserve launcher recents.
            preferences = subprocess.Popen([str(ROOT / "build/rill-settings")], cwd=ROOT,
                                           env=env, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True)
            try:
                preferences_window = wait_for(lambda: window_for(preferences), preferences)

                def preference_input(*args):
                    return send_input(preferences_window, preferences, *args)

                def preference_click(x, y):
                    preference_input("mousemove", "--window", preferences_window, str(x), str(y))
                    preference_input("mousedown", "1")
                    time.sleep(0.1)
                    preference_input("mouseup", "1")
                    time.sleep(0.2)

                preference_input("windowsize", preferences_window, "640", "480")
                preference_input("windowfocus", preferences_window)
                time.sleep(0.4)
                preference_click(75, 124)
                preference_click(300, 280)
                wait_for(lambda: "panel-side = top" in settings.read_text() and
                         "clock-format = %H:%M:%S" in settings.read_text(), preferences)
                assert "recents = desktop-probe" in settings.read_text()
                preference_click(595, 458)
                preferences_stdout, preferences_stderr = preferences.communicate(timeout=10)
                assert preferences.returncode == 0, (preferences_stdout, preferences_stderr)
            finally:
                if preferences.poll() is None:
                    preferences.terminate()
                    preferences.communicate(timeout=5)
            time.sleep(1.5)  # desktop refreshes external preferences once a second
            marker.unlink()
            input_command("windowfocus", window)
            click(50, 15)
            input_command("type", "--window", window, "--clearmodifiers",
                          "--delay", "60", "desktop probe")
            input_command("key", "--window", window, "Return")
            try:
                wait_for(marker.exists, process)
            except AssertionError:
                assert owns_window(window, process)
                subprocess.run(["xwd", "-silent", "-id", window, "-out",
                                str(ROOT / "build/rill-desktop-window.xwd")], check=True, env=env)
                print("settings:", settings.read_text(), flush=True)
                raise
            assert "panel-side = top" in settings.read_text()
            assert "clock-format = %H:%M:%S" in settings.read_text()
            assert "recents = desktop-probe" in settings.read_text()
        finally:
            if process.poll() is None:
                process.terminate()
            stdout, stderr = process.communicate(timeout=5)
            assert process.returncode in (0, -15), (stdout, stderr)
            assert "segmentation" not in stderr.lower(), (stdout, stderr)
    print("rill-desktop-window-test-ok: desktop folder/rename, launch, calendar, panel relocation, shared Settings reload")


if __name__ == "__main__":
    main()
