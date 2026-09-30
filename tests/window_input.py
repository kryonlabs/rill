"""Input authority for supervised applications on a private Xvfb display."""
import ctypes
from pathlib import Path
import subprocess
import time

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
            raise AssertionError(f"Application exited early: {process.communicate()}")
        time.sleep(0.05)
    raise AssertionError("Application window did not become ready")



def window_for(process):
    result = subprocess.run(["xdotool", "search", "--name", "Kryon Ziran"],
                            text=True, capture_output=True)
    if result.returncode == 0:
        return next((window for window in result.stdout.splitlines()
                     if owns_window(window, process)), None)
    return None


def send_input(window, process, *args):
    assert owns_window(window, process), f"Application window owner changed (exit={process.poll()})"
    if args[0] in ("mousedown", "mouseup"):
        pointer = command("xdotool", "getmouselocation", "--shell")
        pointer_id = next(line.split("=", 1)[1] for line in pointer.splitlines()
                          if line.startswith("WINDOW="))
        assert owns_window(pointer_id, process), "Pointer is over a foreign window"
    return command("xdotool", *args)
