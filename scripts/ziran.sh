#!/bin/sh
# Use the canonical local toolchain or the version recorded by ziran.lock.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"

if [ -f ziran.local.toml ]; then
    local_root=$(python3 -c 'import tomllib; print(tomllib.load(open("ziran.local.toml", "rb")).get("overrides", {}).get("ziran", ""))')
    if [ -n "$local_root" ]; then
        exec "$local_root/build/bin/ziran" "$@"
    fi
fi

launcher=${ZIRAN_LAUNCHER:-ziran}
if [ -f ziran.local.toml ]; then
    toolchain=$("$launcher" pkg path ziran)
else
    toolchain=$("$launcher" pkg path ziran --locked)
fi
if [ ! -x "$toolchain/build/bin/ziran" ]; then
    printf 'rill: build the package toolchain first: make -C %s build/bin/ziran\n' "$toolchain" >&2
    exit 1
fi
exec "$toolchain/build/bin/ziran" "$@"
