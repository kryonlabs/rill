#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bin=${RILL_BIN:-$root/build/linux-x86_64/rill}
plan9=${PLAN9PORT_DIR:-$root/../plan9port}
work=$(mktemp -d /tmp/rill-xfce.XXXXXX)
trap 'if [ "${RILL_TEST_KEEP_ARTIFACTS:-0}" = 1 ]; then echo "Artifacts: $work"; else rm -rf "$work"; fi' EXIT HUP INT TERM
for command in xfwm4 xfce4-panel xfconf-query dbus-run-session xvfb-run xwininfo xprop xwd xdotool xmessage; do
    command -v "$command" >/dev/null
done
mkdir -p "$work/config/xfce4/xfconf/xfce-perchannel-xml"
cp /etc/xdg/xfce4/panel/default.xml "$work/config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml"
export XDG_CONFIG_HOME="$work/config" XDG_CACHE_HOME="$work/cache"
mkdir -p "$work/data/applications"
cat > "$work/data/applications/rill-smoke.desktop" <<'ENTRY'
[Desktop Entry]
Type=Application
Name=Launch Test
Exec=xmessage Rill-launch-test
Icon=utilities-terminal
X-Rill-Favorite=true
ENTRY
export XDG_DATA_HOME="$work/data"
export XDG_CURRENT_DESKTOP=XFCE
export PLAN9="$plan9" DEVDRAW="$plan9/bin/devdraw"
export PATH="$plan9/bin:$PATH"
dbus-run-session -- xvfb-run -a -s '-screen 0 1280x800x24' sh -eu -c '
    xfwm4 --sm-client-disable >"$2/wm.log" 2>&1 &
    wm=$!
    rill=
    cleanup() {
        timeout 5 xfce4-panel --quit >/dev/null 2>&1 || true
        if [ -n "$rill" ]; then kill "$rill" 2>/dev/null || true; wait "$rill" 2>/dev/null || true; fi
        kill "$wm" 2>/dev/null || true
        wait "$wm" 2>/dev/null || true
    }
    trap cleanup EXIT HUP INT TERM
    sleep 1
    RILL_TEST_READY_FILE="$2/ready" "$1" --desktop --xfce-panel >"$2/rill.log" 2>&1 &
    rill=$!
    for i in 1 2 3 4 5 6 7 8 9 10; do
        test -s "$2/ready" && break
        kill -0 "$rill"
        sleep 1
    done
    test -s "$2/ready"
    xwininfo -root -tree >"$2/tree"
    id=$(awk '\''$2 == "\"Rill\":" {print $1; exit}'\'' "$2/tree")
    test -n "$id"
    xprop -id "$id" _NET_WM_WINDOW_TYPE >"$2/type"
    grep -q _NET_WM_WINDOW_TYPE_DESKTOP "$2/type"
    for i in 1 2 3 4 5; do
        xwininfo -root -tree >"$2/tree"
        grep -q xfce4-panel "$2/tree" && break
        sleep 1
    done
    grep -q xfce4-panel "$2/tree"
    xfconf-query -c xfce4-panel -lv >"$2/plugins"
    grep -q clock "$2/plugins"
    xwd -root -silent -out "$2/before.xwd"
    xdotool mousemove 68 80
    sleep 0.2
    xdotool mousedown 1
    sleep 0.2
    xdotool mouseup 1
    for i in 1 2 3 4 5; do
        xwininfo -root -tree >"$2/tree"
        grep -q xmessage "$2/tree" && break
        sleep 1
    done
    grep -q xmessage "$2/tree"
    xwd -root -silent -out "$2/desktop.xwd"
    if [ -n "${RILL_XFCE_SCREENSHOT:-}" ]; then
        convert "$2/desktop.xwd" "$RILL_XFCE_SCREENSHOT"
    fi
' sh "$bin" "$work" || {
    cat "$work/rill.log" "$work/wm.log" "$work/tree" "$work/type" 2>/dev/null || true
    exit 1
}
