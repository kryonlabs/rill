# Remaining work for a complete Xfce replacement

Updated 2026-09-19. **Full replacement parity is not complete.** The supported
replacement path today is the native X11 session; the real Xfce session remains
an explicit compatibility option. Device services and several control panels
still come from installed providers.

This is the implementation backlog, ordered by what most affects daily use.
See [session configuration](session.md), [compatibility status](compatibility.md)
and [window-manager details](wm.md) for the implemented behavior.

## Completed foundation

- An independent X11 session manager, WM, desktop and shaped dock panels.
- Authenticated XSMP, XDG autostart precedence, independently supervised services,
  bounded crash recovery, cancellable logout and save/phase-two interactions.
  Applications launched through Rill survive desktop and panel crashes.
- Multiple panels configured by name/output, work-area reservation, horizontal
  top/bottom placement and vertical left/right panels, autohide and persistent
  Shift-drag item ordering. A graphical panels.json editor in Settings manages
  panel creation/removal, output, edge, size and autohide.
- XDG desktop folder discovery, ordinary files/folders, application launching,
  selection, persistent icon placement, rename, new folders and confirmed Trash.
- Multiple desktop selection with Control-click, Shift ranges, a selection
  rectangle and Select All; moving selected icons together; arrow-key spatial
  navigation and type-to-select with an optional single-click activation
  preference.
- External drag and drop both ways through XDND: dropping URI lists onto the
  desktop or onto folder icons starts transfers, and dragging selected desktop
  files delivers them to other applications.
- File transfers with a queue, interactive conflict choices (skip, replace or
  keep both, each also apply-to-all), duplicate-in-place, retry after failure,
  staging-directory recovery after crashes, cut/copy/paste interoperating with
  file-manager clipboard formats, Trash listing, restore and emptying with a
  count badge and management dialog.
- Standalone Settings, installed system-control-panel launchers, concurrent
  preference merging, live panel updates and existing application preferences.
- Notifications, text clipboard history, StatusNotifier and XEmbed tray hosting,
  StatusNotifier D-Bus menus (submenus, toggles, disabled entries), basic volume
  and battery indicators and audio output selection.
- Display mode changes with a fifteen-second revert confirmation, keyboard
  repeat and pointer speed settings, a graphical WM-shortcut editor with
  conflict detection, and a supervised-service plus screen-lock-readiness
  report from the session manager.
- Lock-result checking and lock-before-suspend for Rill's own Suspend action.
- Dynamic decoration hints, input shapes, transient-group stacking, smart
  placement of new windows, xfwm4 themerc color import and normal-window
  geometry recovery after a WM crash.
- Current Kryon text/font/frame APIs and an upstream Linux libdraw fix for real
  key releases, modifier shortcuts, focus state and Delete/Backspace distinction.

## 1. Make daily use dependable

### Screen locking and power

- [ ] Install/configure a supported locker and expose its readiness clearly in
  Settings. The development machine currently reports **no detected locker**;
  the Settings System page now names the detected provider or this gap.
- [ ] Validate lock, unlock, authentication failure, idle timeout, lid close,
  suspend/resume and logout on real hardware.
- [ ] Keep a working locker and polkit provider alive across session failures.
- [x] Decide the locker boundary: supported-provider integration with explicit
  readiness reporting is the product boundary. A self-written PAM locker
  without a security audit would be riskier than the documented provider
  contract; revisit if an audited native locker is produced.
- [ ] Test inhibitor handling and rejected power requests with real logind/polkit.

Acceptance: the session cannot report a successful lock while the desktop remains
accessible; lid/idle/manual suspend must resume to a locked screen under the
supported policy. Private Xvfb tests do not establish this.

### File operations and desktop interaction

- [x] External drag/drop: XDND source and target, URI lists, copy/move
  negotiation, dropping onto folders, cancellation and protocol completion
  after actual I/O.
- [x] Keyboard spatial navigation, type-to-select and configurable single-click
  activation. Accessible selection announcements still need assistive
  technology (see below).
- [x] Conflict choices (skip, rename/keep-both, replace with explicit
  confirmation), duplicate-in-place, retry and a queue for multiple transfer
  jobs.
- [x] Recover abandoned staging directories after a desktop crash; the desktop
  clears them at startup and reports the count.
- [ ] Improve cross-filesystem moves and partial-result reporting. A completed
  destination can remain if source cleanup fails; multiple top-level items are
  committed individually, not as one atomic batch. No undo is implemented.
- [ ] Preserve file selections on clipboard-owner exit/desktop crash and expand
  testing to GVfs network mounts, large trees, ACLs/xattrs, permissions and
  disconnects. Current clipboard tests cover local files and separate owners.
- [x] Removable-media mount/unmount/eject with volume listing and errors
  through the desktop's Removable Drives dialog; actions use udisksctl, so
  privileged operations may still require an authentication provider. The
  installed file manager keeps advanced browsing.

Acceptance: real Thunar and another file manager can copy, cut, paste and drag
files both ways; cancellation/conflicts never silently overwrite or remove data;
errors identify completed and unfinished work.

### Settings and accessibility

- [x] Native display mode changes with a timed rollback for an unusable setup.
- [x] Native keyboard repeat and pointer speed preferences; a graphical
  WM-shortcut editor with conflict detection and reset. Remaining device
  categories keep the documented provider contract through installed control
  panels.
