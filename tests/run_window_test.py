"""Exercise the real Run window and its private launch fixture on Xvfb."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def command(*args):
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def wait_for(predicate, process):
    for _ in range(100):
        value = predicate()
        if value:
            return value
        if process.poll() is not None:
            raise AssertionError(f"Run exited early: {process.communicate()}")
        time.sleep(0.05)
    raise AssertionError("Run window did not become ready")


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
                def window_id():
                    result = subprocess.run(["xdotool", "search", "--name", "Kryon Ziran"],
                                            text=True, capture_output=True)
                    return result.stdout.splitlines()[0] if result.returncode == 0 else None
                window = wait_for(window_id, process)
                command("xdotool", "windowfocus", window)
                time.sleep(0.2)
                if mode == "enter":
                    command("xdotool", "type", "--clearmodifiers", "--delay", "70", "run")
                    command("xdotool", "key", "Return")
                elif mode == "history":
                    marker.unlink()
                    command("xdotool", "mousemove", "--window", window, "80", "95")
                    command("xdotool", "mousedown", "1")
                    time.sleep(0.1)
                    command("xdotool", "mouseup", "1")
                else:
                    command("xdotool", "key", "Escape")
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
