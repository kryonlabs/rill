# X11 sessions

`rill-session` defaults to Rill's session manager, window manager, desktop and
panel. Select **Rill (X11)** at login. `rill-session --mode xfce` and the separate
**Rill (X11, Xfce compatibility)** entry keep the real Xfce session and panel
available for unchanged GTK plugins.

Build with `make`; `make install PREFIX=/usr DESTDIR=/path/to/staging` packages
both session entries, binaries, launchers and the Settings application. Python 3,
D-Bus, X11, SM/ICE, GTK/GIO and the plan9port runtime are required. Native mode
does not require an Xfce executable. Optional service providers are still needed
for device settings, power policy, authentication and screen locking.

```sh
scripts/rill-session --check       # report required components and service providers
scripts/rill-session --prepare     # prepare configuration without starting a session
scripts/rill-window --size 1280x800 # isolated Xephyr desktop
build/linux-x86_64/rill --settings
```

The check reports installed/configured commands; it does not certify successful
hardware operations. It does not start providers or change the current desktop.

## Configuration and migration

With an original configuration root of `~/.config`, native mode uses
`~/.config/rill/desktop` and compatibility mode uses `~/.config/rill/session`.
The original root remains in `XDG_CONFIG_DIRS`. Xfce preferences, existing Rill
settings/panel layouts, GTK preferences, MIME defaults and localized user folders
are copied on first use; subsequent edits in the private profile survive login.
Other existing application configuration entries are linked into the native
profile, so applications retain their preferences and edits remain available
outside Rill. Desktop and autostart overrides remain private. Rill settings live at
`~/.config/rill/desktop/rill/settings` inside a native session.

The native panel reads Rill's panel format. Existing Xfce panel layouts and GTK
plugin settings are preserved for **compatibility mode**; they are not translated
into equivalent native Rill widgets. Theme/font import is separate from plugin
layout migration.

Desktop icon positions are stored in `rill/desktop-layout` within the active
profile. The desktop rescans its XDG desktop folder every two seconds and on F5.
Drag icons to place them, use F2 to rename or Delete to request moving an item
to Trash, and right-click for file/folder actions. Trash requires confirmation.
Control-click toggles selection, Shift-click selects a range, dragging empty
space selects a rectangle, and Ctrl+A selects all icons. Drag selected icons as
a group. Ctrl+C/Ctrl+X copies/cuts files; Ctrl+V pastes into a selected folder or
the desktop. Ctrl+Shift+N creates a folder. The context menu exposes these actions.

File transfers run in the background with progress and Cancel. Recursive copies
preserve symbolic links and basic metadata, refuse collisions and stage each
item before publishing it. Cancel removes that transfer's staging data. Items
already completed remain in place; there is no batch rollback or undo. A desktop
crash can leave hidden staging directories, and a cross-filesystem move whose
source cleanup fails can leave a completed destination plus remaining source
data. The dialog reports the failure and completed count. The installed file
manager still handles browsing, external drag/drop and advanced media/Trash
management.

## Panels

A native panel is a separate X11 dock with a reserved work area. Its window shape
includes open menus and leaves the rest of the desktop available to applications.
Height, top/bottom placement and autohide are persisted. Hold **Shift** while
dragging an item onto another item to reorder it. Right-click retains item
properties and move/remove commands. Settings changes propagate to the desktop
and primary panel without restarting them; concurrent saves merge changed keys.

For multiple panels, put `panels.json` in the **original** `~/.config/rill`:

```json
[
  {"id": "primary"},
  {"id": "secondary", "output": "DP-2"}
]
```

IDs must be unique and contain only ASCII letters, digits, hyphens or underscores.
The default output is the primary monitor. A missing named output temporarily
falls back to the primary monitor and is checked again while running. The panel
tracks monitor geometry. Each additional ID uses `panel-ID` and `settings-ID`
in the active profile. Changes to the panel list take effect at next login.
Vertical/deskbar panels and a graphical panel-list editor remain outstanding.