- [ ] Full keyboard traversal and accessible names/roles/states/actions; desktop
  selection changes are already announced through the shell status line, and
  screen-reader navigation, high contrast and large text throughout Rill's own
  custom desktop, menus, panels and dialogs remain open.
- [ ] Complete preference migration beyond the current copied/linked profiles,
  with documented ownership and a reversible reset/export path.

Acceptance: essential settings can be changed without editing configuration
files, survive relogin, and can be reached and operated with a keyboard and screen
reader. Opening an installed control panel does not establish native support.

## 2. Complete panel and window-manager behavior

### Panels, trays and plugins

- [x] Vertical (left/right edge) orientation with appropriate item layout and
  struts, and deskbar orientation (tall top/bottom bars rendering wrapped
  icon-cell rows).
- [x] Graphical creation/removal of panels, monitor selection and geometry
  editing through the Settings panels page; changes apply at the next login.
- [ ] Full native migration of existing Xfce panel/plugin layouts.
- [ ] Independent hosting of unchanged GTK plugins, or explicit supported plugin
  replacements. Compatibility mode still uses the real Xfce panel.
- [x] StatusNotifier D-Bus menus with submenu/toggle/disabled states; item
  lifecycle menu refresh and icon-only shortcuts inside submenus remain open.
- [x] Audio output selection for the default sink through the volume item;
  more complete network and Bluetooth controls remain open.

Acceptance: representative existing panel layouts migrate without losing their
settings; panels follow hotplug and orientation changes; tray menus operate the
real application actions.

### Window manager and compositor

- [x] Broader transient/group stacking and placement: dialogs and their
  transients stay above their owner, and new windows prefer free screen space.
- [x] xfwm4 theme compatibility at the color level: active/inactive frame and
  title colors are imported from the installed theme's themerc. Themed
  decoration pixmaps and per-theme button art remain open.
- [ ] Frame pacing/vsync, fullscreen bypass and sustained-load benchmarks.
- [ ] Physical output hotplug, mixed DPI, multi-seat and fullscreen-game testing.

Acceptance: common terminals, editors, browsers, file managers and games keep
correct focus, geometry, stacking and input across workspace/output changes and
WM restarts, with measured rendering behavior.

## 3. Broaden session recovery

- [x] Non-XSMP unsaved-document handling: after XSMP clients accept the logout,
  every remaining window is asked to close through the window manager so the
  applications' own save prompts decide about unsaved work, with a bounded
  wait before cleanup instead of a silent kill.
- [ ] Arbitrary XSMP client restart styles and wider application-restore coverage.
- [ ] Persist/restore application state where applications support it, not merely
  their restart commands and working directories.
- [x] Better user-facing diagnostics for missing/crashing optional services and
  failed asynchronous application launches: the session manager publishes a
  per-service status file shown in Settings, including screen-lock readiness.
- [ ] Explicit behavior for in-progress file transfers during logout/recovery.
  Graceful desktop exit currently cancels and joins its active transfer.

Acceptance: cancellation preserves the session; recovery does not duplicate
services or abandon owned processes; restore and unsaved-work claims identify the
applications and protocols actually covered.

## 4. Other desktop backends

### Native Wayland

- [ ] Native renderer/window integration and layer-shell desktop/panel surfaces.
- [ ] Output scale/hotplug, input, workspaces, clipboard, drag/drop and session
  integration, with real-compositor end-to-end tests.
- [ ] Decide and document supported compositors/protocols or a native compositor.

The existing foreign-toplevel task adapter is not a Wayland desktop or compositor.
Current Linux rendering through plan9port relies on X11/Xwayland.

### Native Plan 9

- [ ] Full native desktop/session/rio integration, clipboard/plumbing, input
  semantics, file operations and end-to-end app tests.
- [ ] Validate stack/memory limits and saved-setting recovery on the real target.
- [ ] Define ports/replacements or an execution/display bridge for Linux GTK
  plugins; their binaries cannot run natively on Plan 9.

The latest physical-keyboard fix applies to Linux X11. It does not add key-release
or modifier parity to the native Plan 9 rune transport.

## Verification and completion gates

The automated suite covers recursive file transfers, symlinks, collision refusal,
interactive conflict decisions, the transfer queue, duplicate and retry,
self-copy through a symlink, cancellation cleanup, crash-time staging recovery,
Trash listing/restore/emptying, cross-process clipboard formats, XDND target
and source protocol exchanges, desktop multiple selection and cut/copy/paste,
desktop crash recovery, XSMP transactions, panels, WM input/shapes/geometry and
compositor rendering.

```sh
make test
make native-session-smoke session-smoke
make wm-test protocol-test wayland-test
make visual-test windowed-smoke
```

Before calling Rill a complete Xfce replacement:

- [ ] Record supported distribution, dependencies, applications and hardware.
- [ ] Run a fresh-user installation and upgrade/migration test.
- [ ] Validate login, settings, file operations, everyday apps, lock, suspend,
  resume, recovery and logout together on real hardware.
- [ ] Complete accessibility testing with actual assistive technology.
- [ ] Keep backend-specific claims separate and list retained external providers.

No full-parity release or hardware certification is implied by the passing
isolated X11 tests. See [session.md](session.md) for the tested build and setup.
