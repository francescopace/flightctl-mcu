#!/usr/bin/env bash
# Fetches WAMR (WebAssembly Micro Runtime) at a pinned release into
# third_party/wamr (git-ignored), and applies the patches in
# third_party/patches/wamr-*.patch. The firmware builds it as an ESP-IDF
# component, and the Zephyr build from the same tree.
#   wamr-unaligned-select.patch: the fast interpreter's loader rewrites a
#   `select` of 64-bit values with a plain 32-bit store into code that is only
#   2-byte aligned on chips without unaligned access. On a Cortex-M0+ (the
#   Pico W) that is a HardFault while the app loads. The loader's other
#   stores use STORE_U32, which splits them; the patch does the same. Not
#   fixed upstream as of WAMR 2.4.5 and main.
set -euo pipefail
WAMR_TAG="${WAMR_TAG:-WAMR-2.4.5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="$HERE/wamr"
if [[ ! -d "$DIR/.git" ]]; then
  git clone --depth 1 --branch "$WAMR_TAG" https://github.com/bytecodealliance/wasm-micro-runtime "$DIR"
fi
for p in "$HERE"/patches/wamr-*.patch; do
  if git -C "$DIR" apply --reverse --check "$p" 2>/dev/null; then
    continue # already applied
  fi
  git -C "$DIR" apply "$p"
  echo "WAMR: applied $(basename "$p")"
done
echo "WAMR in $DIR ($(git -C "$DIR" describe --tags))"
