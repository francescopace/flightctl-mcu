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
#   PREBUILT=1 ci/e2e-emu.sh       use firmware/build-<chip>{,-release,-broken} and
#                                  wasm-apps/build/alarm-v2 as they are (CI: the firmware
#                                  and WASM jobs' builds), instead of building
#   SECURE=1 ci/e2e-emu.sh         the secure variant (firmware/sdkconfig.secure): Secure
#                                  Boot v2, flash encryption (release mode) and NVS
#                                  encryption, burned into the emulated chip's eFuses on
#                                  first boot; builds in build-<chip>-secure{,-release,-broken}.
#                                  Also checks that firmware signed with another key is
#                                  refused. SECURE_BOOT_KEY: the signing key of prebuilt
#                                  images is not needed (a test key is made to build).
#
# The secure run also checks anti-rollback: the release and broken builds have
# security version 1, the debug build 0, which the device refuses once it has
# confirmed the release. And, when it builds the images, Secure Boot key
# rotation: the bootloader is signed with a second key, and at the end a
# release signed with that key alone retires the first one.
#
# A valid image of the other security variant is refused too: a plain device
# gets the secure release build (PREBUILT: firmware/build-<chip>-secure-release
# must be there), a secure device the plain release build signed with its own
# key (only when building: prebuilt images come without their key).
#
# The firmware has no credentials in it: the test provisions the emulated
# device's fc_creds partition (deploy/provision.py) for this cluster, so the
# same images as on real boards are tested. It boots the debug build, updates
# it to the release build over the air, then tries the broken build.
#
# Needs: esp-emu, docker (ESP-IDF image), flightctl (logged in), kubectl, oras,
# clang + wasm-ld, python3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source deploy/local-ip.sh
IP="${HOST_IP:-$(local_ip)}"
REGISTRY="${REGISTRY:-$IP:5001}"
CHIP="${CHIP:-esp32c3}"
ROLLBACK="${ROLLBACK:-1}"
SECURE="${SECURE:-0}"
if [[ "$SECURE" == 1 ]]; then
  DEBUG_V=secure; RELEASE_V=secure-release; BROKEN_V=secure-broken
else
  DEBUG_V=""; RELEASE_V=release; BROKEN_V=broken
fi
CERT_TTL=120
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

