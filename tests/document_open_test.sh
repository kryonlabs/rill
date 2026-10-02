#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/scripts/ziran.sh"}
lock_flags=
if [ ! -f "$root/ziran.local.toml" ]; then lock_flags=--locked; fi
std=${ZIRAN_STD:-"$("$ziran" pkg path ziran $lock_flags)/std"}
mkdir -p build/ziran
work=$(mktemp -d "$root/build/ziran/document-open-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11
"$ziran" ir --root tests --module-path src --module-path "$std" -o "$work/ir" tests/document_open_test.zi
for input in source saved; do
    entry=tests/document_open_test.zi
    source_root=tests
    if test "$input" = saved; then source_root=$work/ir; entry=$source_root/document_open_test.zir; fi
    "$ziran" build --target=c --root "$source_root" --module-path src --module-path "$std" -o "$work/$input" "$entry"
    "${CC:-cc}" -std=c11 -D_GNU_SOURCE -O2 -I"$work/$input" "$work/$input"/*.c -o "$work/$input/run"
    mkdir "$work/$input/data"
    RILL_TEST_ROOT="$work/$input/data" "$work/$input/run"
done
