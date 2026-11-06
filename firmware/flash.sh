#!/usr/bin/env bash
# Flashes the last build for the chip found on PORT, with the host esptool.
# Usage: firmware/flash.sh /dev/cu.usbmodemXXXX [target]
# NO_STUB=1 talks to the ROM loader directly (slower), for boards where the
# flasher stub upload fails with "Checksum error" (seen on an ESP32-C6 v0.1).
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
cd "$(dirname "$0")/build-$TARGET"
echo "flashing $TARGET on $PORT"
esptool --chip "$TARGET" -p "$PORT" -b 460800 ${NO_STUB:+--no-stub} --before default-reset --after watchdog-reset \
  write-flash @flash_args
