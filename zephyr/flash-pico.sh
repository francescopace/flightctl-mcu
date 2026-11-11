#!/usr/bin/env bash
# Writes the Zephyr build and a device's credentials to a Raspberry Pi Pico W
# over USB, then prints its console.
# Usage: zephyr/flash-pico.sh SECRETS_DIR [seconds of console, default 60]
#        [provision.py arguments: --server, --labels, --wifi-ssid ...]
# The board must show up as the RPI-RP2 drive: plug it in with BOOTSEL held
# down, or, when it runs this firmware with the console on USB (zephyr/build.sh
# -S cdc-acm-console) or one built with the Pico SDK, PORT=/dev/cu.usbmodemXXXX
# sends it there. macOS only for now (the drive's path).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-build-pico-usb}"
SECRETS="${1:?usage: zephyr/flash-pico.sh SECRETS_DIR [seconds] [provision.py args...]}"
SECONDS_OF_CONSOLE="${2:-60}"
shift; shift || true
DRIVE=/Volumes/RPI-RP2
BIN="$ROOT/zephyr/$BUILD_DIR/zephyr/zephyr.bin"
[[ -f "$BIN" ]] || { echo "no $BIN: BOARD=rpi_pico/rp2040/w BUILD_DIR=$BUILD_DIR zephyr/build.sh -S cdc-acm-console" >&2; exit 1; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT   # the image holds the enrollment key and the WiFi password
cp "$BIN" "$WORK/flash.bin"
"$ROOT/deploy/provision.py" --format zephyr --board rpi_pico_rp2040_w --secrets "$SECRETS" --image "$WORK/flash.bin" "$@"
python3 "$ROOT/third_party/zephyr-ws/zephyr/scripts/build/uf2conv.py" "$WORK/flash.bin" -c -b 0x10000000 -f 0xe48bff56 \
  -o "$WORK/flash.uf2" >/dev/null
if [[ ! -d "$DRIVE" && -n "${PORT:-}" ]]; then
  stty -f "$PORT" 1200 || true
fi
for i in $(seq 1 30); do [[ -d "$DRIVE" ]] && break; sleep 1; done
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
