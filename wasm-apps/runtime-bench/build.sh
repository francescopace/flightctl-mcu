#!/usr/bin/env bash
# Builds the two standalone modules used in the ESP32 runtime comparison.
# Pass --freelist to build the experimental DeltaBlue variant from the
# baseline source produced by a previous invocation. Pass --release VERSION
# to build both modules with the current SDK for publication.
# Install one at a time with FC_ENTRY=run. Each returns the median batch
# duration in milliseconds. A batch contains ten benchmark iterations.
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
sdk="$dir/../sdk/libc"
upstream=https://raw.githubusercontent.com/oracle/graal/32823ea977c280a940d169b5899536c318baab16/wasm/benchmarks/interpreter
historical_sdk=41a2302b863928659c62e72faf6d414d83c6facf
case "${1:-}" in
    "") names=(richards deltablue) ;;
    --freelist) names=(deltablue) ;;
    --release) [[ $# == 2 && $2 =~ ^v[0-9][a-zA-Z0-9.-]*$ ]] || exit 2; names=(richards deltablue) ;;
    *) echo "Usage: $0 [--freelist | --release VERSION]" >&2; exit 2 ;;
esac
for name in "${names[@]}"; do
    out="$dir/../build/$name-toitbench1"
    baseline="$out"
    if [[ ${1:-} == --freelist ]]; then out="$dir/../build/deltablue-freelist"; fi
    if [[ ${1:-} == --release ]]; then out="$dir/../build/$name-$2"; fi
    mkdir -p "$out"
    (
        cd "$out"
        source="$name.c"
        includes=(-I"$sdk")
        if [[ ${1:-} != --release ]]; then
            if ! git -C "$dir" show "$historical_sdk:wasm-apps/sdk/libc/stdlib.h" > stdlib.h 2>/dev/null; then
                curl -fsSL "https://raw.githubusercontent.com/francescopace/flightctl-mcu/$historical_sdk/wasm-apps/sdk/libc/stdlib.h" -o stdlib.h
            fi
            includes=(-I"$out" -I"$sdk" -I"$baseline")
        fi
        if [[ ${1:-} == --freelist ]]; then
            source="$dir/freelist.c"
        else
            curl -fsSL "$upstream/$name.c" -o "$name.c"
            patch -s "$name.c" < "$dir/$name.patch"
        fi
        clang --target=wasm32 -mcpu=mvp -std=gnu17 -O2 -fno-strict-aliasing \
            -Wno-deprecated-non-prototype -nostdlib "${includes[@]}" \
            -Wl,--allow-undefined-file="$sdk/libc.syms" -Wl,--no-entry \
            -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--stack-first \
            -Wl,-z,stack-size=2048 -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
            -Wl,--strip-all -o "$name.wasm" "$source"
    )
done
