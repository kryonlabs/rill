# Rill

Rill is a Kryon/libdraw desktop shell for Taiji and Plan 9.

Rill is being restored as TaijiOS's main desktop environment, with its entire
implementation in current Ziran and its UI built with Kryon. The shell state,
launcher/window behavior, service data types, panel layouts, settings
persistence, stub adapter, and Plan 9 platform services now live in `src/*.zi`. Their
handwritten C implementations were removed; both hosted and native Plan 9
builds use generated output.

The conversion remains incomplete. Graphical screens, file operations,
Linux platform services, window management,
and the session daemon still need conversion. The C desktop entrypoint and
two legacy `.kry` screens still depend on the former Kryon interface, so the
full desktop build is not yet restored against current Kryon.

`make shell-test persistence-test platform-test` checks the generated implementation against
its existing C consumers and runs the Ziran behavior tests from source and
saved IR. In TaijiOS, `make rill-ziran-plan9-smoke` generates both forms
through `plan9-c`, then compiles, links, and runs them with native `8c`/`8l`
in the private guest. It covers launcher/task growth, focus and stacking,
recent launchers, platform task ID collisions, closing applications, and the
stub adapter, plus panel layouts, malformed input, settings merges, large
preference files, native file operations, and interrupted-save recovery. The
Plan 9 adapter checks application and icon registry overrides, rio task
discovery, detached process launching, and guarded focus/close commands.
Window controls require a PID in a process group launched by this adapter;
listing a window grants no authority to control it. Taiji's rio snapshot
includes that PID and rejects guarded commands when the owner has changed.
Graphical desktop readiness requires separate verification after its conversion.

The project keeps shell behavior outside Kryon. Kryon provides the UI runtime,
renderer backends, widgets, and reusable platform primitives; Rill owns panels,
launchers, task/window presentation, desktop surfaces, settings, and platform
adapters.

## Current support

- Shared shell state model.
- Platform service interface for launcher/task/session operations.
- Linux adapter targeting XLibre-compatible X11 desktops through EWMH task
  discovery, focus, and close requests, with plan9port/libdraw for the UI.
- Linux application discovery from XDG `.desktop` files. Rill does not keep a
  hard-coded C launcher list for Linux.
- Plan 9 adapter using native launcher commands and rio window controls.
- Configurable Rill panel with Applications, task buttons, workspace pager and
  clock. The panel can sit at the top or bottom edge, its height is adjustable,
  it can auto-hide until the pointer reaches its screen edge, and the clock
  opens a month calendar. Show-desktop and session action buttons are available
  as panel items, urgent windows flash in the task list, and item widths are
  editable through the item Properties dialog.
- StatusNotifier tray hosting: Rill owns `org.kde.StatusNotifierWatcher` when
  no other host (such as the real Xfce panel) already registered, renders
  StatusNotifierItem icons in the panel, forwards left clicks as Activate and
  shows the item's D-Bus menu (submenus, toggles, disabled entries) on right
  click. Legacy XEmbed tray icons dock into an
  override-redirect host window composited over the same panel slot
  (`_NET_SYSTEM_TRAY_S<n>`), following panel placement and auto-hide.
- Desktop notification server: Rill owns `org.freedesktop.Notifications` when
  no other server is running, shows banners with summary and body, invokes the
  default action on clicks, and honors replacement ids and expiry timeouts.
  Lock, log out, restart, shut down and suspend are available from the
  end-session dialog. Locking checks the screen saver or locker command result;
  Suspend requires a successful lock. The panel's resource
  item shows battery charge and charging state from the platform's
  power-supply data, and a volume item controls the default audio sink through
  pactl: the mouse wheel changes the level, clicking toggles mute.
- Optional Wayland task discovery/focus/close through wlr foreign-toplevel management.
- Persistent panel layouts, including native Plan 9 save recovery.
- Persistent shell settings (wallpaper, wallpaper slideshow, clock format,
  panel height, recently used applications, run history) in
  `$XDG_CONFIG_HOME/rill/settings`.
- Interactive settings app with a wallpaper picker fed by the platform's
  background directories, clock formats and a panel height preference.
- End-session dialog with log out, restart, shut down and suspend. Power
  actions go through logind; logging out asks the session manager to exit.
- Run dialog (`rill --run-dialog`, bound to Alt+F2 in Rill WM) matching
  installed applications and running typed commands, with recent history.
