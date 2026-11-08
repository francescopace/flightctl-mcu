#!/usr/bin/env bash
# Builds a WASM app into wasm-apps/build/<app>-<version>/, then pushes and
# signs it with wasm-apps/push.sh.
# Needs clang with the wasm32 target (Apple clang works) and wasm-ld (brew install lld).
# Usage: wasm-apps/build.sh alarm v2 [REGISTRY]
#   NO_PUSH=1 only builds. See push.sh for REGISTRY, signing and NO_SIGN.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../deploy/local-ip.sh"
APP="${1:?app}"; VER="${2:?version (v1, v2, ...)}"; REG="${3:-${REGISTRY:-$(local_ip):5001}}"
OUT="$DIR/build/$APP-$VER"
mkdir -p "$OUT"
# Small module for MCUs: 1 KB stack placed first, no libc, and __data_end +
# __heap_base exported so WAMR shrinks the 64 KB linear memory page to what
# the module really uses (a few KB), which the C3/C5 heap can satisfy.
# APP_VERSION is the major version from the tag (v2.1 -> 2), for apps that
# build several behaviours from one source (alarm).
clang --target=wasm32 -O2 -nostdlib -I"$DIR/sdk" -DAPP_VERSION="$(echo "${VER#v}" | cut -d. -f1 | tr -cd '0-9')" \
  -Wl,--no-entry -Wl,--export=__heap_base -Wl,--export=__data_end -Wl,--stack-first -Wl,-z,stack-size=1024 \
  -Wl,--initial-memory=65536 -Wl,--max-memory=65536 -Wl,--strip-all \
  -o "$OUT/$APP.wasm" "$DIR/$APP/$APP.c"
echo "$APP $VER: $(wc -c < "$OUT/$APP.wasm" | tr -d ' ') bytes"
[[ -n "${NO_PUSH:-}" ]] && exit 0
"$DIR/push.sh" "$APP" "$VER" "$VER" "$REG"
