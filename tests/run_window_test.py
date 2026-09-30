"""Exercise the real Run window and its private launch fixture on Xvfb."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


from window_input import owns_window, wait_for, window_for, send_input

def main():
    # The caller scrubs inherited displays before starting its own Xvfb.
    assert os.environ.get("RILL_PRIVATE_XVFB") == "1"
    assert int(os.environ["DISPLAY"].split(".")[0][1:]) >= 100
    assert "WAYLAND_DISPLAY" not in os.environ
    with tempfile.TemporaryDirectory(prefix="rill-run-window-") as directory:
        fixture = Path(directory)
        applications = fixture / "applications"
        applications.mkdir()
        marker = fixture / "launched"
        (applications / "run-probe.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Run Probe\n"
            f'Exec=/bin/sh -c "printf launched > {marker}"\n'
            "Comment=Private Run launch fixture\nCategories=Utility;\n"
        )
        env = os.environ.copy()
        env.update(RILL_APPLICATION_DIRS=str(applications),
                   XDG_CONFIG_HOME=str(fixture / "config"),
                   HOME=str(fixture), winsize="480x300")
        for mode in ("enter", "history", "escape"):
            process = subprocess.Popen([str(ROOT / "build/rill-run")], cwd=ROOT,
                                       env=env, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            try:
                window = wait_for(lambda: window_for(process), process)
                unrelated = subprocess.Popen(["sleep", "5"], cwd=fixture)
                try:
                    assert not owns_window(window, unrelated), "An unrelated PID acquired window authority"
                finally:
                    unrelated.terminate()
                    unrelated.wait(timeout=5)

                def input_command(*args):
                    return send_input(window, process, *args)

                input_command("windowsize", window, "480", "300")
                input_command("windowfocus", window)
                time.sleep(0.3)
                geometry = input_command("getwindowgeometry", "--shell", window)
                assert "WIDTH=480\n" in geometry and "HEIGHT=300\n" in geometry, geometry
                if mode == "enter":
                    input_command("type", "--window", window, "--clearmodifiers", "--delay", "70", "run")
                    input_command("keydown", "--window", window, "Return")
                elif mode == "history":
                    marker.unlink()
                    input_command("mousemove", "--window", window, "80", "95")
                    input_command("mousedown", "1")
                    time.sleep(0.1)
                    input_command("mouseup", "1")
                else:
                    input_command("keydown", "--window", window, "Escape")
                stdout, stderr = process.communicate(timeout=10)
                assert process.returncode == 0, (mode, stdout, stderr)
                if mode != "escape":
                    for _ in range(100):
                        if marker.exists():
                            break
                        time.sleep(0.02)
                    assert marker.read_text() == "launched", mode
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.communicate(timeout=5)
        history = fixture / "config/rill/settings"
        assert "run-history = Run Probe" in history.read_text()
    print("rill-run-window-test-ok: Enter, application history, Escape")


if __name__ == "__main__":
    main()
