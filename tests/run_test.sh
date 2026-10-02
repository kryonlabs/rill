#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/scripts/ziran.sh"}
lock_flags=
if [ ! -f "$root/ziran.local.toml" ]; then lock_flags=--locked; fi
std=${ZIRAN_STD:-"$("$ziran" pkg path ziran $lock_flags)/std"}
mkdir -p build/ziran
work=$(mktemp -d "$root/build/ziran/run-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11
"$ziran" ir --root tests --module-path src --module-path "$std" -o "$work/ir" tests/run_test.zi
for input in source saved; do
    mkdir "$work/$input-data"
    if test "$input" = source; then
        "$ziran" build --target=c --root tests --module-path src --module-path "$std" \
            -o "$work/$input" tests/run_test.zi
    else
        "$ziran" build --target=c --root "$work/ir" -o "$work/$input" "$work/ir/run_test.zir"
    fi
    "${CC:-cc}" -std=c11 -O2 -I"$work/$input" "$work/$input"/*.c -o "$work/$input/run"
    RILL_TEST_ROOT="$work/$input-data" "$work/$input/run"
done
