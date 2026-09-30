#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/../ziran/build/bin/ziran"}
std=${ZIRAN_STD:-"$root/../ziran/std"}
mkdir -p "$root/build/ziran"
work=$(mktemp -d "$root/build/ziran/persistence-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11

"$ziran" build --target=c --root src --module-path "$std" \
    -o "$work/abi" src/panel.zi src/settings.zi
"${CC:-cc}" -std=c11 -D_GNU_SOURCE -O2 -Iinclude -I"$work/abi" \
    tests/rill_platform_test.c src/platform_plan9.c "$work/abi"/*.c -o "$work/abi/test"
"$work/abi/test"

"$ziran" ir --root tests --module-path src --module-path "$std" \
    -o "$work/ir" tests/persistence_test.zi
for input in source ir; do
    if test "$input" = source; then
        "$ziran" build --target=c --root tests --module-path src --module-path "$std" \
            -o "$work/$input" tests/persistence_test.zi
    else
        "$ziran" build --target=c --root "$work/ir" \
            -o "$work/$input" "$work/ir/persistence_test.zir"
    fi
    "${CC:-cc}" -std=c11 -D_GNU_SOURCE -O2 -I"$work/$input" "$work/$input"/*.c -o "$work/$input/test"
    mkdir -p "$work/$input/data"
    RILL_TEST_ROOT="$work/$input/data" "$work/$input/test"
done

if test -e src/rill_panel.c || test -e src/rill_settings.c; then
    echo 'the migrated handwritten C persistence implementations must not return' >&2
    exit 1
fi
echo 'rill-persistence: legacy ABI, Ziran source and saved IR passed'
