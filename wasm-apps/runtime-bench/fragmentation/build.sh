#!/usr/bin/env bash
# Build both signed-module inputs from the same workload and the two measured stdlib.h blobs (v0.9.4 and v0.9.5-rc.1).
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
root=$(git -C "$dir" rev-parse --show-toplevel)
sdk="$root/wasm-apps/sdk/libc"
for kind in first-fit free-list; do
    out="$root/wasm-apps/build/fragmentation-$kind"
    mkdir -p "$out"
    case "$kind" in
        first-fit) header_blob=319ba142e09c514eb5ab44e519e04fef8dc92836 ;;
        free-list) header_blob=2bd5de5c75ee31a865ac2961d6339f4395c3c157 ;;
    esac
    git -C "$root" cat-file blob "$header_blob" > "$out/stdlib.h"
    clang --target=wasm32 -mcpu=mvp -O2 -fno-strict-aliasing -nostdlib \
        -I"$out" -I"$sdk" -Wl,--allow-undefined-file="$sdk/libc.syms" \
        -Wl,--no-entry -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--stack-first \
        -Wl,-z,stack-size=2048 -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
        -Wl,--strip-all -o "$out/fragmentation.wasm" "$dir/fragmentation.c"
done
