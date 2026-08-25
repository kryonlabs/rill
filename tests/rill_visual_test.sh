#!/bin/sh
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
bin=${RILL_BIN:-"$root/build/linux-x86_64/rill"}
plan9=${PLAN9PORT_DIR:-"$root/../plan9port"}
work=${TMPDIR:-/tmp}/rill-visual-test.$$
ready="$work/ready"
shot="$work/root.png"
log="$work/rill.log"
pid=

cleanup()
{
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$work"
}
trap cleanup EXIT INT TERM

need()
{
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "rill visual test: missing required command: $1" >&2
        exit 1
    fi
}

need xvfb-run
need import
need convert
need python3

import_cmd=$(command -v import)
convert_cmd=$(command -v convert)

if [ ! -x "$bin" ]; then
    echo "rill visual test: missing binary: $bin" >&2
    exit 1
fi
if [ ! -x "$plan9/bin/devdraw" ]; then
    echo "rill visual test: missing devdraw: $plan9/bin/devdraw" >&2
    exit 1
fi

mkdir -p "$work"

xvfb-run -a -s "-screen 0 1120x720x24" sh -c '
    set -eu
    PLAN9="$1"
    BIN="$2"
    READY="$3"
    SHOT="$4"
    LOG="$5"
    IMPORT="$6"
    export PLAN9
    export PATH="$PLAN9/bin:$PATH"
    export DEVDRAW="$PLAN9/bin/devdraw"
    export RILL_TEST_SCENE=compositor-stack
    export RILL_TEST_READY_FILE="$READY"
    export RILL_TEST_DISABLE_WALLPAPER=1
    "$BIN" >"$LOG" 2>&1 &
    pid=$!
    i=0
    while [ ! -s "$READY" ] && kill -0 "$pid" 2>/dev/null; do
        i=$((i + 1))
        if [ "$i" -gt 100 ]; then
            echo "rill visual test: timed out waiting for ready frame" >&2
            cat "$LOG" >&2 || true
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            exit 1
        fi
        /bin/sleep 0.05
    done
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "rill visual test: rill exited before capture" >&2
        cat "$LOG" >&2 || true
        wait "$pid" 2>/dev/null || true
        exit 1
    fi
    /bin/sleep 0.15
    "$IMPORT" -window root "$SHOT"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
' sh "$plan9" "$bin" "$ready" "$shot" "$log" "$import_cmd"

test -s "$shot"
raw="$work/root.rgba"
"$convert_cmd" "$shot" -depth 8 rgba:"$raw"

python3 - "$raw" <<'PY'
import sys

raw_path = sys.argv[1]
width = 1120
height = 720
upper = (24, 172, 128)
samples = [
    (300, 230), (340, 230), (420, 230), (520, 230),
    (300, 260), (380, 260), (500, 260), (620, 260),
    (300, 320), (420, 320), (560, 320), (640, 320),
]

with open(raw_path, "rb") as f:
    data = f.read()

if len(data) < width * height * 4:
    raise SystemExit("rill visual test: captured image is smaller than expected")

def pixel(x, y):
    i = (y * width + x) * 4
    return data[i], data[i + 1], data[i + 2]

def near(a, b, tolerance=10):
    return all(abs(a[i] - b[i]) <= tolerance for i in range(3))

bad = []
red_leaks = []
for x, y in samples:
    p = pixel(x, y)
    if p[0] > 150 and p[1] < 90 and p[2] < 100:
        red_leaks.append((x, y, p))
    if not near(p, upper):
        bad.append((x, y, p))

if red_leaks:
    raise SystemExit(f"rill visual test: lower-window red text leaked: {red_leaks[:4]}")
if bad:
    raise SystemExit(f"rill visual test: upper surface is not opaque: {bad[:4]}")
PY

echo "rill visual compositor test ok"
