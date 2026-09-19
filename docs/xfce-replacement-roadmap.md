# Roadmap to a complete Xfce alternative

Status: 2026-09-19. The current implementation and its limits are recorded in
[compatibility.md](compatibility.md). Native X11 is the first usable replacement
path. [session.md](session.md) describes configuration and retained providers.

## Completed in this update

- Rill's default X11 login uses its own session manager, WM, desktop and dock.
  The real Xfce session remains a separate compatibility choice.
- Independently supervised services, private authenticated XSMP, correct XDG
  autostart overrides, save/interaction/phase-two cancellation, saved restart
  commands and opt-in restoration. Power rejection preserves the session.
  Launched applications survive desktop/panel crashes and exit on logout.
- Native shaped panel surfaces and work-area reservation, named output/monitor
  selection, multiple configured panels, persistent Shift-drag item ordering,
  correctly sized tray allocations and focus restoration after menus.
- XDG desktop folder discovery, ordinary files/folders, refresh, repaired
  double-click launching, persistent icon dragging, rename/new-folder/Trash
  actions and overwrite refusal.
- Standalone Settings, links to installed device/service controls, concurrent
  preference merging, live updates, and migration of existing Rill/user-folder/
  GTK/MIME preferences into the private profile.
- Lock result checking and lock-before-suspend for Rill's Suspend action.
  Installed lock/power/authentication/network providers are detected and supervised.
- Dynamic decoration hints, propagated input shapes and normal window geometry
  recovery after a WM crash, covered by real X11 interaction tests.
- Current Kryon text/font/frame APIs and generated build headers, optional sync
  dependencies and the downstream clean-text API check.

## Remaining implementation and validation

| Area | Work still needed |
| --- | --- |
| Native panel | Vertical/deskbar layouts, graphical panel/output management, complete Xfce layout/plugin migration, independent unchanged-plugin hosting and D-Bus tray menus. The real Xfce host remains available. |
| Native settings | Display/scaling/input/theme/font/accessibility controls, graphical WM shortcuts, complete preference import and provider-independent persistence. The current System page opens installed control panels. |
| Files | External drag/drop, file clipboard, multiselection, recursive operations and complete removable-media/trash integration. The installed file manager remains responsible for browsing and advanced operations. |
| Session and services | Non-XSMP unsaved-document handling, broader application restore coverage, arbitrary client restart-style support, native lock UI/authentication, accessible custom controls and device-service integration beyond selected providers. |
| WM/compositor | Full transient/group stacking and placement, xfwm4 theme compatibility, frame pacing/vsync/fullscreen bypass and sustained-load profiling. |
| Hardware certification | Real lock/suspend/resume/lid behavior, polkit interaction, networking, physical output hotplug, mixed DPI, multi-seat and application/game regressions. Private Xvfb tests cannot certify these. |
| Wayland | Native renderer/window integration, layer-shell desktop/panel surfaces, output scaling/hotplug, input/workspaces and real-compositor tests. A task adapter is not a compositor or desktop backend. |
| Plan 9 | Full native desktop/session/rio, clipboard/plumbing and end-to-end app testing. Linux GTK plugin binaries require a separate execution/display bridge or ports. |

## Completion criteria

Users must be able to log in, launch and manage applications, configure devices,
lock, suspend, recover and log out without missing essential behavior. Existing
preferences and plugins must migrate without losing configuration. Each supported
backend needs its own end-to-end evidence, and claims must identify retained
providers and the tested applications, versions and hardware.

The current X11 work resolves substantial lifecycle and interaction gaps. It does
not establish Xfce/xfwm4 1:1 parity or native Wayland/Plan 9 completion.
