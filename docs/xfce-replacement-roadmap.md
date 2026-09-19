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
  top/bottom placement, autohide and persistent Shift-drag item ordering.
- XDG desktop folder discovery, ordinary files/folders, application launching,
  selection, persistent icon placement, rename, new folders and confirmed Trash.
- Multiple desktop selection with Control-click, Shift ranges, a selection
  rectangle and Select All; moving selected icons together.
- File cut/copy/paste interoperating with file-manager clipboard formats,
  background recursive copy/move/Trash, progress and cancellation. Copies preserve
  symbolic links, reject existing destinations and stage each top-level item
  before publishing it. Rill clears its own cut clipboard after a successful move.
- Standalone Settings, installed system-control-panel launchers, concurrent
  preference merging, live panel updates and existing application preferences.
- Notifications, text clipboard history, StatusNotifier and XEmbed tray hosting,
  basic volume and battery indicators.
- Lock-result checking and lock-before-suspend for Rill's own Suspend action.
- Dynamic decoration hints, input shapes and normal-window-geometry recovery
  after a WM crash.
- Current Kryon text/font/frame APIs and an upstream Linux libdraw fix for real
  key releases, modifier shortcuts, focus state and Delete/Backspace distinction.

## 1. Make daily use dependable

### Screen locking and power

- [ ] Install/configure a supported locker and expose its readiness clearly in
  Settings. The development machine currently reports **no detected locker**.
- [ ] Validate lock, unlock, authentication failure, idle timeout, lid close,
  suspend/resume and logout on real hardware.
- [ ] Keep a working locker and polkit provider alive across session failures.
- [ ] Decide whether a native PAM-authenticated locker is required or supported
  provider integration is the product boundary; implement and audit accordingly.
- [ ] Test inhibitor handling and rejected power requests with real logind/polkit.

Acceptance: the session cannot report a successful lock while the desktop remains
accessible; lid/idle/manual suspend must resume to a locked screen under the
supported policy. Private Xvfb tests do not establish this.

### File operations and desktop interaction

- [ ] External drag/drop: XDND source and target, URI lists, copy/move negotiation,
  dropping onto folders, cancellation and protocol completion after actual I/O.
- [ ] Add keyboard spatial navigation, type-to-select, configurable single-click
  activation and accessible selection announcements.
- [ ] Add conflict choices (skip, rename, replace with explicit confirmation),
  duplicate-in-place, retry and a queue for multiple transfer jobs. Current
  behavior refuses collisions and permits one active transfer per process.
- [ ] Recover abandoned staging directories after a desktop crash; expose any
  cleanup failure. Graceful cancellation cleans staging data, but a killed
  process can leave a hidden `.transfer-*` directory.
- [ ] Improve cross-filesystem moves and partial-result reporting. A completed
  destination can remain if source cleanup fails; multiple top-level items are
  committed individually, not as one atomic batch. No undo is implemented.
- [ ] Preserve file selections on clipboard-owner exit/desktop crash and expand
  testing to GVfs network mounts, large trees, ACLs/xattrs, permissions and
  disconnects. Current clipboard tests cover local files and separate owners.
- [ ] Complete removable-media mount/eject/unmount, Trash count/restore/empty and
  volume errors. The installed file manager handles browsing and advanced media
  operations today.

Acceptance: real Thunar and another file manager can copy, cut, paste and drag
files both ways; cancellation/conflicts never silently overwrite or remove data;
errors identify completed and unfinished work.

### Settings and accessibility

- [ ] Native display configuration, scaling and output placement, with timed
  rollback for an unusable display setup.
- [ ] Native keyboard/layout/repeat, mouse/touchpad, theme/font/default-app,
  audio and power preferences, or an explicitly supported provider contract.
- [ ] A graphical WM-shortcut editor with conflict detection and reset.
- [ ] Full keyboard traversal, accessible names/roles/states/actions, screen-reader
  navigation, selection announcements, high contrast and large text throughout
  Rill's own custom desktop, menus, panels and dialogs.
- [ ] Complete preference migration beyond the current copied/linked profiles,
  with documented ownership and a reversible reset/export path.

Acceptance: essential settings can be changed without editing configuration
files, survive relogin, and can be reached and operated with a keyboard and screen
reader. Opening an installed control panel does not establish native support.

## 2. Complete panel and window-manager behavior

### Panels, trays and plugins

- [ ] Vertical and deskbar orientation, appropriate item layout and struts.
- [ ] Graphical creation/removal of panels, monitor selection and geometry editing;
  the current panel list is `panels.json` and changes require login/restart.
- [ ] Full native migration of existing Xfce panel/plugin layouts.
- [ ] Independent hosting of unchanged GTK plugins, or explicit supported plugin
  replacements. Compatibility mode still uses the real Xfce panel.
- [ ] StatusNotifier D-Bus menus, submenu/toggle/disabled states and lifecycle
  updates; secondary activation is not a substitute for a complete item menu.
- [ ] More complete audio-device, network, Bluetooth and battery controls.

Acceptance: representative existing panel layouts migrate without losing their
settings; panels follow hotplug and orientation changes; tray menus operate the
real application actions.

### Window manager and compositor

- [ ] Broader transient/group stacking, placement, dialogs, focus-stealing and
  application-startup activation regressions.
- [ ] xfwm4 theme compatibility or a documented migration to Rill themes.
- [ ] Frame pacing/vsync, fullscreen bypass and sustained-load benchmarks.
- [ ] Physical output hotplug, mixed DPI, multi-seat and fullscreen-game testing.

Acceptance: common terminals, editors, browsers, file managers and games keep
correct focus, geometry, stacking and input across workspace/output changes and
WM restarts, with measured rendering behavior.

## 3. Broaden session recovery

- [ ] Non-XSMP unsaved-document handling and clear user interaction when a client
  cannot participate in logout saving.
- [ ] Arbitrary XSMP client restart styles and wider application-restore coverage.
- [ ] Persist/restore application state where applications support it, not merely
  their restart commands and working directories.
- [ ] Better user-facing diagnostics for missing/crashing optional services and
  failed asynchronous application launches.
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
self-copy through a symlink, cancellation cleanup, cross-process clipboard
formats, desktop multiple selection and cut/copy/paste, desktop crash recovery,
XSMP transactions, panels, WM input/shapes/geometry and compositor rendering.

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
