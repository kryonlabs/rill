#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/../../ziranlang/ziran/build/bin/ziran"}
std=${ZIRAN_STD:-"$root/../../ziranlang/ziran/std"}
kryon=${KRYON_DIR:-"$root/../kryon"}
mkdir -p build/ziran
work=$(mktemp -d "$root/build/ziran/run-ui-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11
"$ziran" ir --root tests --module-path app --module-path src \
    --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" \
    --module-path "$std" -o "$work/ir" tests/applications_ui_test.zi
for input in source saved; do
    if test "$input" = source; then
        "$ziran" build --target=c --root tests --module-path app --module-path src \
            --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" \
            --module-path "$std" -o "$work/$input" tests/applications_ui_test.zi
    else
        "$ziran" build --target=c --root "$work/ir" -o "$work/$input" "$work/ir/applications_ui_test.zir"
    fi
    "${CC:-cc}" -std=c11 -O2 -ffunction-sections -fdata-sections \
        -I"$work/$input" "$work/$input"/*.c -o "$work/$input/run" \
        -Wl,--gc-sections -lm -lcairo -ldl
    KRYON_CAPTURE_PATH="$work/$input.png" "$work/$input/run"
done
cmp "$work/source.png" "$work/saved.png"
