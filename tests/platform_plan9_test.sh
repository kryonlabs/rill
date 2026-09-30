#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/../ziran/build/bin/ziran"}
std=${ZIRAN_STD:-"$root/../ziran/std"}
mkdir -p build/ziran
work=$(mktemp -d "$root/build/ziran/platform-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11
"$ziran" ir --root tests --module-path src --module-path "$std" \
    -o "$work/ir" tests/platform_plan9_test.zi
for input in source ir; do
    if test "$input" = source; then
        "$ziran" build --target=c --root tests --module-path src --module-path "$std" \
            -o "$work/$input" tests/platform_plan9_test.zi
    else
        "$ziran" build --target=c --root "$work/ir" \
            -o "$work/$input" "$work/ir/platform_plan9_test.zir"
    fi
    "${CC:-cc}" -std=c11 -D_GNU_SOURCE -O2 -I"$work/$input" "$work/$input"/*.c -o "$work/$input/test"
    mkdir "$work/$input/data"
    RILL_TEST_ROOT="$work/$input/data" "$work/$input/test"
done
test ! -e src/platform_plan9.c
echo 'rill-platform: source and saved IR passed'
