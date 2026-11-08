#!/usr/bin/env bash
# Flashes the last build for the chip found on PORT, with the host esptool.
# Usage: firmware/flash.sh /dev/cu.usbmodemXXXX [target]
# NO_STUB=1 talks to the ROM loader directly (slower), for boards where the
# flasher stub upload fails with "Checksum error" (seen on an ESP32-C6 v0.1).
# BUILD=build-<target>-<variant> flashes another build. Secure builds are
# refused unless BURN_EFUSES names the port (see below).
set -euo pipefail
PORT="${1:?serial port}"
TARGET="${2:-}"
if [[ -z "$TARGET" ]]; then
  # "Chip type: ESP32-C3 (QFN32) ..." -> esp32c3
  TARGET=$(esptool -p "$PORT" --after no-reset chip-id 2>/dev/null |
           sed -n 's/^Chip type: *\(ESP32[-A-Z0-9]*\).*/\1/p' | head -1 | tr -d '-' | tr 'A-Z' 'a-z')
  [[ -n "$TARGET" ]] || { echo "cannot detect the chip on $PORT" >&2; exit 1; }
  # Classic ESP32 reports its package (ESP32-D0WD, ESP32-PICO-...).
  case "$TARGET" in esp32d*|esp32pico*|esp32u*) TARGET=esp32 ;; esac
fi
BUILD="${BUILD:-build-$TARGET}"
cd "$(dirname "$0")/$BUILD"
# A secure build burns the chip's eFuses on first boot, for good: the chip
# then runs only images signed with that build's key. Never by accident.
if grep -qE '^CONFIG_SECURE_(BOOT|FLASH_ENC_ENABLED)=y' sdkconfig; then
  if [[ "${BURN_EFUSES:-}" != "$PORT" ]]; then
    echo "$BUILD enables Secure Boot / flash encryption: booting it burns eFuses on the chip, irreversibly." >&2
    echo "See docs/SECURITY.md. To go ahead: BURN_EFUSES=$PORT $0 $*" >&2
    exit 1
  fi
  echo "flashing $BUILD (secure: bootloader included, eFuses are burned on first boot)"
  esptool --chip "$TARGET" -p "$PORT" -b 460800 ${NO_STUB:+--no-stub} --before default-reset --after watchdog-reset \
    write-flash --flash-mode keep --flash-size keep --flash-freq keep 0x0 merged-binary.bin
  exit 0
fi
echo "flashing $TARGET on $PORT"
esptool --chip "$TARGET" -p "$PORT" -b 460800 ${NO_STUB:+--no-stub} --before default-reset --after watchdog-reset \
  write-flash @flash_args