- Desktop icons from launchers marked `X-Rill-Favorite=true`, entries in the
  user's configured XDG desktop folder (including ordinary files and folders),
  and Home / File System / Trash shortcuts. Icons select on click, open on
  double-click (single-click is configurable), and retain dragged positions.
  Rename, new-folder and confirmed Trash actions are available from the desktop
  context menu, arrow keys move the selection spatially, typing selects by name,
  and the Trash icon carries a count badge with restore and empty commands.
- External drag and drop in both directions through XDND: files dropped by
  other applications land on the desktop or inside folder icons, and dragging
  selected desktop files delivers them to any drop target. Transfers queue,
  offer conflict choices (skip, replace or keep both, optionally for the whole
  job), support duplicate-in-place and retry, and abandoned staging
  directories are recovered after a crash.
- Desktop context menu (right-click) with Applications, Terminal, Files,
  Settings, wallpaper and session entries; middle-click opens a window list
  that focuses any task; the mouse wheel over the desktop switches workspaces.
- Kryon app hosts such as t9 and Shelf can be exposed through `.desktop`
  launchers using `Exec=host:<id>`.
- Linux X11 window-manager mode with `--wm`.
- Contained windowed mode with `--windowed`, which starts a private Xvfb
  display and mirrors real X11 client windows into Rill.
- Native X11 login through `rill-sessiond`, with authenticated XSMP,
  XDG autostart overrides, independent desktop/WM/panel service supervision,
  cancellable save-before-logout, and opt-in saved-command restoration.
- Standalone dock panels with named output selection and reserved work areas.
  Configure multiple panels through `panels.json` or the Settings panels page;
  panels sit on any screen edge (vertical panels use a compact icon column),
  and Shift-drag reorders items.
- A standalone `rill --settings` window, live shared preference updates and
  links to installed system control panels. File saves are atomic on Linux
  and merge changes from separate desktop/panel processes. Settings also
  changes display modes with a fifteen-second revert confirmation, keyboard
  repeat and pointer speed, edits the window-manager shortcuts with conflict
  detection, manages the panel list, and reports supervised services plus
  screen-lock readiness.
- An XSETTINGS provider (the xfsettingsd core): Rill owns the
  `_XSETTINGS_S<n>` selection and broadcasts the imported GTK theme/font
  values (with overrides from the Rill settings file) to every X11
  application through the root window property, yielding to an already
  running settings daemon.
- A clipboard manager (the xfce4-clipman core): Rill watches the CLIPBOARD
  and PRIMARY selections through XFixes, captures text into a deduplicated
  history, and re-serves selections so copies outlive their owner. A panel
  item lists the history and pushes any entry back onto the clipboard.

## Linux Launchers

The Linux target reads launchers from `.desktop` files instead of hard-coding
applications in Rill. Discovery order is:

1. `RILL_APPLICATION_DIRS`, as a colon-separated list of directories.
2. `$XDG_DATA_HOME/applications`, or `$HOME/.local/share/applications`.
3. `$XDG_DATA_DIRS/applications`, or `/usr/local/share/applications` and
   `/usr/share/applications`.

Rill reads ordinary `Desktop Entry` application files with `Type=Application`,
`Name`, `Exec`, `Icon`, `Comment`, and `Categories`. It honors localized names, `TryExec`, desktop visibility and user overrides.
Hidden and `NoDisplay` entries are skipped; terminal applications are supported.
GIO handles ordinary desktop launches, including field codes and working directories. The `Exec` command may also use Rill
commands such as `internal:settings` or `host:t9`; those are handled by the
shell before falling back to the platform launcher.

Two optional keys are recognized:

```ini
X-Rill-ID=terminal
X-Rill-Favorite=true
```

`X-Rill-ID` gives the launcher a stable Rill-facing ID. `X-Rill-Favorite`
places the launcher on the desktop. Packages or user config should provide
these files for Rill-specific built-ins; the Linux platform code should not grow
a fixed launcher table.

## XFCE Compatibility

Rill's native panel items are described through `include/rill_panel.h`. That is
the extension point for Rill panel configuration and native Rill panel modules.

Run `build/linux-x86_64/rill --desktop --xfce-panel` in an X11 session to use
Rill's desktop with the real installed Xfce panel and its plugin host. This
requires an existing window manager and session D-Bus. It suppresses Rill's
own panel; GTK plugins are hosted by Xfce, not embedded into Rill widgets.

