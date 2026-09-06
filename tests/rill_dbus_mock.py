#!/usr/bin/env python3
"""Combined D-Bus mock for the Linux launcher test.

On one private session bus this script hosts a mock StatusNotifierItem and a
mock notification client, so the C test can drive both the tray host and the
notification server without swapping bus addresses.

Sequence: register the item and wait for a StatusNotifierWatcher, then send a
notification and wait for the notification server, then write READY and serve
the ActionInvoked signal by sending a second notification.
"""
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

ITEM_PATH = "/org/ayatana/NotificationItem/mock"
PIXELS = b"\x20\x20\x40\xff" * 64  # 8x8 ARGB, little-endian


def call_with_retry(bus, name, path, interface, method, parameters,
                    reply_type, deadline=20.0):
    """Retry a bus call until the service appears (or the deadline passes)."""
    end = time.monotonic() + deadline
    while True:
        try:
            return bus.call_sync(name, path, interface, method, parameters,
                                 reply_type, Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error:
            if time.monotonic() >= end:
                raise
            time.sleep(0.1)


def main():
    if len(sys.argv) != 4:
        raise SystemExit("usage: rill_dbus_mock.py READY ACTIVATED ACTIONS")
    ready_path, activated_path, actions_path = sys.argv[1:4]

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
         <interface name='org.freedesktop.Notifications'>
          <method name='NotificationClosed'>
           <arg type='u' direction='in'/><arg type='u' direction='in'/>
          </method>
          <signal name='ActionInvoked'>
           <arg type='u'/><arg type='s'/>
          </signal>
         </interface>
        </node>"""
    )

    def on_method(connection, sender, path, interface, method, params,
                  invocation):
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

    def on_action_invoked(connection, sender, path, interface, signal, params):
        notification_id, action = params.unpack()
        with open(actions_path, "a") as log:
            log.write("%s %s\n" % (notification_id, action))
        connection.call_sync(
            "org.freedesktop.Notifications",
            "/org/freedesktop/Notifications",
            "org.freedesktop.Notifications",
            "Notify",
            GLib.Variant("(susssasa{sv}i)",
                         ("Rill mock", 0, "", "Rill note two",
                          "Banner body text", ["default", "Default"], {},
                          30000)),
            GLib.VariantType("(u)"),
            Gio.DBusCallFlags.NONE,
            -1,
            None,
        )

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    bus.register_object(ITEM_PATH, node.interfaces[0], on_method,
                        on_property, None)
    bus.register_object("/org/freedesktop/Notifications",
                        node.interfaces[1], on_method, None, None)
    bus.signal_subscribe(None, "org.freedesktop.Notifications",
                         "ActionInvoked", None, None,
                         Gio.DBusSignalFlags.NONE, on_action_invoked)

    call_with_retry(bus, "org.kde.StatusNotifierWatcher",
                    "/StatusNotifierWatcher",
                    "org.kde.StatusNotifierWatcher",
                    "RegisterStatusNotifierItem",
                    GLib.Variant("(s)", (ITEM_PATH,)), None)
    call_with_retry(bus, "org.freedesktop.Notifications",
                    "/org/freedesktop/Notifications",
                    "org.freedesktop.Notifications", "Notify",
                    GLib.Variant("(susssasa{sv}i)",
                                 ("Rill mock", 0, "", "Rill note one",
                                  "Banner body text", ["default", "Default"],
                                  {}, 30000)),
                    GLib.VariantType("(u)"))
    with open(ready_path, "w") as ready:
        ready.write("ready\n")
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
