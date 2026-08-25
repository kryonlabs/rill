# Rill

Rill is a Kryon/libdraw desktop shell inspired by rio and XFCE.

The project keeps shell behavior outside Kryon. Kryon provides the UI runtime,
renderer backends, widgets, and reusable platform primitives; Rill owns panels,
launchers, task/window presentation, desktop surfaces, settings, and platform
adapters.

## Current Slice

- Shared shell state model.
- Platform service interface for launcher/task/session operations.
- Linux adapter using plan9port/libdraw for the UI and process launching,
  without direct X11 task-management calls.
- Plan 9 adapter using native launcher commands and `/dev/wctl`.
- XFCE-style top panel with Applications, Places, System, quick launchers,
  task buttons, tray indicators, and clock.
- Desktop icons for Home, Terminal, and Settings.
- Kapsule vendored as `vendor/kapsule` and built as the real libdraw terminal
  launched from Rill.

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

Run Rill under plan9port/devdraw:

```sh
make run
```

Native Plan 9 uses Kryon's native `mkfile` plus Rill's Plan 9 adapter. In
Taiji, `/sys/src/cmd/rill` installs the system desktop command and keeps the
rio-compatible `/dev` and `/mnt/wsys` contract for native graphical clients.
