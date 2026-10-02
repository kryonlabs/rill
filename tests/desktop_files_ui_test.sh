#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ziran=${ZIRAN:-"$root/scripts/ziran.sh"}
lock_flags=
if [ ! -f "$root/ziran.local.toml" ]; then lock_flags=--locked; fi
std=${ZIRAN_STD:-"$("$ziran" pkg path ziran $lock_flags)/std"}
kryon=${KRYON_DIR:-"$("$ziran" pkg path kryon $lock_flags)"}
mkdir -p build/ziran
work=$(mktemp -d "$root/build/ziran/desktop-files-ui-test.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
unset DISPLAY WAYLAND_DISPLAY RILL_CONTAINED_X11
"$ziran" ir --root tests --module-path app --module-path src \
    --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" \
    --module-path "$std" -o "$work/ir" tests/desktop_files_ui_test.zi
for input in source saved; do
    mkdir "$work/$input-data"
    cp "$kryon/icons/ui.png" "$work/$input-data/icon1.png"
    cp "$kryon/icons/language.png" "$work/$input-data/icon2.png"
    if test "$input" = source; then
        "$ziran" build --target=c --root tests --module-path app --module-path src \
            --module-path "$kryon/src/ui" --module-path "$kryon/src/backend" \
            --module-path "$std" -o "$work/$input" tests/desktop_files_ui_test.zi
    else
        "$ziran" build --target=c --root "$work/ir" -o "$work/$input" "$work/ir/desktop_files_ui_test.zir"
    fi
    "${CC:-cc}" -std=c11 -O2 -ffunction-sections -fdata-sections \
        -I"$work/$input" "$work/$input"/*.c -o "$work/$input/run" \
        -Wl,--gc-sections -lm -lcairo -lfreetype -ldl
    RILL_TEST_ROOT="$work/$input-data" KRYON_CAPTURE_PATH="$work/$input.png" "$work/$input/run"
done
cmp "$work/source.png" "$work/saved.png"
cp "$work/source.png" build/rill-desktop-files.png
