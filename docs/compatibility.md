# Desktop compatibility status

Rill is a developing desktop shell, not yet a complete replacement for Xfce.
The current implementation provides useful X11 integration, optional Wayland
window management requests, and native Plan 9 launcher/task/settings support.
It does not certify every Xfce panel plugin. An X11 login session is available
using the installed Xfce session services; the native Wayland and Plan 9
desktop integrations remain incomplete.

## Implemented behavior

| Area | XLibre / X11 | Wayland | Plan 9 |
| --- | --- | --- | --- |
| Application discovery | XDG desktop files through GIO, localized names, desktop visibility, TryExec, user overrides | Same Linux discovery | Native launcher registry |
| Launching | GIO desktop launching, terminal entries, working directory and field codes | Preserves Wayland and session environment | Native commands |
| Task discovery / focus / close | EWMH client operations | Optional wlr foreign-toplevel protocol | rio `/dev/wsys` labels and `wctl` |
| Workspaces | EWMH pager; four workspaces in Rill WM mode | No workspace protocol adapter yet | No desktop workspace abstraction |
| Panel configuration | Persistent item order and layout | Same portable model | Native settings with interrupted-save recovery |
| Desktop surface | `--desktop` under an existing WM | Native desktop surface unavailable | Full desktop integration still needs end-to-end verification |
| Xfce plugins | `--desktop --xfce-panel` uses the real installed Xfce panel | Companion mode is currently unavailable | Linux plugin binaries cannot load natively |
| Renderer | plan9port/libdraw | Currently Xwayland via plan9port | Native libdraw target |

Launcher and platform-task storage grows dynamically. Rill's built-in app-host
slots and some UI caches still have fixed limits.

`--wm` is an experimental X11 manager: it publishes supporting-WM properties,
handles basic client discovery, focus, close and workspace requests, and
preserves clients while switching workspaces. `--windowed` starts Xvfb and
captures redirected client pixmaps. Neither mode is a complete xfwm4 replacement.

## Xfce plugin compatibility

Use the real Xfce panel as the compatibility host:

```sh
build/linux-x86_64/rill --desktop --xfce-panel
```

Run this in an X11 session with a window manager and session D-Bus. It makes
Rill the desktop surface, hides Rill's panel, and starts `xfce4-panel`.
The panel owns its GTK plugins, wrappers and Xfconf configuration. Rill's
“Add XFCE Plugin” action opens the installed panel's item chooser.
The companion panel has its own lifecycle; Rill does not yet supervise the
complete session or stop an existing panel when it exits.

This gives plugins their existing host API rather than emulating that API in
Rill. It does not embed GTK plugin widgets into Rill's own panel. Installed
plugin dependencies, Xfce versions and display-backend support still matter.
All 37 plugin modules installed on the test system loaded successfully,
including after panel resizing, orientation changes and restart. This does not
cover every available third-party plugin or every plugin function. See
[the recorded plugin validation](xfce-plugins.md). Native Plan 9 cannot execute
Linux GTK modules; that requires porting their dependencies or a separate
Linux execution/display bridge, neither of which is implemented here.

## Work required for replacement parity

1. **Session lifecycle:** X11 now has a display-manager entry and an Xfce-backed
   session with desktop XSMP registration. Startup, crash restart and logout
   have passed integration tests. Saved-session restoration, power actions and
   cancellation with unsaved applications still need end-to-end validation.
   Native Wayland and Plan 9 session lifecycles remain to be implemented.
2. **Complete X11 window management:** ICCCM focus protocols, transient/modal
   relationships, size hints, interactive resize, minimize/maximize/fullscreen,
   stacking rules, Alt-Tab, struts/workareas, multi-monitor layout and hotplug.
   Validate against XLibre itself, not only Xvfb.
3. **Native Wayland desktop:** renderer/window-system integration, layer-shell
   panel and background surfaces, output scaling and hotplug, keyboard/input
   routing, workspace protocols, compositor-specific capability reporting and
   real-compositor integration tests. The wlr task adapter alone is insufficient.
4. **Desktop services:** notification server, StatusNotifier and legacy XEmbed
   tray hosting, clipboard history/ownership, settings daemon, display/input
   configuration, power/battery, audio, networking, removable media, shortcuts,
   screen locking, authentication agents and accessibility integration.
