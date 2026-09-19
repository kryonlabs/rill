# Desktop compatibility status

Status: 2026-09-19. Rill now supplies a usable independent X11 session with its
own manager, WM, desktop and dock panel. Full Xfce parity is not established.
Optional installed providers still supply device settings, power policy,
authentication, networking, Bluetooth, removable-media and screen-lock services.
Native Wayland and complete native Plan 9 desktop integration remain unfinished.

## Implemented and tested

| Area | Current behavior |
| --- | --- |
| Session | Default native login, private cookie-authenticated XSMP, XDG autostart precedence/filtering, bounded WM/panel/desktop/service restart, cancellable logout and phase-two save, restart-command persistence and opt-in restoration. Restart/shutdown participate in the same save transaction; rejected power requests cancel. A per-service status file feeds the Settings diagnostics view, including screen-lock readiness. |
| Panel | Separate X11 dock, work-area reservation, shaped menus, named monitor/output selection, multiple configured panels with a graphical editor in Settings, horizontal top/bottom and vertical left/right placement, height/width, autohide, Shift-drag item reordering and persisted properties. StatusNotifier items expose their D-Bus menus (submenus, toggles, disabled entries). |
| Desktop | XDG user-folder discovery, ordinary files and folders, desktop-only launchers, live refresh, double-click, persistent dragged positions, multiple/range/rectangle selection, grouped icon dragging, arrow-key navigation, type-to-select, optional single-click activation, cut/copy/paste, XDND drops onto the desktop or folder icons and drags to other applications, cancellable recursive transfers with a queue, conflict choices (skip/replace/keep both), duplicate-in-place, retry, crash-time staging recovery, rename/new folder/confirmed Trash with listing, restore, empty and a count badge, Home/File System/Trash shortcuts and context/window-list menus. |
| Settings | Standalone preferences window, wallpaper picker with scrolling/slideshow, clock/panel preferences, atomic Linux saves, concurrent-key merging and live updates across processes. Display mode changes with a 15-second revert confirmation, keyboard repeat and pointer speed preferences, a WM-shortcut editor with conflict detection and reset, a graphical panels.json editor, and a supervised-service and locker-readiness report. System categories open installed control panels and report missing providers. |
| Window manager | Decorations including live Motif changes and xfwm4 themerc colors, workspaces, struts, modal relationships with transient-group stacking, smart placement of new windows, focus prevention, native move/resize, MRU Alt+Tab, configurable shortcuts with a graphical editor, minimize/maximize/fullscreen, bounding/input shapes, saved normal geometry across WM crashes, XRender shadows/damage repainting. |
| Desktop services | Notifications, text clipboard history/re-serving, StatusNotifier and XEmbed trays with per-item menus, battery indicator, default audio-sink volume and output selection. Native session supervises detected optional service providers. Lock commands are checked and Rill's Suspend action requires a successful lock; lock readiness is reported in Settings. |
| Compatibility session | `rill-session --mode xfce` uses the real Xfce session/panel and retains GTK plugin and layout support. Existing preferences are copied into a separate profile. |

See [session configuration](session.md) for provider choices, profiles, panel
configuration, saved sessions and controls. The native profile does **not**
translate arbitrary Xfce panel plugin layouts into Rill widgets. Existing Xfce
layouts remain available through compatibility mode.

The system settings links are integration with existing tools. They do not mean
Rill reimplements RandR/input configuration, power management, a PAM screen locker,
polkit, NetworkManager or accessibility infrastructure. A missing provider is
reported; it is not replaced by a decorative control or a successful-fork claim.

## Backend boundaries

| Capability | X11 / XLibre | Wayland | Plan 9 |
| --- | --- | --- | --- |
| Application discovery and launch | XDG/GIO, localized names, overrides, field codes and working directories | Same Linux discovery; preserves environment | Native registry/commands |
| Task focus/close | EWMH | Optional wlr foreign-toplevel adapter | rio labels/wctl |
| Desktop/panel/WM | Native X11 session described above | No native renderer/layer-shell/session implementation | Native libdraw build; full desktop/session integration incomplete |
| Unchanged Xfce GTK plugins | Real installed Xfce panel in compatibility mode | Not certified | Cannot execute Linux binaries natively |
| Rendering | plan9port/libdraw plus XRender WM compositor | Current application rendering depends on Xwayland | Native libdraw |

`rill-wm` is the standalone session WM. The older `rill --wm` and `--windowed`
paths use experimental capture/input forwarding and are not equivalent to it.

## Outstanding work

- Deskbar (wrapped multi-row) panel layout, full Xfce layout migration into
  native widgets, and an independent GTK plugin host.
- Remaining native system-settings categories (layouts, touchpad gestures,
  theme/font/default-app/power) and accessibility of Rill's own custom UI;
  retaining supported external providers remains necessary.
- Transfer undo, batch rollback, GVfs/network/ACL coverage and full
  removable-media management. Background recursive transfers, interactive
  conflict choices, file clipboard operations, external XDND drag/drop and
  Trash management are implemented; advanced browsing and default-app
  selection remain with the installed file manager/control panels.
- Tray menu live refresh on item updates, and more complete network and
  Bluetooth controls.
- Themed decoration pixmaps beyond imported themerc colors, compositor
  vsync/frame pacing/fullscreen bypass and sustained-load benchmarks.
- Physical multi-monitor/hotplug/mixed-DPI/multi-seat checks, real lock and
  suspend/resume/lid tests, non-XSMP unsaved-work handling and broad application
  regressions. Command restoration is not universal document-state restoration.
- Native Wayland desktop and full Plan 9 session/clipboard/plumbing integration.

See [the remaining roadmap](xfce-replacement-roadmap.md) and [WM details](wm.md).

## Verification

`make test` covers the shell, platform adapters, launcher discovery, desktop
files/file-operation failures, settings save/merge, tray/XSETTINGS/clipboard
protocols, XSMP cancellation/phase two/power rejection, exact autostart matching,
service recovery, process cleanup and command restoration.

`make native-session-smoke` uses a private X11 display and bus to exercise native
login, dock properties, desktop double-click/drag/persistence, standalone settings,
live panel updates, panel reordering, named monitor placement, crash restart and
logout. It disables hardware agents and access to the real system bus.
`make wm-test` exercises real X clients, native input and compositor pixels,
including live decorations, input shapes and post-crash normal geometry.

Additional checks are `make protocol-test`, `make wayland-test`, `make visual-test`,
`make windowed-smoke`, `make session-smoke`, `make xfce-smoke`, `make plugin-smoke`
and `make nested-smoke`. Tests require the relevant Xvfb/Xephyr/D-Bus utilities.
The Wayland test verifies the optional task protocol, not a native desktop.

Historical testing loaded all 37 installed Xfce plugin modules and exercised
resizing, orientation and restart. This is evidence of compatibility-host
integration, not certification of every plugin function, third-party plugin,
hardware dependency or backend. See [the recorded plugin report](xfce-plugins.md).

Native Plan 9 module compilation and panel-save recovery were previously tested
in Taiji; this change has not been certified as a complete native Plan 9 desktop.
