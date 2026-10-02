#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/scripts/ziran.sh"}
mkdir -p "$root/build/ziran"
work=$(mktemp -d "$root/build/ziran/shell-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11

# The C consumer has the existing desktop ABI. Its behavior test must pass
# against the replacement, without linking a second shell implementation.
"$ziran" build --target=c --root src -o "$work/abi" src/shell.zi
"${CC:-cc}" -std=c11 -O2 -Iinclude -I"$work/abi" \
    tests/rill_shell_test.c "$work/abi"/*.c -o "$work/abi/test"
"$work/abi/test"

# Current Ziran tests also run from saved checked IR. Native Plan 9 uses
# this same test entry through the Taiji guest gate.
"$ziran" ir --root tests --module-path src -o "$work/ir" tests/shell_test.zi
for input in source ir; do
    if test "$input" = source; then
        "$ziran" build --target=c --root tests --module-path src \
            -o "$work/$input" tests/shell_test.zi
    else
        "$ziran" build --target=c --root "$work/ir" \
            -o "$work/$input" "$work/ir/shell_test.zir"
    fi
    "${CC:-cc}" -std=c11 -O2 -I"$work/$input" "$work/$input"/*.c -o "$work/$input/test"
    "$work/$input/test"
done

if test -e src/rill_shell.c || test -e src/platform_stub.c; then
    echo 'the migrated handwritten C implementations must not return' >&2
    exit 1
fi
