#!/usr/bin/env bash
# The Zephyr build's field provisioning (core/src/fc_prov.c), against
# Espressif's own installer tool: build/host/fc-prov-host serves it on this
# machine with a pretend radio, tests/e2e/esp-prov.sh talks to it as it does to
# an ESP-IDF device's access point.
#   1. a wrong proof of possession: refused before anything is sent;
#   2. a wrong WiFi password: the installer is told, the device waits for another;
#   3. the right one, picked from the device's scan: joined, and the server returns;
#   4. a radio that cannot join with its access point up (--later): the server
#      returns once the network is applied.
# Needs build/host (cmake), and Python with protobuf and cryptography, which
# are installed in build/prov-venv when missing.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PORT="${PORT:-8090}"
POP=prov-test-pop-1
BIN="$ROOT/build/host/fc-prov-host"
[[ -x "$BIN" ]] || { echo "no $BIN: build the host agent first (cmake --build build/host)" >&2; exit 1; }
if ! python3 -c 'import google.protobuf, cryptography' 2>/dev/null; then
  [[ -x "$ROOT/build/prov-venv/bin/python3" ]] || python3 -m venv "$ROOT/build/prov-venv"
  "$ROOT/build/prov-venv/bin/pip" install -q --disable-pip-version-check protobuf cryptography
  export PATH="$ROOT/build/prov-venv/bin:$PATH"
fi
export ESP_PROV_LOCAL=1
LOG="$(mktemp)"; OUT="$(mktemp)"
trap 'kill "${PID:-}" 2>/dev/null || true; rm -f "$LOG" "$OUT"' EXIT
prov() { "$ROOT/tests/e2e/esp-prov.sh" "127.0.0.1:$PORT" "$@" > "$OUT" 2>&1 || true; }
expect() { grep -q -- "$1" "$OUT" || { echo "FAIL: $2"; cat "$OUT"; echo "--- device:"; cat "$LOG"; exit 1; }; echo "ok: $2"; }
wait_up() { for _ in $(seq 50); do grep -q "waiting for an installer" "$LOG" && return; sleep 0.1; done; cat "$LOG"; exit 1; }

"$BIN" --pop "$POP" --port "$PORT" > "$LOG" 2>&1 & PID=$!
wait_up
prov --pop wrong-pop-0000 --ssid fc-test-site --passphrase fc-test-pass
expect "Error in establishing session" "a wrong proof of possession is refused"
grep -q "wrong proof of possession" "$LOG"
prov --pop "$POP" --ssid fc-test-site --passphrase wrong-password
expect "Failure reason: Incorrect Password" "a wrong WiFi password is reported to the installer"
printf '1\n' | prov --pop "$POP" --passphrase fc-test-pass
expect "fc-test-other" "the installer sees the device's scan"
expect "Provisioning was successful" "the network picked from the scan is joined"
wait "$PID"
grep -q 'network "fc-test-site" (joined)' "$LOG" || { cat "$LOG"; exit 1; }

"$BIN" --pop "$POP" --port "$PORT" --later > "$LOG" 2>&1 & PID=$!
wait_up
prov --pop "$POP" --ssid fc-test-site --passphrase fc-test-pass
wait "$PID"
grep -q 'network "fc-test-site" (to join)' "$LOG" || { cat "$LOG"; exit 1; }
echo "ok: a radio that cannot join with its access point up gets the network to try"
PID=""
echo "field provisioning: all passed"
