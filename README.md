# Rill

Rill is a Kryon/libdraw desktop shell inspired by rio and XFCE.

The project keeps shell behavior outside Kryon. Kryon provides the UI runtime,
renderer backends, widgets, and reusable platform primitives; Rill owns panels,
launchers, task/window presentation, desktop surfaces, settings, and platform
adapters.

## Current Slice

- Shared shell state model.
- Platform service interface for launcher/task/session operations.
- Linux adapter using plan9port/libdraw for the UI and process launching.
- Stub Plan 9 adapter shape for the native port.
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

Native Plan 9 is intended to use Kryon's native `mkfile` plus Rill's Plan 9
adapter. The source boundary is present; the full native Plan 9 app build is a
follow-up once the shared shell behavior is stable.
