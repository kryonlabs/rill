#!/usr/bin/env python3
"""Mock StatusNotifierItem used by the Linux launcher test.

Registers an item with the first StatusNotifierWatcher on the session bus,
serves Title/Category/IconPixmap properties and records Activate/
SecondaryActivate calls to a file.
"""
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

ITEM_PATH = "/org/ayatana/NotificationItem/mock"
PIXELS = b"\x20\x20\x40\xff" * 64  # 8x8 ARGB, little-endian


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: rill_sni_mock.py READY ACTIVATED")
    ready_path, activated_path = sys.argv[1], sys.argv[2]

    node = Gio.DBusNodeInfo.new_for_xml(
        """
        <node>
         <interface name='org.kde.StatusNotifierItem'>
          <method name='Activate'>
           <arg type='i' direction='in'/><arg type='i' direction='in'/>
          </method>
          <method name='SecondaryActivate'>
           <arg type='i' direction='in'/><arg type='i' direction='in'/>
          </method>
          <property name='Title' type='s' access='read'/>
          <property name='Category' type='s' access='read'/>
          <property name='IconPixmap' type='(iiay)' access='read'/>
         </interface>
        </node>"""
    )

    def on_method(connection, sender, path, interface, method, params, invocation):
        with open(activated_path, "a") as log:
            log.write(method + "\n")
        invocation.return_value(None)

    def on_property(connection, sender, path, interface, name):
        if name == "Title":
            return GLib.Variant("s", "Rill tray mock")
        if name == "Category":
            return GLib.Variant("s", "ApplicationStatus")
        if name == "IconPixmap":
            return GLib.Variant("(iiay)", (8, 8, list(PIXELS)))
        return None

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    bus.register_object(
        ITEM_PATH, node.interfaces[0], on_method, on_property, None
    )
    # The host acquires the watcher name asynchronously; wait for it.
    import time

    for _ in range(100):
        try:
            bus.call_sync(
                "org.kde.StatusNotifierWatcher",
                "/StatusNotifierWatcher",
                "org.kde.StatusNotifierWatcher",
                "RegisterStatusNotifierItem",
                GLib.Variant("(s)", (ITEM_PATH,)),
                None,
                Gio.DBusCallFlags.NONE,
                -1,
                None,
            )
            break
        except GLib.Error:
            time.sleep(0.1)
    else:
        raise SystemExit("no StatusNotifierWatcher appeared")
    with open(ready_path, "w") as ready:
        ready.write("ready\n")
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
