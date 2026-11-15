#!/usr/bin/env bash
# Fetches CoreMark as it is published, unmodified, for wasm-apps/push.sh:
#   wasm3          wasm3's build (source/extra/coremark_minimal.wasm.h), 7.7 KB,
#                  WebAssembly MVP, env.clock_ms as an i32, 64 KB of memory
#   wasm-coremark  wasm-coremark's minimal build, 12.5 KB, tail calls,
#                  env.clock_ms as an i64, 128 KB of memory
# Each is checked against its SHA-256 and written to
# wasm-apps/build/coremark-<variant>/coremark.wasm. Both export run(), which
# runs for at least 10 s and returns the score as a float: the fleet gives the
# app FC_ENTRY=run (docs/APPS.md).
# Usage: wasm-apps/coremark/fetch.sh [wasm3|wasm-coremark]...   (default: both)
#   then: wasm-apps/push.sh coremark wasm3 [tag] [REGISTRY]
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
fetch() {
    local variant="$1" url sha out="$DIR/build/coremark-$1"
    case "$variant" in
    wasm3)
        url=https://raw.githubusercontent.com/wasm3/wasm3/main/source/extra/coremark_minimal.wasm.h
        sha=1a4abcbf5f98089668d620aae935d53409d9f2bb5e685433f6d13f01de69cce4 ;;
    wasm-coremark)
        url=https://wasm3.github.io/wasm-coremark/coremark-minimal.wasm
        sha=054c0e85e211761087f33372cb5415eab09989842813c2d6dee13c316ccc754b ;;
    *) echo "unknown variant $variant (wasm3, wasm-coremark)" >&2; return 1 ;;
    esac
    mkdir -p "$out"
    curl -sSfL -o "$out/download" "$url"
    if [[ "$url" == *.h ]]; then # a C array of the module's bytes
        python3 -I -c '
import re, sys
src = open(sys.argv[1]).read()
body = src[src.index("{") + 1:src.rindex("}")]
open(sys.argv[2], "wb").write(bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body)))' \
            "$out/download" "$out/coremark.wasm"
    else
        mv "$out/download" "$out/coremark.wasm"
    fi
    rm -f "$out/download"
    local got
    got="$(shasum -a 256 "$out/coremark.wasm" | cut -d' ' -f1)"
    if [[ "$got" != "$sha" ]]; then
        echo "coremark $variant: SHA-256 $got, expected $sha (the published file changed)" >&2
        rm -f "$out/coremark.wasm"
        return 1
    fi
    echo "coremark $variant: $(wc -c < "$out/coremark.wasm" | tr -d ' ') bytes in $out"
}
[[ $# -gt 0 ]] || set -- wasm3 wasm-coremark
for v in "$@"; do
    fetch "$v"
done
