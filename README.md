# Rill

Rill is a Kryon/libdraw desktop shell for Taiji and Plan 9.

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
- Configurable Rill panel with Applications, task buttons, workspace pager and clock.
- Optional Wayland task discovery/focus/close through wlr foreign-toplevel management.
- Persistent panel layouts, including native Plan 9 save recovery.
- Desktop icons populated from discovered launchers marked
  `X-Rill-Favorite=true`.
- Kryon app hosts such as ktrem and Shelf can be exposed through `.desktop`
  launchers using `Exec=host:<id>`.
- Linux X11 window-manager mode with `--wm`.
- Contained windowed mode with `--windowed`, which starts a private Xvfb
  display and mirrors real X11 client windows into Rill.

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
commands such as `internal:settings` or `host:ktrem`; those are handled by the
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

An X11 login session is also available through `scripts/rill-session`, with
Xfce session services and Rill crash restart/logout integration. `make install`
installs its login entry. The separate session profile imports existing Xfce
preferences on first launch. All 37 locally installed Xfce plugin modules passed
load, resize/orientation and panel-restart checks; see the
[plugin validation](docs/xfce-plugins.md).

Full Xfce replacement parity is not implemented. Native Wayland desktop surfaces,
native session lifecycles, many desktop services and exhaustive plugin testing
remain. See [the compatibility report](docs/compatibility.md) for the platform
matrix, remaining work and verification commands.

## Build

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
Rill session, our `rill-wm` window manager, Xfce panel/plugins and applications on its separate display.
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

New sessions import your existing Xfce panel layout, pinned launchers and wallpaper.
With no existing layout, Rill creates a single 26-pixel top panel; it does not
add a bottom dock. Imported layouts and later Rill customizations are preserved.
