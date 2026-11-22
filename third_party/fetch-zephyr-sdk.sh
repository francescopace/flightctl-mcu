#!/usr/bin/env bash
# For real boards (native_sim needs none of this): the Zephyr SDK's toolchains
# for them, into third_party/zephyr-sdk (git-ignored), and the binary firmware
# of their WiFi chips into the Zephyr tree. The ESP32-S3 uses Espressif's
# firmware and the Pico 2 W uses Infineon's.
# Run after third_party/fetch-zephyr.sh. Uses west: this machine's, or the one
# in the Zephyr build image.
set -euo pipefail
ZEPHYR_SDK_VERSION="${ZEPHYR_SDK_VERSION:-1.0.1}" # what Zephyr v4.5 asks for (SDK_VERSION)
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.30.0}"
# Arm Cortex-M and the ESP32-S3 (Xtensa LX7).
TOOLCHAINS="${ZEPHYR_SDK_TOOLCHAINS:-arm-zephyr-eabi xtensa-espressif_esp32s3_zephyr-elf}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WS="$ROOT/third_party/zephyr-ws"
SDK="$ROOT/third_party/zephyr-sdk"
[[ -d "$WS/zephyr" ]] || { echo "no Zephyr tree: run third_party/fetch-zephyr.sh" >&2; exit 1; }
if ! command -v west >/dev/null; then
  exec docker run --rm -v "$ROOT:/project" -w /project -e ZEPHYR_SDK_VERSION="$ZEPHYR_SDK_VERSION" \
    -e ZEPHYR_SDK_TOOLCHAINS="$TOOLCHAINS" -e BOARD="${BOARD:-}" \
    "$ZEPHYR_IMAGE" third_party/fetch-zephyr-sdk.sh
fi
git config --global --add safe.directory '*' 2>/dev/null || true
cd "$WS"
if [[ "$(cat "$SDK/sdk_version" 2>/dev/null)" != "$ZEPHYR_SDK_VERSION" ]]; then
  rm -rf "$SDK"
  west sdk install --version "$ZEPHYR_SDK_VERSION" -t ${TOOLCHAINS%% *} -d "$SDK"
fi
# A toolchain added to the list since the SDK was installed.
for t in $TOOLCHAINS; do
  [[ -d "$SDK/gnu/$t" ]] || "$SDK/setup.sh" -t "$t"
done
# Espressif's WiFi libraries, for a build with its toolchain.
[[ -d "$WS/modules/hal/espressif" && "$TOOLCHAINS" == *xtensa* ]] && west blobs fetch hal_espressif
if [[ "${BOARD:-}" == rpi_pico2/*/w || "${BOARD:-}" == rpi_pico2/*/w/mcuboot ]]; then
  west blobs fetch hal_infineon
elif [[ "${BOARD:-}" == nrf7002dk/nrf5340/cpuapp ]]; then
  west blobs fetch nrf_wifi
fi
echo "Zephyr SDK $ZEPHYR_SDK_VERSION ($TOOLCHAINS) in $SDK"
