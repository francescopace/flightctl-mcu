#!/usr/bin/env bash
# End-to-end test of the real firmware, run by Espressif's emulator (esp-emu)
# against Flight Control: the ESP-IDF port, NVS, mbedTLS and WAMR on the chip,
# A/B OTA with reboot, signature checks, certificate renewal and rollback.
# The emulator's WiFi soft AP NATs to this machine, so the firmware reaches
# Flight Control and the registry as a board on the LAN would.
#
#   ci/e2e-emu.sh                  deploy Flight Control on kind + registry, then test
#   SKIP_DEPLOY=1 ci/e2e-emu.sh    against the cluster and registry already running
#   CHIP=esp32c5 ci/e2e-emu.sh     another emulated chip (esp32c3, esp32c5, esp32s3)
#   ROLLBACK=0 ci/e2e-emu.sh       skip the rollback step (about 4 minutes)
#
# Needs: esp-emu, docker (ESP-IDF image), flightctl (logged in), kubectl, oras,
# clang + wasm-ld, python3. Builds go to firmware/build-<chip>-e2e*.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source deploy/local-ip.sh
IP="${HOST_IP:-$(local_ip)}"
REGISTRY="${REGISTRY:-$IP:5001}"
CHIP="${CHIP:-esp32c3}"
ROLLBACK="${ROLLBACK:-1}"
CERT_TTL=240
TAG="emu$(date +%s)"
FLEET="emu-e2e"
WORK="$ROOT/build/e2e-emu"   # inside the repo: the ESP-IDF container mounts it
EMU_PID=""
DEVICE=""

log() { printf '\n== %s\n' "$*"; }
fail() {
  echo "FAIL: $*" >&2
  echo "--- emulator log (last 60 lines)" >&2
  grep -v '^\s*$' "$WORK/emu.log" 2>/dev/null | tail -60 >&2 || true
  exit 1
}

cleanup() {
  [[ -n "$EMU_PID" ]] && kill "$EMU_PID" 2>/dev/null || true
  flightctl delete "fleet/$FLEET" >/dev/null 2>&1 || true
  if [[ -n "$DEVICE" ]]; then
    flightctl delete "device/$DEVICE" >/dev/null 2>&1 || true
    flightctl delete "enrollmentrequest/$DEVICE" >/dev/null 2>&1 || true
  fi
  kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS- >/dev/null 2>&1 || true
}
trap cleanup EXIT

# wait_for <seconds> <description> <python expression on the device JSON `d`>
wait_for() {
  local timeout="$1" what="$2" expr="$3" start
  start=$(date +%s)
  while true; do
    if flightctl get "device/$DEVICE" -o json 2>/dev/null | python3 -c "
import json, sys
d = json.load(sys.stdin)
st = d.get('status') or {}
ci = (st.get('systemInfo') or {}).get('customInfo') or {}
apps = st.get('applications') or []
upd = st.get('updated') or {}
sys.exit(0 if ($expr) else 1)" 2>/dev/null; then
      echo "ok: $what ($(( $(date +%s) - start )) s)"
      return 0
    fi
    (( $(date +%s) - start > timeout )) && fail "timed out after ${timeout}s waiting for: $what"
    sleep 3
  done
}

custom() { flightctl get "device/$DEVICE" -o json | python3 -c "import json,sys; print(json.load(sys.stdin)['status']['systemInfo']['customInfo'].get('$1',''))"; }

# fleet <firmware tag> <app tag>
apply_fleet() {
  flightctl apply -f - >/dev/null <<EOF2
apiVersion: flightctl.io/v1beta1
kind: Fleet
metadata:
  name: $FLEET
spec:
  selector:
    matchLabels:
      app: $FLEET
  template:
    spec:
      os:
        image: $REGISTRY/flightctl-mcu/fw:$1
      applications:
        - name: alarm
          appType: container
          image: $REGISTRY/flightctl-mcu/alarm:$2
          envVars:
            TICK_MS: "100"
      config:
        - name: alarm
          inline:
            - path: /etc/fc-app/alarm.json
              content: '{"threshold_c": 40}'
EOF2
}

build() { # build <version> <build dir suffix> [extra config]
  local extra="../build/e2e-emu/endpoint.cfg${3:+;$3}"
  FC_SECRETS_DIR="$WORK/secrets" EXTRA_SDKCONFIG="$extra" BUILD_DIR="build-$CHIP-$2" \
    TARGET="$CHIP" VARIANT=emu FW_VERSION="$1" firmware/build.sh build merge-bin >"$WORK/build-$2.log" 2>&1 \
    || { tail -30 "$WORK/build-$2.log"; fail "firmware build $1"; }
}

if [[ -z "${SKIP_DEPLOY:-}" ]]; then
  log "deploy Flight Control on kind ($IP) and the registry"
  deploy/kind/up.sh "$IP"
  deploy/registry/up.sh
  kubectl wait --timeout=15m -n flightctl --for=condition=Available deployment --all
  for i in $(seq 1 60); do
    curl -sk -o /dev/null "https://api.$IP.nip.io:8443/api/v1/auth/config" && break
    (( i == 60 )) && fail "API not reachable"
    sleep 5
  done
  flightctl login "https://api.$IP.nip.io:8443" --insecure-skip-tls-verify \
    -t "$(kubectl create token flightctl-admin -n flightctl --duration=2h)" >/dev/null
