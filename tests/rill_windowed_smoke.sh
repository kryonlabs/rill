#!/bin/sh
set -eu

bin=${RILL_BIN:-build/linux-x86_64/rill}
plan9=${PLAN9PORT_DIR:-/mnt/storage/Projects/plan9port}
root=$(mktemp -d /tmp/rill-windowed-smoke.XXXXXX)
log=$root/rill.log
display_file=$root/display
shot=$root/root.xwd
png=$root/root.png
tree=$root/inner-tree.txt

cleanup()
{
    rm -rf "$root"
}
trap cleanup EXIT INT TERM

command -v Xvfb >/dev/null
command -v xvfb-run >/dev/null
command -v xmessage >/dev/null
command -v xwd >/dev/null
command -v xwininfo >/dev/null
command -v convert >/dev/null

xvfb-run -a -s "-screen 0 1280x1024x24" sh -c '
    env PLAN9="$1" \
        PATH="$1/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
        DEVDRAW="$1/bin/devdraw" \
        RILL_X11_DISPLAY_FILE="$5" \
        RILL_TEST_EXIT_AFTER_FRAMES=300 \
        "$0" --windowed "xmessage contained" >"$2" 2>&1 &
    pid=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do
        test -s "$5" && break
        sleep 1
    done
    inner_display=$(cat "$5")
    for i in 1 2 3 4 5 6 7 8 9 10; do
        DISPLAY="$inner_display" xwininfo -root -tree >"$6" 2>/dev/null || true
        grep -q "xmessage" "$6" && break
        sleep 1
    done
    grep -q "xmessage" "$6"
    xwd -root -silent -out "$3"
    wait "$pid"
    convert "$3" "$4"
' "$bin" "$plan9" "$log" "$shot" "$png" "$display_file" "$tree"

test -s "$png"
if grep -Eqi 'BadWindow|BadAccess|failed to open|Could not claim' "$log"; then
    cat "$log"
    exit 1
fi