build() { # build <build dir> <version> [variant] [security version]
  BUILD_DIR="$1" TARGET="$CHIP" VARIANT="${3:-}" FW_VERSION="$2" SECURE_BOOT_KEY="${SECURE_BOOT_KEY:-}" \
    SECURE_VERSION="${4:-}" firmware/build.sh build merge-bin >"$WORK/$1.log" 2>&1 ||
    { tail -30 "$WORK/$1.log"; fail "firmware build $1"; }
  if [[ "${3:-}" == secure* ]]; then
    # The bootloader also trusts a second key, for the key rotation step.
    python3 firmware/merge-secure.py ${NEXT_KEY:+--extra-key "$NEXT_KEY"} "firmware/$1" >>"$WORK/$1.log" 2>&1 ||
      fail "merged image of $1"
  fi
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

log "firmware for $CHIP${DEBUG_V:+ ($DEBUG_V)}: debug, release, broken"
rm -rf "$WORK" && mkdir -p "$WORK"
if [[ -n "${PREBUILT:-}" ]]; then
  SUFFIX=""
else
  SUFFIX="-e2e"
  if [[ -z "${SECURE_BOOT_KEY:-}" ]]; then
    export SECURE_BOOT_KEY="$WORK/secure_boot_test.pem"
    python3 -m espsecure generate_signing_key --version 2 --scheme rsa3072 "$SECURE_BOOT_KEY" >/dev/null
  fi
  if [[ "$SECURE" == 1 ]]; then
    NEXT_KEY="$WORK/secure_boot_next.pem"
    python3 -m espsecure generate_signing_key --version 2 --scheme rsa3072 "$NEXT_KEY" >/dev/null
    SV=1
  else
    SV=""
  fi
  build "build-$CHIP-e2e${DEBUG_V:+-$DEBUG_V}" e2e "$DEBUG_V"
  build "build-$CHIP-e2e-$RELEASE_V" e2e-release "$RELEASE_V" "$SV"
  [[ "$ROLLBACK" == 1 ]] && build "build-$CHIP-e2e-$BROKEN_V" e2e-broken "$BROKEN_V" "$SV"
fi
# The other security variant's release build (see the top)
VARIANT_DIR=""
if [[ "$SECURE" == 0 ]]; then
  [[ -n "${PREBUILT:-}" ]] || build "build-$CHIP-e2e-secure-release" e2e-release secure-release
  VARIANT_DIR="firmware/build-$CHIP$SUFFIX-secure-release"
elif [[ -z "${PREBUILT:-}" ]]; then
  build "build-$CHIP-e2e-release" e2e-release release
  VARIANT_DIR="firmware/build-$CHIP-e2e-release-signed"
  mkdir -p "$VARIANT_DIR"
  python3 -m espsecure sign_data --version 2 --keyfile "$SECURE_BOOT_KEY" \
    --output "$VARIANT_DIR/flightctl_mcu_fw.bin" "firmware/build-$CHIP-e2e-release/flightctl_mcu_fw.bin" >/dev/null
fi
DEBUG_DIR="firmware/build-$CHIP$SUFFIX${DEBUG_V:+-$DEBUG_V}"
RELEASE_DIR="firmware/build-$CHIP$SUFFIX-$RELEASE_V"
for d in "$DEBUG_DIR" "$RELEASE_DIR"; do
  [[ -f "$d/flightctl_mcu_fw.bin" ]] || fail "no $d"
done

log "credentials and signing key ($REGISTRY), provisioned into the flash image"
# One enrollment certificate for this run, as for a production batch
BATCH="e2e-$TAG" EXPIRATION=1d deploy/enrollment.sh "$WORK/secrets" >/dev/null
deploy/signing/keygen.sh "$WORK/secrets" >/dev/null
export SIGNING_KEY="$WORK/secrets/signing.key"
cp "$DEBUG_DIR/merged-binary.bin" "$WORK/flash.bin"
deploy/provision.py --image "$WORK/flash.bin" --secrets "$WORK/secrets" --server "agent-api.$IP.nip.io:8443" \
  --labels "app=$FLEET,site=emu" --wifi-ssid myssid --wifi-password mypassword
firmware/fw-push.py --variant "${SUFFIX#-}${SUFFIX:+-}$RELEASE_V" "$REGISTRY/flightctl-mcu/fw:release.$TAG" "$CHIP" >/dev/null
if [[ "$SECURE" == 1 ]]; then
  # The release build signed with a key the chip does not know, pushed and
  # cosign-signed like any other: only Secure Boot can stop it.
  OTHER_DIR="firmware/build-$CHIP$SUFFIX-$RELEASE_V-otherkey"
  mkdir -p "$OTHER_DIR"
  python3 -m espsecure generate_signing_key --version 2 --scheme rsa3072 "$WORK/other_key.pem" >/dev/null
  python3 -m espsecure sign_data --version 2 --keyfile "$WORK/other_key.pem" \
    --output "$OTHER_DIR/flightctl_mcu_fw.bin" "$RELEASE_DIR/flightctl_mcu_fw-unsigned.bin" >/dev/null
  firmware/fw-push.py --variant "${SUFFIX#-}${SUFFIX:+-}$RELEASE_V-otherkey" "$REGISTRY/flightctl-mcu/fw:otherkey.$TAG" "$CHIP" >/dev/null
  rm -rf "$OTHER_DIR"
fi
if [[ -n "$VARIANT_DIR" ]]; then
  [[ -f "$VARIANT_DIR/flightctl_mcu_fw.bin" ]] || fail "no $VARIANT_DIR"
  firmware/fw-push.py --variant "${VARIANT_DIR#firmware/build-$CHIP-}" "$REGISTRY/flightctl-mcu/fw:variant.$TAG" "$CHIP" >/dev/null
  [[ "$VARIANT_DIR" == *-signed ]] && rm -rf "$VARIANT_DIR"
fi
if [[ "$SECURE" == 1 ]]; then
  # Anti-rollback: the debug build (security version 0) after the release (1)
  firmware/fw-push.py --variant "${DEBUG_DIR#firmware/build-$CHIP-}" "$REGISTRY/flightctl-mcu/fw:old.$TAG" "$CHIP" >/dev/null
fi
if [[ -n "${NEXT_KEY:-}" ]]; then
  # Key rotation: the release build signed with the next key only
  NEXT_DIR="firmware/build-$CHIP-e2e-$RELEASE_V-nextkey"
  mkdir -p "$NEXT_DIR"
  python3 -m espsecure sign_data --version 2 --keyfile "$NEXT_KEY" \
    --output "$NEXT_DIR/flightctl_mcu_fw.bin" "$RELEASE_DIR/flightctl_mcu_fw-unsigned.bin" >/dev/null
  firmware/fw-push.py --variant "e2e-$RELEASE_V-nextkey" "$REGISTRY/flightctl-mcu/fw:nextkey.$TAG" "$CHIP" >/dev/null
  rm -rf "$NEXT_DIR"
fi
[[ -n "${PREBUILT:-}" ]] || NO_PUSH=1 wasm-apps/build.sh alarm v2 >/dev/null
wasm-apps/push.sh alarm v2 "v2.$TAG" "$REGISTRY" >/dev/null
NO_SIGN=1 ORAS_ARGS="--annotation io.flightctl-mcu.e2e=unsigned-$TAG" \
  wasm-apps/push.sh alarm v2 "v2.$TAG-unsigned" "$REGISTRY" >/dev/null

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
wait_for 120 "device Online (debug build)" "st.get('summary', {}).get('status') == 'Online' and not ci.get('firmwareVersion', '').endswith('-release')"
if [[ "$SECURE" == 1 ]]; then
  wait_for 30 "chip reports Secure Boot and flash encryption (release)" \
    "ci.get('secureBoot') == 'true' and ci.get('flashEncryption') == 'release'"
  wait_for 30 "device key in the DS peripheral" "ci.get('deviceKey') == 'ds'"
  grep -q 'provisioning moved to the encrypted store' "$WORK/emu.log" || fail "fc_creds not moved to the encrypted store"
  echo "ok: provisioning moved to the encrypted store, fc_creds erased"
fi

log "signed release build over the air, signed app"
apply_fleet "release.$TAG" "v2.$TAG"
wait_for 300 "release build confirmed, app running" \
  "ci.get('firmwareVersion', '').endswith('-release') and upd.get('status') == 'UpToDate' and apps and apps[0]['status'] == 'Running'"
# UpToDate is enough: the device reports the new version only once the new
# image is confirmed (and the release build does not log at INFO level).
echo "heap: free $(custom memFreeBytes), min $(custom memMinFreeBytes), largest internal block $(custom memInternalLargestBlock)"

log "unsigned app refused"
apply_fleet "release.$TAG" "v2.$TAG-unsigned"
wait_for 120 "unsigned app refused" "upd.get('status') == 'OutOfDate' and 'not signed' in upd.get('info', '')"
apply_fleet "release.$TAG" "v2.$TAG"
wait_for 120 "back to the signed app" "upd.get('status') == 'UpToDate'"

if [[ "$SECURE" == 1 ]]; then
  log "firmware signed with another Secure Boot key refused"
  apply_fleet "otherkey.$TAG" "v2.$TAG"
  wait_for 300 "firmware refused, still on the release build" \
    "upd.get('status') == 'OutOfDate' and 'rejected' in upd.get('info', '') and ci.get('firmwareVersion', '').endswith('-release')"
  apply_fleet "release.$TAG" "v2.$TAG"
  wait_for 120 "back to the release build" "upd.get('status') == 'UpToDate'"
fi

if [[ "$SECURE" == 1 ]]; then
  log "older firmware refused (anti-rollback)"
  wait_for 60 "release build's security version held by the chip" "ci.get('secureVersion') == '1/1'"
  apply_fleet "old.$TAG" "v2.$TAG"
  wait_for 300 "older firmware refused, still on the release build" \
    "upd.get('status') == 'OutOfDate' and 'older' in upd.get('info', '') and ci.get('firmwareVersion', '').endswith('-release')"
  apply_fleet "release.$TAG" "v2.$TAG"
  wait_for 120 "back to the release build" "upd.get('status') == 'UpToDate'"
fi

if [[ -n "$VARIANT_DIR" ]]; then
  log "firmware of the other security variant refused"
  apply_fleet "variant.$TAG" "v2.$TAG"
  wait_for 300 "firmware refused, still on the release build" \
    "upd.get('status') == 'OutOfDate' and 'security variant' in upd.get('info', '') and ci.get('firmwareVersion', '').endswith('-release')"
  apply_fleet "release.$TAG" "v2.$TAG"
  wait_for 120 "back to the release build" "upd.get('status') == 'UpToDate'"
fi

log "management certificate renewal"
first=$(custom mgmtCertNotAfter)
wait_for $CERT_TTL "certificate renewed" "ci.get('mgmtCertNotAfter', '$first') > '$first'"
expiry=$(python3 -c "import calendar,time; print(calendar.timegm(time.strptime('$first','%Y-%m-%dT%H:%M:%SZ')))")
wait_s=$(( expiry - $(date +%s) + 5 ))
(( wait_s > 0 )) && { echo "waiting ${wait_s}s for the first certificate to expire"; sleep "$wait_s"; }
tx=$(custom netTxBytes)
wait_for 120 "status accepted after the first certificate expired" "int(ci.get('netTxBytes', 0)) > $tx"

if [[ "$ROLLBACK" == 1 ]]; then
  # The broken build keeps the device away from the server for a minute or
  # more: a test certificate could expire meanwhile, and a device with an
  # expired certificate cannot get back in yet (see the roadmap). Back to the
  # server's normal lifetime, and wait for a renewal to bring one.
  log "back to long-lived certificates"
  kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS- >/dev/null
  kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null
  day=$(python3 -c "import time; print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 86400)))")
  wait_for 300 "certificate valid for more than a day" "ci.get('mgmtCertNotAfter', '') > '$day'"

  log "broken build is rolled back"
  # sdkconfig.broken: cannot reach Flight Control (wrong port), never confirmed
  [[ -f "firmware/build-$CHIP$SUFFIX-$BROKEN_V/flightctl_mcu_fw.bin" ]] || fail "no firmware/build-$CHIP$SUFFIX-$BROKEN_V"
  firmware/fw-push.py --variant "${SUFFIX#-}${SUFFIX:+-}$BROKEN_V" "$REGISTRY/flightctl-mcu/fw:broken.$TAG" "$CHIP" >/dev/null
  apply_fleet "broken.$TAG" "v2.$TAG"
  wait_for 480 "back on the release build, update reported as failed" \
    "ci.get('firmwareVersion', '').endswith('-release') and 'rolled back' in upd.get('info', '')"
fi

if [[ -n "${NEXT_KEY:-}" ]]; then
  # Last: afterwards the device takes only images signed with the next key.
  log "Secure Boot key rotation: a release signed with the next key retires the first"
  wait_for 60 "both keys usable, the first signs the running image" "ci.get('secureBootKeys') == '0*,1'"
  apply_fleet "nextkey.$TAG" "v2.$TAG"
  wait_for 300 "release signed with the next key confirmed, the first key revoked" \
    "upd.get('status') == 'UpToDate' and ci.get('secureBootKeys') == '1*'"
fi

log "PASS"