5. **Desktop and panel UX:** wallpaper/output configuration, icon placement,
   drag/drop, context actions, file operations, panel preferences, plugin
   properties, multiple panels, auto-hide, per-output positioning and migration
   from existing Xfce configuration. Resource/tray/language items are still
   incomplete; do not interpret their current drawings as working services.
6. **Plan 9 integration:** build and launch the whole application against the
   intended Kryon revision in Taiji, verify live rio focus/close behavior, native
   app-host availability, clipboard/plumbing and desktop lifecycle. Decide and
   implement whether Linux-only plugins use a remote host or native alternatives.
7. **Compatibility certification:** inventory Xfce plugins and dependencies;
   test creation, configuration, resize/orientation, removal, crash recovery and
   upgrades on each supported display backend. “All plugins” is not established
   by a single panel smoke test.

## Verification

```sh
make test                 # shell state, launcher behavior, portable platform fixtures
make protocol-test        # Xvfb WM, workspace, close, client capacity and focus
make wayland-test         # isolated protocol server: discover/focus/close
make visual-test          # opaque window/menu compositing
make windowed-smoke       # contained X11 application capture
make xfce-smoke           # real Xfce panel plus desktop application launch
make session-smoke        # real session startup, desktop crash restart, logout
make plugin-smoke         # every installed plugin: load, resize, orientation, restart
```

`wayland-test` needs Wayland server development files; the application only
needs client development files and `wayland-scanner` for the optional adapter.
X11 integration tests need Xvfb and the utilities named by their scripts.
`xfce-smoke` additionally needs xfwm4, xfce4-panel, xfconf-query, xmessage and
D-Bus. It uses a private display, bus and temporary configuration.

The Linux checks above passed on 2026-09-05, including the real Xfce panel
and desktop-launch smoke test. Kryon libdraw smoke tests also passed for
blending, gradient clipping, rotated text and rectangle intersection.

On native Plan 9, `mk test` builds the panel persistence test. Native compilation
of shell/panel/settings/rio modules and the save/load/recovery runtime test have
passed in a Taiji VM. This is not a full native desktop test.

## X11 login session

The `rill-session` launcher runs the real Xfce session manager with Rill WM,
xfsettingsd, xfce4-panel, Thunar and Rill. Rill registers through XSMP, persists
its panel settings independently and requests immediate restart after a crash.
The Xfce session manager provides the installed autostart and logout services.
The login entry is explicitly X11-only; it does not advertise native Wayland.

Build with the SM and ICE development libraries in addition to the existing
Linux dependencies. The launcher requires Python 3, D-Bus and the Xfce
components listed above. The plan9port runtime must be available: source-tree
launches discover the sibling `plan9port`; installations can set `PLAN9` and
`DEVDRAW` or install plan9port under `/usr/local/plan9` or `/usr/lib/plan9`.

```sh
make
make install PREFIX=/usr             # package/system install; needs write access
# Or package into a staging root:
make install PREFIX=/usr DESTDIR=/path/to/package-root
```

Select “Rill (X11, Xfce services)” at the login screen. From a fresh X11 display
without an active session manager, `scripts/rill-session` also starts it.
`--prepare` only prepares the profile and prints its paths.

The first launch copies existing `xfce4` preferences to
`$XDG_CONFIG_HOME/rill/session/xfce4` (default `~/.config/rill/session/xfce4`).
Subsequent launches retain that copy. Original Xfce preferences remain intact;
changes made in Rill's session are separate. Other user autostart entries and
system XDG configuration remain in the search path. Session cache lives under
`$XDG_CACHE_HOME/rill-session`. The generated Rill failsafe startup list is
owned by the launcher. Login-provided SSH/GPG agents are inherited; the launcher
disables Xfce's agent replacement in this profile.

The session uses its own D-Bus session bus. It does not replace an already
running session manager. Installing the entry does not change the current
desktop or select Rill as the default session.

The Xfce session configuration follows the installed default schema and the
[upstream session documentation](https://docs.xfce.org/xfce/xfce4-session/advanced).

The standalone X11 WM now lives in `src/wm.c`; its tested behavior and outstanding
xfwm4 compatibility work are tracked in [wm.md](wm.md). This does not add a native
Wayland compositor or replace Plan 9 rio.
