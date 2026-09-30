"""Exercise Calendar input only on a private display and supervised window."""
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
    with tempfile.TemporaryDirectory(prefix="rill-calendar-window-") as directory:
        env = os.environ.copy()
        env.update(HOME=directory, XDG_CONFIG_HOME=str(Path(directory) / "config"),
                   RILL_APPLICATION_DIRS=directory)
        for action in ("escape", "close"):
            process = subprocess.Popen([str(ROOT / "build/rill-calendar")], cwd=ROOT,
                                       env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                window = wait_for(lambda: window_for(process), process)
                def input_command(*args):
                    return send_input(window, process, *args)
                input_command("windowsize", window, "480", "300")
                input_command("windowfocus", window)
                time.sleep(0.3)
                input_command("key", "--window", window, "Page_Down", "Page_Up", "Home")
                if action == "escape":
                    input_command("key", "--window", window, "Escape")
                else:
                    input_command("mousemove", "--window", window, "350", "274")
                    input_command("mousedown", "1")
                    time.sleep(0.1)
                    input_command("mouseup", "1")
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 0, (action, stdout, stderr)
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)
    print("rill-calendar-window-test-ok: navigation, Today, Escape, Close")


if __name__ == "__main__":
    main()
