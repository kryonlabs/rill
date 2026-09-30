"""Exercise the real Run window and its private launch fixture on Xvfb."""
import os
import ctypes
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


class ClientSpec(ctypes.Structure):
    _fields_ = [("client", ctypes.c_ulong), ("mask", ctypes.c_uint)]


def client_pid(window):
    # Ask the server for its client's PID; plan9port has no _NET_WM_PID.
    xlib = ctypes.CDLL("libX11.so.6")
    resource = ctypes.CDLL("libXRes.so.1")
    xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
    xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    resource.XResQueryClientIds.argtypes = [ctypes.c_void_p, ctypes.c_long,
                                           ctypes.POINTER(ClientSpec),
                                           ctypes.POINTER(ctypes.c_long),
                                           ctypes.POINTER(ctypes.c_void_p)]
    resource.XResQueryClientIds.restype = ctypes.c_int
    resource.XResGetClientPid.argtypes = [ctypes.c_void_p]
    resource.XResGetClientPid.restype = ctypes.c_int
    resource.XResClientIdsDestroy.argtypes = [ctypes.c_long, ctypes.c_void_p]
    resource.XResClientIdsDestroy.restype = None
    display = xlib.XOpenDisplay(None)
    if not display:
        return 0
    values = ctypes.c_void_p()
    count = ctypes.c_long()
    try:
        spec = ClientSpec(int(window), 2)  # XRES_CLIENT_ID_PID_MASK
        result = resource.XResQueryClientIds(display, 1, ctypes.byref(spec),
                                             ctypes.byref(count), ctypes.byref(values))
        if result != 0 or count.value != 1 or not values.value:
            return 0
        return resource.XResGetClientPid(values)
    finally:
        if values.value:
            resource.XResClientIdsDestroy(count, values)
        xlib.XCloseDisplay(display)


def owns_window(window, process):
    if process.poll() is not None:
        return False
    pid = client_pid(window)
    for _ in range(32):
        if pid == process.pid:
            return True
        if pid <= 1:
            return False
        try:
            status = Path(f"/proc/{pid}/status").read_text()
            pid = int(next(line.split()[1] for line in status.splitlines()
                           if line.startswith("PPid:")))
        except (OSError, StopIteration, ValueError):
            return False
    return False


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
                    if result.returncode == 0:
                        return next((window for window in result.stdout.splitlines()
                                     if owns_window(window, process)), None)
                    return None
                window = wait_for(window_id, process)
                unrelated = subprocess.Popen(["sleep", "5"], cwd=fixture)
                try:
                    assert not owns_window(window, unrelated), "An unrelated PID acquired window authority"
                finally:
                    unrelated.terminate()
                    unrelated.wait(timeout=5)

                def input_command(*args):
                    assert owns_window(window, process), "Run window owner changed"
                    if args[0] in ("mousedown", "mouseup"):
                        pointer = command("xdotool", "getmouselocation", "--shell")
                        pointer_id = next(line.split("=", 1)[1] for line in pointer.splitlines()
                                          if line.startswith("WINDOW="))
                        assert owns_window(pointer_id, process), "Pointer is over a foreign window"
                    return command("xdotool", *args)

                input_command("windowfocus", window)
                time.sleep(0.2)
                if mode == "enter":
                    input_command("type", "--window", window, "--clearmodifiers", "--delay", "70", "run")
                    input_command("key", "--window", window, "Return")
                elif mode == "history":
                    marker.unlink()
                    input_command("mousemove", "--window", window, "80", "95")
                    input_command("mousedown", "1")
                    time.sleep(0.1)
                    input_command("mouseup", "1")
                else:
                    input_command("key", "--window", window, "Escape")
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
