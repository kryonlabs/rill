# Rill WM

`rill-wm` is Rill's own X11 window manager and XRender compositor. The session
launchers use it by default. It runs inside Xephyr for the desktop-in-a-window
mode, or on a fresh X11 display. It refuses to take over an existing WM.
XLibre remains the host X server; Xephyr remains the nested X server.

This is an implementation in Rill, not an xfwm4 wrapper. The compatible session
still uses the real xfce4-panel and its installed plugins, xfsettingsd,
xfce4-session and Thunar. Their code and APIs have not been reimplemented.

## Implemented behavior

- Reparenting decorations, UTF-8 titles, focus, close, move and resize, with
  application size constraints and native X input.
- Minimize, maximize, fullscreen, restore, shade, half-screen tiling, above/below
  stacking, sticky windows and show-desktop.
- Configurable workspace count through EWMH, workspace switching and moving
  windows between workspaces; client lists for task buttons and pagers.
- Transient/modal dialogs, parent minimize/restore, panel struts/work areas,
  basic monitor-aware maximization/fullscreen and display resize handling.
- ICCCM delete/take-focus messages, EWMH state/move/resize requests, Motif
  undecorated hints at startup, shaped windows and ARGB frame visuals.
- XComposite/XDamage/XFixes/XRender compositing, window opacity and
  override-redirect menus. Compositing disables itself if required extensions
  are unavailable or another compositor owns the selection.
- XSMP registration, session restart support and graceful exit that returns
  live application windows to the root window.

## Controls

| Input | Action |
|---|---|
| Alt+Tab / Alt+Shift+Tab | Cycle windows |
| Alt+F4 / Alt+F9 / Alt+F10 | Close / minimize / maximize |
| Alt+F7 / Alt+F8 | Move / resize using keyboard or pointer |
| Alt+Space, or title right-click | Window menu |
| Alt+left/right drag | Move / resize |
| Title double-click / wheel | Maximize / shade |
| Ctrl+Alt+Left/Right | Switch workspace |
| Ctrl+Alt+Shift+Left/Right | Move active window between workspaces |
| Super+arrows | Tile window |
| Super+D / Ctrl+Alt+D | Show desktop |
| Escape during move/resize | Restore previous geometry |

## Running and testing

Build dependencies include X11, Xext, Xft, XRandR, XRender, XComposite, XDamage,
XFixes, SM and ICE development libraries. The session dependencies in the
README remain required.

```sh
make
scripts/rill-window --size 1280x800
RILL_WM_COMPOSITE=0 scripts/rill-window     # disable compositing
RILL_WM=xfwm4 scripts/rill-window          # previous WM as a fallback
make wm-test
make session-smoke
make plugin-smoke
make nested-smoke
```

`wm-test` uses real X clients and XTest input. It checks panel struts,
reparenting, state transitions and normal geometry restoration, size hints,
workspaces, modal focus, keyboard/menu actions, pointer movement and cancel,
shaped/ARGB compositing, popup pixels, protocol timestamps and graceful WM
restart/adoption. Session tests run with private configuration and buses.
`RILL_TEST_REAL_APPS=1` additionally launches xterm, Mousepad and Thunar and
checks keyboard delivery by executing a command in the terminal.

The installed 37 Xfce plugin modules have passed loading, panel resizing,
orientation changes and panel restart with Rill WM and compositing enabled.
A combined Xephyr run also passed inner/outer isolation, live desktop resizing,
terminal/editor/file-manager startup, typed terminal execution, desktop crash
restart and logout. This verifies module/session integration, not every plugin's hardware,
network or privileged functionality.

## Remaining work before claiming xfwm4 1:1 parity

Rill WM is not yet a 1:1 xfwm4 replacement. These gaps remain explicit:

- xfwm4 theme parsing, matching decoration appearance, xfconf preference
  compatibility and its complete shortcut/settings interfaces.
- MRU switcher UI, detailed placement and focus-stealing policies, full
  transient/group stacking behavior and application-specific compatibility.
- Sync-resize/ping protocols, comprehensive session geometry restoration after
  a WM crash, dynamic decoration changes and full input-shape handling.
- Compositor shadows, vsync/frame pacing, efficient damage-region rendering,
  GPU/fullscreen bypass and performance certification under sustained load.
- Full multi-monitor/hotplug, mixed-DPI and multi-seat validation, accessibility
  coverage and a broad application/game regression suite.
- Native Wayland compositor and native Plan 9 WM parity. This executable speaks
  X11; it does not turn the Wayland client adapter into a compositor or replace
  rio. Native Xfce GTK panel plugins remain an X11/Linux compatibility path.

The older `rill --wm`/`--windowed` capture implementation is separate and remains
experimental. Use the standalone session path for real client windows.
