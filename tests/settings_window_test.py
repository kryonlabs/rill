"""Settings and About input on a private display with supervised windows."""
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
    with tempfile.TemporaryDirectory(prefix="rill-settings-window-") as directory:
        fixture = Path(directory)
        env = os.environ.copy()
        env.update(HOME=directory, XDG_CONFIG_HOME=str(fixture / "config"),
                   RILL_APPLICATION_DIRS=directory)
        for application, close in (("settings", "escape"), ("settings", "button"), ("about", "escape")):
            process = subprocess.Popen([str(ROOT / f"build/rill-{application}")], cwd=ROOT,
                                       env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                window = wait_for(lambda: window_for(process), process)

                def input_command(*args):
                    return send_input(window, process, *args)

                def click(x, y):
                    input_command("mousemove", "--window", window, str(x), str(y))
                    input_command("mousedown", "1")
                    time.sleep(0.1)
                    input_command("mouseup", "1")

                input_command("windowsize", window, "480", "300")
                input_command("windowfocus", window)
                time.sleep(0.4)
                if application == "settings":
                    # Save a desktop preference through the actual Kryon UI.
                    click(170, 124)
                    settings = fixture / "config/rill/settings"
                    try:
                        wait_for(lambda: settings.exists() and "panel-side = bottom" in settings.read_text(), process)
                    except AssertionError:
                        assert owns_window(window, process)
                        capture = ROOT / "build/rill-settings-window.xwd"
                        subprocess.run(["xwd", "-silent", "-id", window, "-out", str(capture)], check=True, env=env)
                        print("settings:", settings.read_text() if settings.exists() else "missing", flush=True)
                        raise
                    # Change tabs and scroll through the complete shortcut list.
                    for x in (170, 286, 400):
                        click(x, 24)
                        time.sleep(0.2)
                    input_command("mousemove", "--window", window, "350", "180")
                    for _ in range(20):
                        input_command("click", "5")
                    time.sleep(0.3)
                    assert process.poll() is None
                if close == "escape":
                    input_command("keydown", "--window", window, "Escape")
                else:
                    click(435, 278)
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 0, (application, close, stdout, stderr)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)
    print("rill-settings-window-test-ok: preferences, tabs, scrolling, Escape, Close, About")


if __name__ == "__main__":
    main()
