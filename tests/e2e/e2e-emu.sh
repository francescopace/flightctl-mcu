#!/usr/bin/env bash
# End-to-end test of the real firmware, run by Espressif's emulator (esp-emu)
# against Flight Control: the ESP-IDF port, NVS, mbedTLS and WAMR on the chip,
# A/B OTA with reboot, signature checks, certificate renewal and rollback.
# The emulator's WiFi soft AP NATs to this machine, so the firmware reaches
# Flight Control and the registry as a board on the LAN would.
#
#   tests/e2e/e2e-emu.sh                  deploy Flight Control on kind + registry, then test
#   SKIP_DEPLOY=1 tests/e2e/e2e-emu.sh    against the cluster and registry already running
#   CHIP=esp32c5 tests/e2e/e2e-emu.sh     another emulated chip (esp32c3, esp32c5, esp32s3)
#   ROLLBACK=0 tests/e2e/e2e-emu.sh       skip the rollback step (about 4 minutes)
#   CERT=0 tests/e2e/e2e-emu.sh           skip the certificate renewal (about 3 minutes): the
#                                  server keeps its normal certificate lifetime, so a
#                                  slow step cannot outlast a 2-minute certificate
#   PREBUILT=1 tests/e2e/e2e-emu.sh       use port/espidf/build-<chip>{,-release,-broken} and
#                                  wasm-apps/build/alarm-v2 as they are (CI: the firmware
#                                  and WASM jobs' builds), instead of building. Secure
#                                  builds come unsigned (port/espidf/build-ci.sh): the test
#                                  signs them with throwaway keys, into build-<chip>-signed-*
#   SECURE=1 tests/e2e/e2e-emu.sh         the secure variant (port/espidf/sdkconfig.secure): Secure
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
# release signed with that key alone retires the first one; before that, a
# third, spare key is revoked through the fleet's config.
#
# The device pulls through the registry's door with TLS and a password
# (docs/REGISTRY.md): it speaks nothing else. Last, the device is decommissioned: it wipes
# its key and certificate and does not enroll again.
#
# A valid image of the other security variant is refused too: a plain device
# gets the secure release build (PREBUILT: port/espidf/build-<chip>-secure-release
# must be there), a secure device the plain release build signed with its own
# key.
#
# The plain run gets its WiFi network in the field: provisioned with a proof
# of possession and no network, the device opens an access point, which the
# emulator joins; tests/e2e/esp-prov.sh then plays the installer (a wrong proof is
# refused first). The secure run is given its network at the factory.
#
# The firmware has no credentials in it: the test provisions the emulated
# device's fc_creds partition (tools/provision.py) for this cluster, so the
# same images as on real boards are tested. It boots the debug build, updates
# it to the release build over the air, then tries the broken build.
#
# Needs: esp-emu, docker (ESP-IDF image), flightctl (logged in), kubectl, oras,
# clang + wasm-ld, python3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
source tools/local-ip.sh
IP="${HOST_IP:-$(local_ip)}"
REGISTRY="${REGISTRY:-$IP:5001}"            # where the test pushes: the plain door
TLS_REGISTRY="${TLS_REGISTRY:-registry.$IP.nip.io:5443}"  # the same images, TLS and a password
FLEET_REGISTRY="$TLS_REGISTRY"              # what the fleet names: devices pull over TLS only
CHIP="${CHIP:-esp32c3}"
ROLLBACK="${ROLLBACK:-1}"
SECURE="${SECURE:-0}"
if [[ "$SECURE" == 1 ]]; then
  DEBUG_V=secure; RELEASE_V=secure-release; BROKEN_V=secure-broken
else
  DEBUG_V=""; RELEASE_V=release; BROKEN_V=broken
fi
CERT="${CERT:-1}"
CERT_TTL=120
TAG="emu$(date +%s)"
FLEET="emu-e2e"
WORK="$ROOT/build/e2e-emu"   # inside the repo: the ESP-IDF container mounts it
EMU_PID=""
DEVICE=""

log() { printf '\n== %s\n' "$*"; }
fail() {
  echo "FAIL: $*" >&2
  [[ -n "$DEVICE" ]] && device_state >&2
  echo "--- emulator log (last 60 lines; all of it in $WORK/emu.log)" >&2
  grep -v '^\s*$' "$WORK/emu.log" 2>/dev/null | tail -60 >&2 || true
  exit 1
}

