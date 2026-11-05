#!/usr/bin/env bash
# Flashes the last build for the chip found on PORT, with the host esptool.
# Usage: firmware/flash.sh /dev/cu.usbmodemXXXX [target]
set -euo pipefail
PORT="${1:?serial port}"
TARGET="${2:-}"
if [[ -z "$TARGET" ]]; then
  # "Chip type: ESP32-C3 (QFN32) ..." -> esp32c3
  TARGET=$(esptool -p "$PORT" --after no-reset chip-id 2>/dev/null |
           sed -n 's/^Chip type: *\(ESP32[-A-Z0-9]*\).*/\1/p' | head -1 | tr -d '-' | tr 'A-Z' 'a-z')
  [[ -n "$TARGET" ]] || { echo "cannot detect the chip on $PORT" >&2; exit 1; }
fi
cd "$(dirname "$0")/build-$TARGET"
echo "flashing $TARGET on $PORT"
esptool --chip "$TARGET" -p "$PORT" -b 460800 --before default-reset --after watchdog-reset \
  write-flash @flash_args
