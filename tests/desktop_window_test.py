"""Desktop panel interactions in a supervised window on a private display."""
import os
from pathlib import Path
import subprocess
import tempfile
import time
from window_input import wait_for, window_for, send_input

ROOT = Path(__file__).resolve().parents[1]


def main():
    assert os.environ.get("RILL_PRIVATE_XVFB") == "1"
    assert int(os.environ["DISPLAY"].split(".")[0][1:]) >= 100
    assert "WAYLAND_DISPLAY" not in os.environ
    with tempfile.TemporaryDirectory(prefix="rill-desktop-window-") as directory:
        fixture = Path(directory)
        applications = fixture / "applications"
        applications.mkdir()
        marker = fixture / "launched"
        (applications / "desktop-probe.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Desktop Probe\n"
            f'Exec=/bin/sh -c "printf launched > {marker}"\n'
            "Categories=Utility;\nX-Rill-Favorite=true\n"
        )
        env = os.environ.copy()
        env.update(RILL_APPLICATION_DIRS=str(applications),
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
        finally:
            if process.poll() is None:
                process.terminate()
            stdout, stderr = process.communicate(timeout=5)
            assert "segmentation" not in stderr.lower(), (stdout, stderr)
    print("rill-desktop-window-test-ok: launch, calendar, persistent panel relocation")


if __name__ == "__main__":
    main()