# What the device last reported: its update, its apps and its heap. The heap
# is as the agent last read it (at the first status, after 5 minutes, then
# hourly), not as it is now; restarts are current.
device_state() {
  flightctl get "device/$DEVICE" -o json 2>/dev/null | python3 -c "
import json, sys
st = json.load(sys.stdin).get('status') or {}
ci = (st.get('systemInfo') or {}).get('customInfo') or {}
upd = st.get('updated') or {}
apps = ', '.join(f\"{a['name']} {a['status']} ({a.get('restarts', 0)} restarts)\" for a in st.get('applications') or [])
print(f\"   heap at the agent's last read: free {ci.get('memFreeBytes')}, min {ci.get('memMinFreeBytes')}, largest block {ci.get('memInternalLargestBlock', '-')}\")
print(f\"   update: {upd.get('status')} {upd.get('info', '')}\")
print(f\"   apps: {apps or 'none'}\")" 2>/dev/null || true
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
      device_state
      return 0
    fi
    (( $(date +%s) - start > timeout )) && fail "timed out after ${timeout}s waiting for: $what"
    sleep 3
  done
}

custom() { flightctl get "device/$DEVICE" -o json | python3 -c "import json,sys; print(json.load(sys.stdin)['status']['systemInfo']['customInfo'].get('$1',''))"; }

# fleet <firmware tag> <app tag> [content of /etc/flightctl-mcu/secure-boot.json]
apply_fleet() {
  local security=""
  [[ -n "${3:-}" ]] && security="
        - name: security
          inline:
            - path: /etc/flightctl-mcu/secure-boot.json
              content: '$3'"
  tools/fleet/registry-config.py "$FLEET_REGISTRY" <<EOF2 | flightctl apply -f - >/dev/null
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
        image: $FLEET_REGISTRY/flightctl-mcu/fw:$1
      applications:
        - name: alarm
          appType: container
          image: $FLEET_REGISTRY/flightctl-mcu/alarm:$2
          envVars:
            FC_ALLOW: "led0"
      config:
        - name: alarm
          inline:
            - path: /etc/fc-app/alarm.json
              content: '{"threshold_c": 40}'$security
EOF2
}

build() { # build <build dir> <version> [variant] [security version]
  # LOCAL=0: not your sdkconfig.wifi/.local, the test provisions its own.
  LOCAL=0 BUILD_DIR="$1" TARGET="$CHIP" VARIANT="${3:-}" FW_VERSION="$2" SECURE_BOOT_KEY="${SECURE_BOOT_KEY:-}" \
    SECURE_VERSION="${4:-}" port/espidf/build.sh build merge-bin >"$WORK/$1.log" 2>&1 ||
    { tail -30 "$WORK/$1.log"; fail "firmware build $1"; }
  if [[ "${3:-}" == secure* ]]; then
    # The bootloader also trusts a second key, for the key rotation step.
    python3 port/espidf/merge-secure.py ${NEXT_KEY:+--extra-key "$NEXT_KEY"} ${SPARE_KEY:+--extra-key "$SPARE_KEY"} \
      "port/espidf/$1" >>"$WORK/$1.log" 2>&1 ||
      fail "merged image of $1"
  fi
}

if [[ -z "${SKIP_DEPLOY:-}" ]]; then
  log "deploy Flight Control on kind ($IP) and the registry"
  tools/kind/up.sh "$IP"
  tools/registry/up.sh
  kubectl wait --timeout=15m -n flightctl --for=condition=Available deployment --all
  for i in $(seq 1 60); do
    curl -sk -o /dev/null "https://api.$IP.nip.io:8443/api/v1/auth/config" && break
    (( i == 60 )) && fail "API not reachable"
    sleep 5
  done
  flightctl login "https://api.$IP.nip.io:8443" --insecure-skip-tls-verify \
    -t "$(kubectl create token flightctl-admin -n flightctl --duration=2h)" >/dev/null
fi

if [[ "$CERT" == 1 ]]; then
  log "server issues ${CERT_TTL}s management certificates"
  kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS=$CERT_TTL >/dev/null
else
  # A cluster left by an interrupted run may still issue short ones.
  kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS- >/dev/null
fi
kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null

new_key() { python3 -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$1" >/dev/null; }

# sign_prebuilt <variant> [merge]: a secure build as CI makes it, unsigned
# (port/espidf/build-<chip>-<variant>), signed with SECURE_BOOT_KEY into
# port/espidf/build-<chip>-signed-<variant>; with "merge", its bootloader too,
# which also trusts the next and spare keys, and the merged flash image.
sign_prebuilt() {
  local src="port/espidf/build-$CHIP-$1" dst="port/espidf/build-$CHIP-signed-$1"
  [[ -f "$src/flightctl_mcu_fw.bin" ]] || fail "no $src"
  python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(d[-0x1000:-0x1000+1] == b'\xe7')" \
    "$src/flightctl_mcu_fw.bin" || fail "$src is signed already: not a CI build"
  rm -rf "$dst" && mkdir -p "$dst" && cp -R "$src/." "$dst/"
  cp "$src/flightctl_mcu_fw.bin" "$dst/flightctl_mcu_fw-unsigned.bin"
  python3 -m espsecure sign-data --version 2 --keyfile "$SECURE_BOOT_KEY" \
    --output "$dst/flightctl_mcu_fw.bin" "$src/flightctl_mcu_fw.bin" >/dev/null || fail "signing $src"
  if [[ "${2:-}" == merge ]]; then
    python3 -m espsecure sign-data --version 2 --keyfile "$SECURE_BOOT_KEY" \
      --output "$dst/bootloader/bootloader.bin" "$src/bootloader/bootloader.bin" >/dev/null ||
      fail "signing $src's bootloader"
    python3 port/espidf/merge-secure.py --extra-key "$NEXT_KEY" --extra-key "$SPARE_KEY" "$dst" \
      >"$WORK/merge-$1.log" 2>&1 || { cat "$WORK/merge-$1.log"; fail "merged image of $dst"; }
  fi
}

log "firmware for $CHIP${DEBUG_V:+ ($DEBUG_V)}: debug, release, broken"
rm -rf "$WORK" && mkdir -p "$WORK"
if [[ -z "${SECURE_BOOT_KEY:-}" ]]; then
  export SECURE_BOOT_KEY="$WORK/secure_boot_test.pem"
  new_key "$SECURE_BOOT_KEY"
fi
if [[ "$SECURE" == 1 ]]; then
  NEXT_KEY="$WORK/secure_boot_next.pem"
  SPARE_KEY="$WORK/secure_boot_spare.pem"
  new_key "$NEXT_KEY" && new_key "$SPARE_KEY"
fi
if [[ -n "${PREBUILT:-}" ]]; then
  SUFFIX=""
  if [[ "$SECURE" == 1 ]]; then
    SUFFIX="-signed"
    sign_prebuilt "$DEBUG_V" merge
    sign_prebuilt "$RELEASE_V"
    [[ "$ROLLBACK" == 1 ]] && sign_prebuilt "$BROKEN_V"
  fi
else
  SUFFIX="-e2e"
  SV=""
  [[ "$SECURE" == 1 ]] && SV=1
  build "build-$CHIP-e2e${DEBUG_V:+-$DEBUG_V}" e2e "$DEBUG_V"
  build "build-$CHIP-e2e-$RELEASE_V" e2e-release "$RELEASE_V" "$SV"
  [[ "$ROLLBACK" == 1 ]] && build "build-$CHIP-e2e-$BROKEN_V" e2e-broken "$BROKEN_V" "$SV"
fi
# The other security variant's release build (see the top)
VARIANT_DIR=""
if [[ "$SECURE" == 0 ]]; then
  [[ -n "${PREBUILT:-}" ]] || build "build-$CHIP-e2e-secure-release" e2e-release secure-release
  VARIANT_DIR="port/espidf/build-$CHIP$SUFFIX-secure-release"
else
  # The plain release signed with this device's own key: refused all the same.
  PLAIN_RELEASE="port/espidf/build-$CHIP-release"
  if [[ -z "${PREBUILT:-}" ]]; then
    build "build-$CHIP-e2e-release" e2e-release release
    PLAIN_RELEASE="port/espidf/build-$CHIP-e2e-release"
  fi
  [[ -f "$PLAIN_RELEASE/flightctl_mcu_fw.bin" ]] || fail "no $PLAIN_RELEASE"
  VARIANT_DIR="port/espidf/build-$CHIP-variant-signed"
  mkdir -p "$VARIANT_DIR"
  python3 -m espsecure sign-data --version 2 --keyfile "$SECURE_BOOT_KEY" \
    --output "$VARIANT_DIR/flightctl_mcu_fw.bin" "$PLAIN_RELEASE/flightctl_mcu_fw.bin" >/dev/null
fi
DEBUG_DIR="port/espidf/build-$CHIP$SUFFIX${DEBUG_V:+-$DEBUG_V}"
RELEASE_DIR="port/espidf/build-$CHIP$SUFFIX-$RELEASE_V"
for d in "$DEBUG_DIR" "$RELEASE_DIR"; do
  [[ -f "$d/flightctl_mcu_fw.bin" ]] || fail "no $d"
done

log "credentials and signing key ($REGISTRY), provisioned into the flash image"
# One enrollment certificate for this run, as for a production batch
BATCH="e2e-$TAG" EXPIRATION=1d tools/enrollment.sh "$WORK/secrets" >/dev/null
tools/signing/keygen.sh "$WORK/secrets" >/dev/null
export SIGNING_KEY="$WORK/secrets/signing.key"
cp "$DEBUG_DIR/merged-binary.bin" "$WORK/flash.bin"
POP="fieldprov-$TAG"
PROV_PORT="${PROV_PORT:-18080}"
if [[ "$SECURE" == 1 ]]; then
  NET_ARGS=(--wifi-ssid myssid --wifi-password mypassword)
else
  NET_ARGS=(--pop "$POP")
fi
tools/provision.py --image "$WORK/flash.bin" --secrets "$WORK/secrets" --server "agent-api.$IP.nip.io:8443" \
  --labels "app=$FLEET,site=emu" "${NET_ARGS[@]}"
# The debug build is pushed too: the device starts on it, and the release
# comes with a delta from it, which the device must use.
DEBUG_VARIANT="${DEBUG_DIR#port/espidf/build-$CHIP}"
DEBUG_ARGS=()
[[ -n "$DEBUG_VARIANT" ]] && DEBUG_ARGS=(--variant "${DEBUG_VARIANT#-}")
tools/fw-push.py ${DEBUG_ARGS[@]+"${DEBUG_ARGS[@]}"} "$REGISTRY/flightctl-mcu/fw:debug.$TAG" "$CHIP" >/dev/null
tools/fw-push.py --variant "${SUFFIX#-}${SUFFIX:+-}$RELEASE_V" --delta-from "debug.$TAG" \
  "$REGISTRY/flightctl-mcu/fw:release.$TAG" "$CHIP" >"$WORK/fw-push.log" 2>&1 || { cat "$WORK/fw-push.log"; fail "push of the release"; }
grep 'delta from' "$WORK/fw-push.log" || fail "no delta from the debug build in the release"
if [[ "$SECURE" == 1 ]]; then
  # The release build signed with a key the chip does not know, pushed and
  # cosign-signed like any other: only Secure Boot can stop it.
  OTHER_DIR="port/espidf/build-$CHIP$SUFFIX-$RELEASE_V-otherkey"
  mkdir -p "$OTHER_DIR"
  python3 -m espsecure generate-signing-key --version 2 --scheme rsa3072 "$WORK/other_key.pem" >/dev/null
  python3 -m espsecure sign-data --version 2 --keyfile "$WORK/other_key.pem" \
    --output "$OTHER_DIR/flightctl_mcu_fw.bin" "$RELEASE_DIR/flightctl_mcu_fw-unsigned.bin" >/dev/null
  tools/fw-push.py --variant "${SUFFIX#-}${SUFFIX:+-}$RELEASE_V-otherkey" "$REGISTRY/flightctl-mcu/fw:otherkey.$TAG" "$CHIP" >/dev/null
  rm -rf "$OTHER_DIR"
fi
if [[ -n "$VARIANT_DIR" ]]; then
  [[ -f "$VARIANT_DIR/flightctl_mcu_fw.bin" ]] || fail "no $VARIANT_DIR"
  tools/fw-push.py --variant "${VARIANT_DIR#port/espidf/build-$CHIP-}" "$REGISTRY/flightctl-mcu/fw:variant.$TAG" "$CHIP" >/dev/null
  [[ "$VARIANT_DIR" == *-signed ]] && rm -rf "$VARIANT_DIR"
fi
if [[ "$SECURE" == 1 ]]; then
  # Anti-rollback: the debug build (security version 0) after the release (1)
  tools/fw-push.py --variant "${DEBUG_DIR#port/espidf/build-$CHIP-}" "$REGISTRY/flightctl-mcu/fw:old.$TAG" "$CHIP" >/dev/null
fi
if [[ -n "${NEXT_KEY:-}" ]]; then
  # Key rotation: the release build signed with the next key only
  NEXT_DIR="port/espidf/build-$CHIP$SUFFIX-$RELEASE_V-nextkey"
  mkdir -p "$NEXT_DIR"
  python3 -m espsecure sign-data --version 2 --keyfile "$NEXT_KEY" \
    --output "$NEXT_DIR/flightctl_mcu_fw.bin" "$RELEASE_DIR/flightctl_mcu_fw-unsigned.bin" >/dev/null
  tools/fw-push.py --variant "${SUFFIX#-}-$RELEASE_V-nextkey" "$REGISTRY/flightctl-mcu/fw:nextkey.$TAG" "$CHIP" >/dev/null
  rm -rf "$NEXT_DIR"
fi
[[ -n "${PREBUILT:-}" ]] || NO_PUSH=1 wasm-apps/build.sh alarm v2 >/dev/null
wasm-apps/push.sh alarm v2 "v2.$TAG" "$REGISTRY" >/dev/null
NO_SIGN=1 ORAS_ARGS="--annotation io.flightctl-mcu.e2e=unsigned-$TAG" \
  wasm-apps/push.sh alarm v2 "v2.$TAG-unsigned" "$REGISTRY" >/dev/null

log "boot v1 in the emulator"
ESP_EMU="$(command -v esp-emu || echo "$HOME/.local/bin/esp-emu")"
EMU_NET=(--net user,net=10.0.2.0/24)
if [[ "$SECURE" == 0 ]]; then
  EMU_NET=(--net "user,net=10.0.2.0/24,hostfwd=tcp::$PROV_PORT-:80" --wifi-fw-ap-password "$POP")
fi
"$ESP_EMU" --chip "$CHIP" --firmware "$WORK/flash.bin" "${EMU_NET[@]}" --timeout 2400s >"$WORK/emu.log" 2>&1 &
EMU_PID=$!
if [[ "$SECURE" == 0 ]]; then
  log "WiFi provisioning in the field"
  for i in $(seq 1 60); do
    grep -q 'join, AID' "$WORK/emu.log" && break
    (( i == 60 )) && fail "no provisioning access point"
    sleep 2
  done
  grep -q 'field provisioning on access point' "$WORK/emu.log" || fail "no field provisioning"
  if [[ "${ESP_PROV_LOCAL:-0}" == 1 || "$(uname)" == Linux ]]; then PROV_HOST=127.0.0.1; else PROV_HOST=host.docker.internal; fi
  if tests/e2e/esp-prov.sh "$PROV_HOST:$PROV_PORT" --pop "wrong-$POP" --ssid myssid --passphrase mypassword >"$WORK/prov-wrong.log" 2>&1; then
    fail "provisioning with a wrong proof of possession accepted"
  fi
  grep -q 'Error in establishing session' "$WORK/prov-wrong.log" || { cat "$WORK/prov-wrong.log"; fail "wrong proof: unexpected error"; }
  echo "ok: wrong proof of possession refused"
  tests/e2e/esp-prov.sh "$PROV_HOST:$PROV_PORT" --pop "$POP" --ssid myssid --passphrase mypassword >"$WORK/prov.log" 2>&1 ||
    { tail -20 "$WORK/prov.log"; fail "field provisioning"; }
  grep -q 'Provisioning was successful' "$WORK/prov.log" || fail "field provisioning not confirmed"
  echo "ok: network given in the field, device connected"
fi
for i in $(seq 1 150); do
  DEVICE=$(sed -n 's/.*device name: \([a-z0-9]*\).*/\1/p' "$WORK/emu.log" | head -1)
  [[ -n "$DEVICE" ]] && flightctl get "enrollmentrequest/$DEVICE" >/dev/null 2>&1 && break
  (( i == 150 )) && fail "no enrollment request"
  sleep 2
done
echo "device: $DEVICE"
printf 'devices:\n  - name: %s\n    labels:\n      stage: emu\n' "$DEVICE" > "$WORK/allowlist.yaml"
tools/auto-approve/auto-approve.py "$WORK/allowlist.yaml"
wait_for 120 "debug build Online in the first OTA slot" \
  "st.get('summary', {}).get('status') == 'Online' and ci.get('bootSlot') == 'ota_0' and not ci.get('firmwareVersion', '').endswith('-release')"
if [[ "$SECURE" == 1 ]]; then
  wait_for 30 "chip reports Secure Boot and flash encryption (release)" \
    "ci.get('secureBoot') == 'true' and ci.get('flashEncryption') == 'release'"
  if [[ "$CHIP" == esp32c5 ]]; then
    # The C5 has an ECDSA peripheral: the key is in an eFuse block only it
    # reads, and the certificate was issued for a request it signed.
    wait_for 30 "device key in the ECDSA peripheral" "ci.get('deviceKey') == 'ecdsa'"
    grep -q 'ECDSA: signature made by the peripheral verified' "$WORK/emu.log" || fail "device key not sealed"
    grep -q 'eFuse read protection closed for the other key blocks: ESP_OK' "$WORK/emu.log" ||
      fail "eFuse read protection left open"
    echo "ok: key burned into eFuse and read-protected, read protection then closed"
  else
    wait_for 30 "device key in the encrypted store" "ci.get('deviceKey') == 'storage'"
  fi
  grep -q 'provisioning moved to the encrypted store' "$WORK/emu.log" || fail "fc_creds not moved to the encrypted store"
  echo "ok: provisioning moved to the encrypted store, fc_creds erased"
fi

log "signed release build over the air, signed app"
apply_fleet "release.$TAG" "v2.$TAG"
wait_for 300 "release build confirmed in the second OTA slot, app running" \
  "ci.get('firmwareVersion', '').endswith('-release') and ci.get('bootSlot') == 'ota_1' and upd.get('status') == 'UpToDate' and apps and apps[0]['status'] == 'Running'"
# UpToDate is enough: the device reports the new version only once the new
# image is confirmed (and the release build does not log at INFO level).
# The firmware and the app came in one spec: the firmware first, the app
# after the reboot, checked by the firmware that runs it. The debug build,
# which logs at INFO, installed no app before rebooting.
grep -q 'installed, rebooting' "$WORK/emu.log" || fail "no reboot into the release build in the log"
sed -n '1,/installed, rebooting/p' "$WORK/emu.log" | grep -q 'installing app alarm' &&
  fail "the debug build installed the app before its new firmware"
echo "ok: firmware first, the app after its reboot"
echo "heap: free $(custom memFreeBytes), min $(custom memMinFreeBytes), largest internal block $(custom memInternalLargestBlock)"
wait_for 30 "the release came as a delta from the debug build" \
  "(st.get('os') or {}).get('lastDelta', {}).get('outcome') == 'Applied'"

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

if [[ "$CERT" == 1 ]]; then
  log "management certificate renewal"
  first=$(custom mgmtCertNotAfter)
  wait_for $CERT_TTL "certificate renewed" "ci.get('mgmtCertNotAfter', '$first') > '$first'"
  expiry=$(python3 -c "import calendar,time; print(calendar.timegm(time.strptime('$first','%Y-%m-%dT%H:%M:%SZ')))")
  wait_s=$(( expiry - $(date +%s) + 5 ))
  (( wait_s > 0 )) && { echo "waiting ${wait_s}s for the first certificate to expire"; sleep "$wait_s"; }
  # The counters in customInfo change once an hour: a status accepted since is
  # shown by the next renewal, every quarter of CERT_TTL.
  last=$(custom mgmtCertNotAfter)
  wait_for 120 "status accepted after the first certificate expired" "ci.get('mgmtCertNotAfter', '') > '$last'"
fi

if [[ "$ROLLBACK" == 1 ]]; then
  if [[ "$CERT" == 1 ]]; then
    # The broken build keeps the device away from the server for a minute or
    # more: a test certificate could expire meanwhile, and a device with an
    # expired certificate cannot get back in yet (see the roadmap). Back to the
    # server's normal lifetime, and wait for a renewal to bring one.
    log "back to long-lived certificates"
    kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS- >/dev/null
    kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null
    day=$(python3 -c "import time; print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 86400)))")
    wait_for 300 "certificate valid for more than a day" "ci.get('mgmtCertNotAfter', '') > '$day'"
  fi

  log "broken build is rolled back"
  # sdkconfig.broken: cannot reach Flight Control (wrong port), never confirmed
  [[ -f "port/espidf/build-$CHIP$SUFFIX-$BROKEN_V/flightctl_mcu_fw.bin" ]] || fail "no port/espidf/build-$CHIP$SUFFIX-$BROKEN_V"
  tools/fw-push.py --variant "${SUFFIX#-}${SUFFIX:+-}$BROKEN_V" "$REGISTRY/flightctl-mcu/fw:broken.$TAG" "$CHIP" >/dev/null
  apply_fleet "broken.$TAG" "v2.$TAG"
  wait_for 480 "back in the confirmed release slot, update reported as failed" \
    "ci.get('firmwareVersion', '').endswith('-release') and ci.get('bootSlot') == 'ota_1' and 'rolled back' in upd.get('info', '')"
  # The broken build has no delta: it came whole.
  wait_for 30 "an image without a delta came whole" \
    "(st.get('os') or {}).get('lastDelta', {}).get('outcome') == 'NotUsed'"
fi

if [[ -n "${SPARE_KEY:-}" ]]; then
  log "a spare Secure Boot key revoked through the fleet's config"
  wait_for 60 "three keys usable, the first signs the running image" "ci.get('secureBootKeys') == '0*,1,2'"
  apply_fleet "release.$TAG" "v2.$TAG" '{"revokeKeys": [2]}'
  wait_for 180 "key 2 revoked" "upd.get('status') == 'UpToDate' and ci.get('secureBootKeys') == '0*,1'"
  apply_fleet "release.$TAG" "v2.$TAG" '{"revokeKeys": [0]}'
  wait_for 180 "revoking the key that signs the running firmware refused" \
    "upd.get('status') == 'OutOfDate' and ci.get('secureBootKeys') == '0*,1'"
  apply_fleet "release.$TAG" "v2.$TAG"
  wait_for 120 "back to the plain config" "upd.get('status') == 'UpToDate'"
fi

if [[ -n "${NEXT_KEY:-}" ]]; then
  # Last: afterwards the device takes only images signed with the next key.
  log "Secure Boot key rotation: a release signed with the next key retires the first"
  wait_for 60 "both keys usable, the first signs the running image" "ci.get('secureBootKeys') == '0*,1'"
  apply_fleet "nextkey.$TAG" "v2.$TAG"
  wait_for 300 "release signed with the next key confirmed, the first key revoked" \
    "upd.get('status') == 'UpToDate' and ci.get('secureBootKeys') == '1*'"
fi

log "decommission"
# Last: the device wipes its key and certificate, restarts, and stays out.
requests() { flightctl get enrollmentrequests -o name 2>/dev/null | wc -l; }
before=$(requests)
flightctl decommission "device/$DEVICE" >/dev/null
wait_for 180 "device Decommissioned" "(st.get('lifecycle') or {}).get('status') == 'Decommissioned'"
for i in $(seq 1 60); do
  grep -q 'not enrolling' "$WORK/emu.log" && break
  (( i == 60 )) && fail "the device did not come back decommissioned"
  sleep 3
done
sleep 20
(( $(requests) == before )) || fail "the decommissioned device asked to enroll"
echo "ok: decommissioned, and not enrolling again"

log "PASS"
