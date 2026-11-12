#!/usr/bin/env bash
# Writes the Zephyr build and a device's credentials to a Raspberry Pi Pico W
# over USB, then prints its console.
# Usage: port/zephyr/flash-pico.sh SECRETS_DIR [seconds of console, default 60]
#        [provision.py arguments: --server, --labels, --wifi-ssid ...]
# The board must show up as the RPI-RP2 drive: plug it in with BOOTSEL held
# down, or, when it runs this firmware with the console on USB (port/zephyr/build.sh
# -S cdc-acm-console) or one built with the Pico SDK, PORT=/dev/cu.usbmodemXXXX
# sends it there. macOS only for now (the drive's path).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-pico-usb}"
SECRETS="${1:?usage: port/zephyr/flash-pico.sh SECRETS_DIR [seconds] [provision.py args...]}"
SECONDS_OF_CONSOLE="${2:-60}"
shift; shift || true
DRIVE=/Volumes/RPI-RP2
BIN="$ROOT/port/zephyr/$BUILD_DIR/zephyr/zephyr.bin"
BOOT="$ROOT/port/zephyr/$BUILD_DIR/mcuboot/zephyr/zephyr.bin"
BOARD_FILES=rpi_pico_rp2040_w
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT   # the image holds the enrollment key and the WiFi password
if [[ -f "$BOOT" ]]; then
  # Built with MCUboot (BOARD=rpi_pico/rp2040/w/mcuboot): the bootloader, then
  # the signed application in the first slot, at 64 KB.
  APP="$ROOT/port/zephyr/$BUILD_DIR/zephyr/zephyr/zephyr.signed.bin"
  [[ -f "$APP" ]] || { echo "no $APP" >&2; exit 1; }
  BOARD_FILES=rpi_pico_rp2040_w_mcuboot
  python3 - "$BOOT" "$APP" "$WORK/flash.bin" <<'PY'
import sys
boot, app = (open(p, "rb").read() for p in sys.argv[1:3])
assert len(boot) <= 0x10000, "MCUboot is larger than its partition"
open(sys.argv[3], "wb").write(boot + b"\xff" * (0x10000 - len(boot)) + app)
PY
else
  [[ -f "$BIN" ]] || { echo "no $BIN: BOARD=rpi_pico/rp2040/w BUILD_DIR=$BUILD_DIR port/zephyr/build.sh -S cdc-acm-console" >&2; exit 1; }
  cp "$BIN" "$WORK/flash.bin"
fi
"$ROOT/tools/provision.py" --format zephyr --board "$BOARD_FILES" --secrets "$SECRETS" --image "$WORK/flash.bin" "$@"
python3 "$ROOT/third_party/zephyr-ws/zephyr/scripts/build/uf2conv.py" "$WORK/flash.bin" -c -b 0x10000000 -f 0xe48bff56 \
  -o "$WORK/flash.uf2" >/dev/null
# Ask for the bootloader until the drive is there: a board that restarts in
# a loop only listens at times.
for i in $(seq 1 60); do
  [[ -d "$DRIVE" ]] && break
  if [[ -n "${PORT:-}" && -e "$PORT" ]]; then
    python3 -c "
import serial, sys
try:
    serial.Serial(sys.argv[1], 1200, timeout=1, write_timeout=1).close()
except Exception:
    pass" "$PORT" || true
  fi
  sleep 1
done
[[ -d "$DRIVE" ]] || { echo "no $DRIVE: plug the board in with BOOTSEL held down" >&2; exit 1; }
before="$(ls /dev/cu.usbmodem* 2>/dev/null || true)"
# The drive appears a moment before it can be written.
for i in 1 2 3 4 5 6; do
  cp -X "$WORK/flash.uf2" "$DRIVE/" 2>/dev/null && break
  (( i == 6 )) && { echo "cannot write to $DRIVE" >&2; exit 1; }
  sleep 1
done
echo "written; waiting for the console"
python3 - "$SECONDS_OF_CONSOLE" "$before" <<'PY'
import glob, sys, time
import serial  # pyserial
seconds, before = float(sys.argv[1]), set(sys.argv[2].split())
port, t = None, time.time()
while port is None and time.time() - t < 30:
    new = [p for p in glob.glob("/dev/cu.usbmodem*") if p not in before]
    port = new[0] if new else None
    time.sleep(0.1)
if port is None:
    sys.exit("no new serial port: is the console on USB in this build?")
ser = serial.Serial(port, 115200, timeout=0.5)
ser.dtr = True
print(f"--- {port}", flush=True)
t = time.time()
while time.time() - t < seconds:
    data = ser.read(4096)
    if data:
        sys.stdout.write(data.decode(errors="replace"))
        sys.stdout.flush()
PY