`scripts/rill-session` now defaults to Rill's own manager, WM, desktop and
panel. `--mode xfce` selects the compatibility session and real Xfce panel.
`make install` installs both login entries and the Settings launcher.
Optional installed providers supply settings, power, authentication, networking,
Bluetooth, removable-media and screen-lock services. No Xfce executable is a
hard prerequisite of native mode, but these service implementations are still
needed for their respective features.

See [session setup and configuration](docs/session.md),
[the compatibility report](docs/compatibility.md) and
[the remaining roadmap](docs/xfce-replacement-roadmap.md).
Full Xfce replacement parity, native Wayland desktop surfaces and complete
native Plan 9 integration remain unfinished.

## Build

Rill uses the current Kryon checkout at `../kryon`. Update that checkout to
Kryon's latest `master` before building. The desktop file shortcuts require
Kryon's Linux libdraw keyboard fix `edf2e5ab` or a descendant. The build generates Kryon's runtime
and headers under Rill's `build/` directory. Kryon's optional sync support is
disabled by default; use `KRYON_WITH_SYNC=1` when building with sync-enabled
embedded app hosts.

The default backend is `libdraw` so the Linux build exercises the same visual
path intended for Plan 9:

```sh
make
```

Override paths when needed:

```sh
make KRYON_DIR=/mnt/storage/Projects/kryon \
     PLAN9PORT_DIR=/mnt/storage/Projects/plan9port
```

Run tests:

```sh
make test
```

Run the contained X11 smoke test:

```sh
make windowed-smoke
```

Run Rill under plan9port/devdraw:

```sh
make run
```

Run Rill as an X11 window manager on the current display:

```sh
build/linux-x86_64/rill --wm
```

Run Rill as a contained virtual display inside an ordinary desktop window:

```sh
build/linux-x86_64/rill --windowed xterm
```

Native Plan 9 uses Kryon's native `mkfile` plus Rill's Plan 9 adapter. In
Taiji, `/sys/src/cmd/rill` installs the system desktop command.

## Complete desktop in a window

```sh
scripts/rill-window --size 1280x800
```

This starts a real Xephyr X server in one host X11/XLibre window, then runs the
Rill session, `rill-wm`, Rill's panel and applications on its separate display.
Use `RILL_SESSION=xfce` for the real Xfce panel and its GTK plugins.
Closing the outer window stops the nested session; logging out closes it too.
The window is resizable and Rill follows the inner display size.

Install Xephyr (`xserver-xephyr` on Debian) and the session dependencies above.
Set `RILL_XEPHYR=/path/to/Xephyr` to use a local server binary. `make install`
also installs `rill-window`. The nested server chooses an available display,
uses a private Xauthority cookie, disables TCP listening, and starts the session
with its own D-Bus bus and runtime directory. This is a nested desktop on the
same machine, not a virtual machine.

The older `rill --windowed` uses Xvfb and Rill's experimental window capture and
input forwarding. Use `rill-window` for the complete session and standard X11
input/window handling. Clipboard and drag/drop between the outer and inner
desktops are not bridged automatically. GPU/GLX behavior and global shortcut
capture depend on the nested server and host; they are not certified by the
basic smoke test.

`make nested-smoke` checks separate inner/outer window trees, desktop resizing,
application launch inside the inner server, crash restart and logout. It needs
Xephyr, xdotool and the existing session-test dependencies.

## Rill window manager

`make` builds the standalone `rill-wm`. It is now the default WM in
`rill-session` and `rill-window`. It manages real X11 application windows with
native input, decorations, workspaces, panel work areas and XRender compositing.
See [window-manager behavior and compatibility](docs/wm.md) for shortcuts,
validation and the remaining gaps before xfwm4 parity.

Use `RILL_WM=xfwm4 scripts/rill-window` for the previous WM, or
`RILL_WM_COMPOSITE=0 scripts/rill-window` to disable Rill's compositor.
The older `rill --wm` is an experimental capture path; it is separate from
`rill-wm`.

The compatibility session preserves your Xfce panel layout and pinned launchers.
Native sessions preserve existing Rill panel files, import theme/background
preferences and default to one 26-pixel top panel. Xfce plugin layouts are not
translated into native Rill panel items.