fi

log "server issues ${CERT_TTL}s management certificates"
kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS=$CERT_TTL >/dev/null
kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null

log "credentials, signing key, firmware for $CHIP ($REGISTRY)"
rm -rf "$WORK" && mkdir -p "$WORK"
deploy/enrollment.sh "$WORK/secrets" >/dev/null
deploy/signing/keygen.sh "$WORK/secrets" >/dev/null
export SIGNING_KEY="$WORK/secrets/signing.key"
printf 'CONFIG_FC_SERVER_HOST="agent-api.%s.nip.io"\nCONFIG_FC_SERVER_PORT=8443\nCONFIG_FC_LABELS="app=%s,site=emu"\n' \
  "$IP" "$FLEET" > "$WORK/endpoint.cfg"
build v1 e2e
cp "firmware/build-$CHIP-e2e/merged-binary.bin" "$WORK/flash.bin"
build v2 e2e
firmware/fw-push.py --variant e2e "$REGISTRY/flightctl-mcu/fw:v2.$TAG" "$CHIP" >/dev/null
wasm-apps/build.sh alarm "v2.$TAG" "$REGISTRY" >/dev/null
(cd "wasm-apps/build/alarm-v2.$TAG" && oras push --plain-http --disable-path-validation \
  --annotation "io.flightctl-mcu.e2e=unsigned-$TAG" --config config.json:application/vnd.wasm.config.v0+json \
  "$REGISTRY/flightctl-mcu/alarm:v2.$TAG-unsigned" alarm.wasm:application/vnd.wasm.content.layer.v1+wasm >/dev/null)

log "boot v1 in the emulator"
ESP_EMU="$(command -v esp-emu || echo "$HOME/.local/bin/esp-emu")"
"$ESP_EMU" --chip "$CHIP" --firmware "$WORK/flash.bin" --net user --timeout 2400s >"$WORK/emu.log" 2>&1 &
EMU_PID=$!
for i in $(seq 1 60); do
  DEVICE=$(sed -n 's/.*device name: \([a-z0-9]*\).*/\1/p' "$WORK/emu.log" | head -1)
  [[ -n "$DEVICE" ]] && flightctl get "enrollmentrequest/$DEVICE" >/dev/null 2>&1 && break
  (( i == 60 )) && fail "no enrollment request"
  sleep 2
done
echo "device: $DEVICE"
printf 'devices:\n  - name: %s\n    labels:\n      stage: emu\n' "$DEVICE" > "$WORK/allowlist.yaml"
deploy/auto-approve/auto-approve.py "$WORK/allowlist.yaml"
wait_for 120 "device Online" "st.get('summary', {}).get('status') == 'Online' and ci.get('firmwareVersion') == 'v1'"

log "signed firmware v2 over the air, signed app"
apply_fleet "v2.$TAG" "v2.$TAG"
wait_for 300 "firmware v2 confirmed, app running" \
  "ci.get('firmwareVersion') == 'v2' and upd.get('status') == 'UpToDate' and apps and apps[0]['status'] == 'Running'"
grep -q "confirmed" "$WORK/emu.log" || fail "no firmware confirmation in the log"
echo "heap: free $(custom memFreeBytes), min $(custom memMinFreeBytes), largest internal block $(custom memInternalLargestBlock)"

log "unsigned app refused"
apply_fleet "v2.$TAG" "v2.$TAG-unsigned"
wait_for 120 "unsigned app refused" "upd.get('status') == 'OutOfDate' and 'not signed' in upd.get('info', '')"
apply_fleet "v2.$TAG" "v2.$TAG"
wait_for 120 "back to the signed app" "upd.get('status') == 'UpToDate'"

log "management certificate renewal"
first=$(custom mgmtCertNotAfter)
wait_for $CERT_TTL "certificate renewed" "ci.get('mgmtCertNotAfter', '$first') > '$first'"
expiry=$(python3 -c "import calendar,time; print(calendar.timegm(time.strptime('$first','%Y-%m-%dT%H:%M:%SZ')))")
wait_s=$(( expiry - $(date +%s) + 5 ))
(( wait_s > 0 )) && { echo "waiting ${wait_s}s for the first certificate to expire"; sleep "$wait_s"; }
tx=$(custom netTxBytes)
wait_for 120 "status accepted after the first certificate expired" "int(ci.get('netTxBytes', 0)) > $tx"

if [[ "$ROLLBACK" == 1 ]]; then
  log "broken firmware v3 is rolled back"
  build v3 e2e-broken sdkconfig.broken   # cannot reach Flight Control (wrong port)
  firmware/fw-push.py --variant e2e-broken "$REGISTRY/flightctl-mcu/fw:v3.$TAG" "$CHIP" >/dev/null
  apply_fleet "v3.$TAG" "v2.$TAG"
  wait_for 480 "back on v2, update reported as failed" \
    "ci.get('firmwareVersion') == 'v2' and 'rolled back' in upd.get('info', '')"
fi

log "PASS"
