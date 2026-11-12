#!/usr/bin/env bash
# Writes the Zephyr build and a device's credentials to an ESP32-S3-DevKitC
# over USB, with esptool (the board's UART port).
# Usage: zephyr/flash-esp32.sh PORT SECRETS_DIR [provision.py arguments: --server, --labels, --wifi-ssid ...]
# The build is zephyr/build-esp32s3 (BOARD=esp32s3_devkitc/esp32s3/procpu
# BUILD_DIR=build-esp32s3 zephyr/build.sh), or $BUILD_DIR: MCUboot at the start
# of the flash, the signed application in the first slot.
# ERASE=1 erases the whole flash first: the device's key is in its storage
# partition, so it comes back as a new device.
# Nothing here burns eFuses: Zephyr's MCUboot checks the image's signature in
# software.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${1:?usage: zephyr/flash-esp32.sh PORT SECRETS_DIR [provision.py args...]}"
SECRETS="${2:?usage: zephyr/flash-esp32.sh PORT SECRETS_DIR [provision.py args...]}"
shift 2
BUILD="$ROOT/zephyr/${BUILD_DIR:-build-esp32s3}"
BOOT="$BUILD/mcuboot/zephyr/zephyr.bin"
APP="$BUILD/zephyr/zephyr/zephyr.signed.bin"
[[ -f "$BOOT" && -f "$APP" ]] || { echo "no build in $BUILD: BOARD=esp32s3_devkitc/esp32s3/procpu BUILD_DIR=build-esp32s3 zephyr/build.sh" >&2; exit 1; }
# Zephyr's layout for the board (boards/esp32s3_devkitc_procpu.overlay has the rest).
BOOT_AT=0x0
SLOT0_AT=0x20000
if [[ -n "${ERASE:-}" ]]; then
  esptool --chip esp32s3 -p "$PORT" -b 460800 --after no-reset erase-flash
fi
esptool --chip esp32s3 -p "$PORT" -b 460800 --after no-reset write-flash "$BOOT_AT" "$BOOT" "$SLOT0_AT" "$APP"
"$ROOT/deploy/provision.py" --format zephyr --board esp32s3_devkitc_procpu --secrets "$SECRETS" --port "$PORT" "$@"
