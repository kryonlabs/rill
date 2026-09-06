# Roadmap to a complete Xfce alternative

Status: 2026-09-06.

Rill has its own desktop shell and X11 window manager, but several essential
parts still come from Xfce. Matching its appearance does not establish full
replacement parity. Xfce also supplies panels, settings, session management,
file management and desktop services.

The goal is a complete desktop on XLibre/X11, Wayland and Plan 9, preserving
the user's existing panel layout and supporting unchanged Xfce plugins where
the required execution environment is available.

## Current foundation

- Rill provides the desktop surface and the standalone `rill-wm` X11 window
  manager, including decorations, native input, workspaces, window states and
  XRender compositing with drop shadows and damage-region repaints. The MRU
  Alt+Tab switcher, configurable key bindings, focus-stealing prevention,
  `_NET_WM_PING`/`_NET_WM_SYNC_REQUEST` handling and imported xfwm4
  preferences (button layout, double-click action, focus model) are in place.
- A complete nested X11 session runs inside a Xephyr window. Xephyr remains
  the nested X server; it is not a server implemented by Rill.
- Existing Xfce panel settings, pinned launchers and wallpaper are imported.
  With no existing layout, the default is one top panel; placement, height and
  auto-hide are configurable and persisted.
- Rill's own panel covers the applications menu, task buttons with urgency
  flashing, the workspace pager, a clock with calendar, show-desktop and
  session action buttons, item width properties and StatusNotifier tray
  hosting (the real Xfce panel keeps precedence when it is running).
- Shell settings persist independently: wallpaper with slideshow, clock
  format, panel placement/height/auto-hide, recently used applications and run
  history. The end-session dialog performs log out through the session manager
  and power actions through logind; `rill --run-dialog` (Alt+F2) matches
  applications and runs typed commands.
- The desktop carries a right-click menu, a middle-click window list, wheel
  workspace switching and icons for Desktop-directory entries, favorites and
  Home / File System / Trash.
- The compatible session still uses `xfce4-panel` (as a compatibility host and
  fallback), `xfsettingsd`, Xfconf, `xfce4-session` and Thunar. These have not
  been reimplemented.
- All 37 installed Xfce plugin modules passed loading, panel resizing,
  orientation changes and panel restart in the tested X11 session.
- Integration tests passed application startup, native terminal keyboard
  input, nested desktop resizing, desktop crash restart and logout.

The plugin result verifies module loading and lifecycle integration. It does
not certify every plugin function, hardware dependency, third-party plugin,
or Wayland and Plan 9 compatibility.

## Remaining work

| Area | Missing work |
| --- | --- |
| Window manager | xfwm4 theme/settings pixel compatibility, dynamic decoration changes, full transient/group stacking behavior, complete input-shape handling, crash geometry recovery, a graphical shortcut/settings interface (shortcuts are configurable through the wm-keys file today) and extensive multi-monitor/hotplug testing beyond the fake-monitor suite. |
| Compositor | Smooth frame pacing/vsync, GPU/fullscreen bypass and performance certification under sustained load. Shadows and damage-region repaints are implemented. |
| Panel and plugins | Multiple panels, vertical/deskbar modes, per-output positioning, drag reordering and the compatibility host for unchanged Xfce plugins. StatusNotifier and legacy XEmbed tray hosting work; per-item D-Bus menus do not. |
| Settings | Complete controls for displays, scaling, keyboard, mouse, themes, fonts and accessibility, with compatible preference import. Rill's settings file covers wallpaper/slideshow, clock format and panel placement; session settings still depend on xfsettingsd and Xfconf. |
| Session | An independent session manager covering autostart, saved-session restoration, crash supervision and applications with unsaved work. Log out, restart, shutdown and suspend are dispatched through the existing session manager and logind. |
| Desktop and files | Icon dragging/free placement, non-entry desktop files, drag-and-drop, file operations and trash/removable-media management. Thunar remains the file manager. |
| Everyday services | Notification server, clipboard, screen-locking, power/battery, brightness, audio, networking, authentication and accessibility integration. Existing services may be reused, but the complete experience needs validation. Legacy XEmbed tray icons and tray item D-Bus menus remain. |
| Wayland | Native rendering and desktop surfaces, compositor integration, output scaling/hotplug, input routing, workspaces and real-compositor testing. Current rendering largely depends on Xwayland; the optional task protocol adapter is not a compositor. |
| Plan 9 | Complete native desktop/session integration, rio behavior, clipboard/plumbing and application testing. Unchanged Linux GTK plugin binaries need a Linux execution/display bridge; native alternatives require ports. Neither path is complete. |
| Compatibility validation | Broader application and plugin coverage, configuration migration, updates, plugin removal/crash recovery, accessibility, mixed-DPI displays and sustained workloads on each supported backend. |

## Recommended implementation order

1. **Finish X11 window-manager reliability.** Resolve focus, stacking, resize,
   restoration and monitor edge cases. Test on XLibre itself as well as private
   Xvfb and Xephyr displays.
2. **Complete settings and session integration.** Make configuration, login,
   locking, suspend, logout and recovery dependable before replacing their
   existing service implementations.
3. **Build the Rill panel with Xfce plugin compatibility.** Preserve imported
   layouts and pinned launchers. Validate actual plugin behavior as well as
   successful loading, retaining the real Xfce panel as a fallback while the
   replacement matures.
4. **Complete desktop behavior and services.** Finish file interactions,
   notifications, clipboard, trays and everyday device/service controls.
5. **Implement native Wayland and Plan 9 support.** Give each backend explicit
   capability reporting and its own integration tests. Define how Linux-only
   plugins are handled on Plan 9 rather than implying native binary support.
6. **Validate replacement parity.** Track requirements against repeatable
   tests and real use. Keep working Xfce components available until their
   replacements meet the required behavior.

## Completion criteria

- Users can log in, launch and manage applications, configure the desktop,
  lock, suspend, recover and log out without missing essential behavior.
- Existing panel layouts and preferences migrate without unwanted panels or
  lost launcher configuration.
- Plugin compatibility claims identify the tested versions, functions and
  supported backends, including any required Xfce or Linux host dependencies.
- Each advertised platform passes native end-to-end tests; X11 results are
  not presented as evidence of native Wayland or Plan 9 support.
- Remaining behavioral differences and retained Xfce components are documented.

Full xfwm4 or Xfce 1:1 parity has not yet been achieved.

## References

- [Xfce component overview](https://www.xfce.org/projects)
- [Rill desktop compatibility status](compatibility.md)
- [Rill WM behavior and remaining gaps](wm.md)
- [Recorded Xfce plugin validation](xfce-plugins.md)
