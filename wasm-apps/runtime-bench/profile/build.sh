#!/usr/bin/env bash
# Build the benchmark variants after running ../build.sh.
set -euo pipefail
dir=$(cd "$(dirname "$0")" && pwd)
apps=$(cd "$dir/../.." && pwd)
# The existing standalone build supplies the C port with the comparison workload.
sdk="$apps/sdk/libc"
build="$apps/build"
source_file="$build/deltablue-toitbench1/deltablue.c"
for mode in phases counts timed empty; do
    out="$build/deltablue-profile-$mode"
    mkdir -p "$out"
    cp "$source_file" "$out/deltablue.c"
    cp "$build/deltablue-toitbench1/stdlib.h" "$out/stdlib.h"
    patch -s "$out/deltablue.c" < "$dir/deltablue.c.patch"
    patch -s "$out/stdlib.h" < "$dir/stdlib.h.patch"
    flag=-DPROFILE_PHASES
    case "$mode" in
        counts) flag=-DPROFILE_COUNTS ;;
        timed)  flag=-DPROFILE_TIMED ;;
        empty)  flag=-DPROFILE_EMPTY ;;
    esac
    clang --target=wasm32 -mcpu=mvp -std=gnu17 -O2 -fno-strict-aliasing \
        -Wno-deprecated-non-prototype -nostdlib "$flag" -I"$out" -I"$dir" -I"$sdk" \
        -Wl,--allow-undefined-file="$sdk/libc.syms" -Wl,--no-entry \
        -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--stack-first \
        -Wl,-z,stack-size=2048 -Wl,--initial-memory=65536 -Wl,--max-memory=65536 \
        -Wl,--strip-all -o "$out/deltablue.wasm" "$out/deltablue.c"
done
