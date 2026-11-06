#!/usr/bin/env bash
# Fetches WAMR (WebAssembly Micro Runtime) at a pinned release into
# third_party/wamr (git-ignored). The firmware builds it as an ESP-IDF component.
set -euo pipefail
WAMR_TAG="${WAMR_TAG:-WAMR-2.4.5}"
DIR="$(cd "$(dirname "$0")" && pwd)/wamr"
if [[ -d "$DIR/.git" ]]; then
  echo "WAMR already in $DIR ($(git -C "$DIR" describe --tags))"
  exit 0
fi
git clone --depth 1 --branch "$WAMR_TAG" https://github.com/bytecodealliance/wasm-micro-runtime "$DIR"