## Services

Native mode supervises its desktop, panels and WM independently, with bounded
restart backoff. Applications launched through Rill belong to the session manager,
so restarting the desktop or panel leaves them running; logout cleans up their
process groups, including children that outlive their leader. It selects installed
foreground providers for settings, power,
polkit authentication, networking, Bluetooth, removable media and screen locking.
Explicitly hidden autostart entries remain disabled. Rill masks duplicate Xfce
panel, desktop, notification and clipboard autostarts in its own profile.

Customize the selection in the **original** `~/.config/rill/services.json`:

```json
[
  {"name": "Bluetooth", "enabled": false},
  {"name": "Locker", "command": ["/usr/bin/xfce4-screensaver", "--no-daemon"], "restart": true}
]
```

Commands are argument arrays; they are not evaluated by a shell. Use a foreground
process for reliable supervision. `required: true` ends the session if a component
cannot start or repeatedly crashes. `RILL_SESSION_SERVICES=none` retains only
Rill's required WM and panel; `RILL_NO_AUTOSTART=1` suppresses XDG autostarts.
These switches are useful for isolated testing.

The Settings application's System page opens an installed control panel for
displays, keyboard, mouse/touchpad, appearance, default applications,
accessibility, power, network, Bluetooth and sound. It reports when no suitable
control panel is installed. These controls retain their external providers;
they are not native Rill implementations of the device settings protocols.

## Saving, locking and ending a session

XSMP connections require a cookie in a private authority file. Session endpoints
live in a private runtime directory. Autostart entries use GIO argument parsing,
XDG precedence, exact desktop-list matching, Hidden overrides and TryExec checks.
The private session bus receives the session-manager environment for activated
applications; Rill does not import its display into the shared systemd user manager.

Logout, restart and shutdown ask XSMP applications to save first. Applications
can request phase two or display a save dialog, and cancellation keeps the session
running. An unresponsive save request cancels after 30 seconds; a client showing
an interaction dialog is allowed to finish. A rejected logind power request also
cancels the transaction. Non-XSMP applications cannot participate in this protocol;
Rill does not promise recovery of their unsaved buffers.

Accepted saves write restart argument arrays and working directories to
`$XDG_CACHE_HOME/saved-session.ini` inside the session cache (normally
`~/.cache/rill-session`). Use `RILL_RESTORE_SESSION=1` to restore participating
applications on the next native login. Managed services are not duplicated.
This restores commands supplied by applications, not arbitrary application state.

Locking contacts an installed screen saver and checks its active state, or waits
for a locker command to report success. `RILL_LOCK_COMMAND` selects a command
whose successful exit must mean the screen has locked. Missing or failed lockers
report failure. Rill's Suspend action refuses to suspend if locking fails.
Automatic lid/idle policy belongs to the selected power and lock providers.
Real suspend/resume, lid behavior and lock security need hardware validation.

## Verification

The 2026-09-19 file-operation update was tested with the committed Kryon
keyboard fix `edf2e5ab` (on top of `16304d06`). The build also includes the earlier
libdraw exit-key fix, so Escape closes desktop popups without terminating the
desktop. UI labels use `Text(TextProps)` with the active font. The regression
suite passed; the file-transfer tests also passed AddressSanitizer and
UndefinedBehaviorSanitizer with GTK process-lifetime leak detection disabled.

```sh
make test                  # files, clipboard, settings, XSMP and process lifecycle tests
make wm-test               # native input, shapes, geometry recovery, compositor
make native-session-smoke  # Rill dock, desktop input, settings, crashes and logout
make session-smoke         # Xfce compatibility session
make protocol-test wayland-test
make visual-test windowed-smoke
```

Tests use private X11 displays, runtime directories, configuration and buses.
Native smoke disables hardware services and the real system bus. A successful
smoke test does not verify authentication dialogs, physical outputs, screen
locking, suspend/resume or native Wayland/Plan 9 desktop support.
