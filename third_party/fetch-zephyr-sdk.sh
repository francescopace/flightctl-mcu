#!/usr/bin/env bash
# For real boards (native_sim needs none of this): the Zephyr SDK's ARM
# toolchain, into third_party/zephyr-sdk (about 2.3 GB, git-ignored), and the
# binary firmware of Infineon's WiFi chips, which the Raspberry Pi Pico W's
# CYW43439 needs (into the Zephyr tree, under Infineon's licence).
# Run after third_party/fetch-zephyr.sh. Uses west: this machine's, or the one
# in the Zephyr build image.
set -euo pipefail
ZEPHYR_SDK_VERSION="${ZEPHYR_SDK_VERSION:-0.17.4}" # what Zephyr v4.3 asks for (SDK_VERSION)
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.29.1}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$ROOT/third_party/zephyr-ws"
SDK="$ROOT/third_party/zephyr-sdk"
[[ -d "$WS/zephyr" ]] || { echo "no Zephyr tree: run third_party/fetch-zephyr.sh" >&2; exit 1; }
if ! command -v west >/dev/null; then
  exec docker run --rm -v "$ROOT:/project" -w /project -e ZEPHYR_SDK_VERSION="$ZEPHYR_SDK_VERSION" \
    "$ZEPHYR_IMAGE" third_party/fetch-zephyr-sdk.sh
fi
git config --global --add safe.directory '*' 2>/dev/null || true
cd "$WS"
if [[ "$(cat "$SDK/sdk_version" 2>/dev/null)" != "$ZEPHYR_SDK_VERSION" ]]; then
  rm -rf "$SDK"
  west sdk install --version "$ZEPHYR_SDK_VERSION" -t arm-zephyr-eabi -d "$SDK"
fi
west blobs fetch hal_infineon
echo "Zephyr SDK $ZEPHYR_SDK_VERSION (arm-zephyr-eabi) in $SDK"
