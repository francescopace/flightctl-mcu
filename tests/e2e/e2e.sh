#!/usr/bin/env bash
# End-to-end test: the simulated device (build/host/fc-host-agent) against a
# real Flight Control. Checks enrollment, approval, status, inline config,
# a WASM app install, an update v1 -> v2 without restart, a config change,
# the rejection of an unsigned app, an update while the registry is down,
# the app removal, a resource alert from the fleet's spec.resources, the
# renewal of the management certificate (the server is told to issue
# 2-minute certificates for the test), a server restart, a new enrollment
# after the certificate expired, the fleet kept in Git (a ResourceSync) and,
# last, the device's decommissioning. Apps are signed with a throwaway key;
# when cosign is installed, it checks that signature too. The device pulls
# from the registry's door with TLS and a password (tools/registry/up.sh),
# first without the password, which it must report.
#
#   tests/e2e/e2e.sh                 deploy Flight Control on kind + registry, then test
#   SKIP_DEPLOY=1 tests/e2e/e2e.sh   test against the cluster and registry already running
#   DEVICE_KIND=zephyr tests/e2e/e2e.sh   the same test with the Zephyr build on native_sim
#                             (port/zephyr/build.sh) as the device, instead of the host agent
#
# Needs: kind, helm, kubectl, flightctl, oras, clang + wasm-ld, python3 (PyYAML),
# and the host agent built in build/host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
source tools/local-ip.sh
IP="${HOST_IP:-$(local_ip)}"
API="https://api.$IP.nip.io:8443"
AGENT="agent-api.$IP.nip.io:8443"
# The door with TLS and a password; the plain one serves the same images.
REGISTRY="${REGISTRY:-registry.$IP.nip.io:5443}"
PLAIN_REGISTRY="${PLAIN_REGISTRY:-$IP:5001}"
TAG="e2e$(date +%s)"
WORK="$(mktemp -d)"
echo "work dir: $WORK"
FLEET=e2e
DEVICE_KIND="${DEVICE_KIND:-host}"
# The Zephyr device's flash: inside the repository, which is what the build
# image sees when the device runs in it (port/zephyr/run.sh).
ZFLASH="$ROOT/build/e2e-zephyr-$TAG.bin"
AGENT_PID=""
GIT_SERVER=""
DEVICE=""
OLD_DEVICE=""
CERT_TTL=120

log() { printf '\n== %s\n' "$*"; }
fail() {
  echo "FAIL: $*" >&2
  echo "--- agent log (last 60 lines, without LED changes)" >&2
  grep -v '^LED' "$WORK/agent.log" | tail -60 >&2 || true
  exit 1
}

cleanup() {
  if [[ -n "$AGENT_PID" ]]; then
    kill "$AGENT_PID" 2>/dev/null || true
    wait "$AGENT_PID" 2>/dev/null || true
  fi
  [[ -n "$GIT_SERVER" ]] && docker rm -f "$GIT_SERVER" >/dev/null 2>&1 || true
  if [[ -z "${KEEP:-}" ]]; then
    flightctl delete "resourcesync/$FLEET" >/dev/null 2>&1 || true
    flightctl delete "repository/$FLEET-fleets" >/dev/null 2>&1 || true
    [[ -n "$GIT_SERVER" ]] && kubectl -n flightctl delete secret/fc-registry-ca secret/fc-registry-auth \
      role/flightctl-worker-registry-secrets rolebinding/flightctl-worker-registry-secrets >/dev/null 2>&1 || true
    flightctl delete "fleet/$FLEET" >/dev/null 2>&1 || true
    [[ -n "$DEVICE" ]] && flightctl delete "device/$DEVICE" >/dev/null 2>&1 || true
    [[ -n "$DEVICE" ]] && flightctl delete "enrollmentrequest/$DEVICE" >/dev/null 2>&1 || true
    [[ -n "$OLD_DEVICE" ]] && flightctl delete "device/$OLD_DEVICE" >/dev/null 2>&1 || true
    [[ -n "$OLD_DEVICE" ]] && flightctl delete "enrollmentrequest/$OLD_DEVICE" >/dev/null 2>&1 || true
    rm -rf "$WORK" "$ZFLASH"
  fi
  # Back to the default certificate lifetime on a cluster that stays up.
  [[ -n "${SKIP_DEPLOY:-}" ]] && kubectl -n flightctl set env deployment/flightctl-api \
    FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS- >/dev/null 2>&1 || true
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
sys.exit(0 if ($expr) else 1)" 2>/dev/null; then
      echo "ok: $what ($(( $(date +%s) - start )) s)"
      return 0
    fi
    (( $(date +%s) - start > timeout )) && fail "timed out after ${timeout}s waiting for: $what"
    sleep 2
  done
}

# fleet <threshold_c> <app tag, or empty for no app> [app image name, default alarm]
# RESOURCES: spec.resources to add (YAML, indented for the template's spec).
RESOURCES=""
fleet_yaml() {
  local threshold="$1" app="$2" image="${3:-alarm}"
  {
    cat <<EOF
apiVersion: flightctl.io/v1beta1
kind: Fleet
metadata:
  name: $FLEET
spec:
  selector:
    matchLabels:
      app: e2e
  template:
    metadata:
      labels:
        fleet: $FLEET
    spec:
      config:
        - name: alarm
          inline:
            - path: /etc/fc-app/alarm.json
              content: '{"threshold_c": $threshold}'
EOF
    [[ -n "$RESOURCES" ]] && printf '%s\n' "$RESOURCES"
    if [[ -n "$app" ]]; then
      cat <<EOF
      applications:
        - name: alarm
          appType: container
          image: $REGISTRY/flightctl-mcu/$image:$app
          envVars:
            FC_ALLOW: "led0"
EOF
    fi
  }
}
apply_fleet() {
  fleet_yaml "$@" | tools/fleet/registry-config.py "$REGISTRY" | flightctl apply -f - >/dev/null
}

rendered() {
  flightctl get "device/$DEVICE" -o json | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["metadata"]["annotations"]["device-controller/renderedVersion"])'
}

if [[ -z "${SKIP_DEPLOY:-}" ]]; then
  log "deploy Flight Control on kind ($IP) and the registry"
  tools/kind/up.sh "$IP"
  tools/registry/up.sh
fi

log "wait for Flight Control (management certificates valid ${CERT_TTL}s)"
kubectl -n flightctl set env deployment/flightctl-api FLIGHTCTL_TEST_MGMT_CERT_EXPIRY_SECONDS=$CERT_TTL >/dev/null
kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m
kubectl wait --timeout=15m -n flightctl --for=condition=Available deployment --all
for i in $(seq 1 60); do
  curl -sk -o /dev/null "$API/api/v1/auth/config" && break
  (( i == 60 )) && fail "API $API not reachable"
  sleep 5
done
# Keep a working login (a cluster you already use); else log in for the test.
flightctl get devices >/dev/null 2>&1 || flightctl login "$API" --insecure-skip-tls-verify \
  -t "$(kubectl create token flightctl-admin -n flightctl --duration=2h)" >/dev/null

log "enrollment credentials, signing key and WASM apps ($REGISTRY)"
tools/enrollment.sh "$WORK/secrets" >/dev/null
tools/signing/keygen.sh "$WORK/signing" >/dev/null
export SIGNING_KEY="$WORK/signing/signing.key"
# WASM apps: built here, or taken as they are from wasm-apps/build
# (PREBUILT_APPS=1: CI's WASM job), then pushed and signed under test tags.
for a in alarm-v1 alarm-v2 trend-v1; do
  [[ -n "${PREBUILT_APPS:-}" ]] || NO_PUSH=1 wasm-apps/build.sh "${a%-*}" "${a##*-}" >/dev/null
done
wasm-apps/push.sh alarm v1 "v1.$TAG" "$REGISTRY" >/dev/null
wasm-apps/push.sh alarm v2 "v2.$TAG" "$REGISTRY" >/dev/null
# The unsigned image must differ from every signed one: same content means
# same digest, and a digest that is signed is signed whatever the tag.
NO_SIGN=1 ORAS_ARGS="--annotation io.flightctl-mcu.e2e=unsigned-$TAG" \
  wasm-apps/push.sh alarm v2 "v2.$TAG-unsigned" "$REGISTRY" >/dev/null
wasm-apps/push.sh trend v1 "v1.$TAG" "$REGISTRY" >/dev/null
if command -v cosign >/dev/null; then
  # Through the plain door: the same storage, so the same signature.
  cosign verify --key "$WORK/signing/signing.pub" --insecure-ignore-tlog --allow-http-registry \
    "$PLAIN_REGISTRY/flightctl-mcu/alarm:v2.$TAG" >/dev/null 2>&1 || fail "cosign does not accept the signature"
  echo "ok: cosign verifies the signature"
fi

# How many times the device has said its name: once per start.
starts() { awk '/device name:/ { n++ } END { print n + 0 }' "$WORK/agent.log" 2>/dev/null || echo 0; }

# await_device <starts before> <seconds>: waits for the device to say its name
# again, and sets DEVICE to it.
await_device() {
  for i in $(seq 1 "$2"); do
    (( $(starts) > $1 )) && break
    sleep 1
  done
  (( $(starts) > $1 )) || fail "the device did not start"
  DEVICE=$(sed -n 's/.*device name: \([a-z0-9]*\).*/\1/p' "$WORK/agent.log" | tail -1)
  echo "device: $DEVICE"
}

# start_agent: runs the device and sets DEVICE to the name it reports. Its
# state stays in $WORK/data (host agent) or in its flash image (Zephyr).
start_agent() {
  local before; before=$(starts)
  run_device
  await_device "$before" 60
}

# run_device: starts the device in the background (AGENT_PID).
run_device() {
  if [[ "$DEVICE_KIND" == zephyr ]]; then
    if [[ ! -f "$ZFLASH" ]]; then
      mkdir -p "$(dirname "$ZFLASH")"
      python3 -c "open('$ZFLASH', 'wb').write(b'\xff' * (2 * 1024 * 1024))"
      cp "$WORK/signing/signing.pub" "$WORK/secrets/signing.pub"
      tools/provision.py --format zephyr --secrets "$WORK/secrets" --server "$AGENT" \
        --labels app=e2e,site=ci --image "$ZFLASH" >/dev/null
    fi
    port/zephyr/run.sh "$ZFLASH" >>"$WORK/agent.log" 2>&1 &
  else
    build/host/fc-host-agent --server "$AGENT" --ca "$WORK/secrets/ca.crt" \
      --enroll-cert "$WORK/secrets/enroll.crt" --enroll-key "$WORK/secrets/enroll.key" \
      --signing-key "$WORK/signing/signing.pub" \
      --data-dir "$WORK/data" --label app=e2e --label site=ci -v >>"$WORK/agent.log" 2>&1 &
  fi
  AGENT_PID=$!
}

# config_stored <threshold> <nth write> <ok message> <failure>: the device
# stored the fleet's config file. The host agent's is a file here; the Zephyr
# device's is inside its flash image, so its log is what says it was written.
config_stored() {
  if [[ "$DEVICE_KIND" == zephyr ]]; then
    (( $(grep -c 'config: wrote /etc/fc-app/alarm.json' "$WORK/agent.log") >= $2 )) || fail "$4"
  else
    grep -q "\"threshold_c\": $1" "$WORK/data/files/_etc_fc-app_alarm.json" || fail "$4"
  fi
  echo "ok: $3"
}

log "start the simulated device"
start_agent

log "enrollment"
for i in $(seq 1 30); do
  flightctl get "enrollmentrequest/$DEVICE" >/dev/null 2>&1 && break
  (( i == 30 )) && fail "no enrollment request"
  sleep 2
done
# Approved by the allow-list script, as a production setup would.
printf 'defaults:\n  labels:\n    site: ci\ndevices:\n  - name: %s\n    labels:\n      stage: ci\n' "$DEVICE" > "$WORK/allowlist.yaml"
tools/auto-approve/auto-approve.py "$WORK/allowlist.yaml"
wait_for 120 "device Online" "st.get('summary', {}).get('status') == 'Online'"
wait_for 30 "status accepted with system info" "st['systemInfo'].get('agentVersion', '').startswith('flightctl-mcu')"

log "registry with TLS: no credentials, then the fleet's"
# The CA alone: the registry asks for a password the device does not have.
REGISTRY_AUTH_FILE="$WORK/no-auth.json" apply_fleet 40 "v1.$TAG"
wait_for 180 "missing credentials reported" "st['updated']['status'] == 'OutOfDate' and 'asks for credentials' in st['updated'].get('info', '')"

log "fleet: config + app v1"
apply_fleet 40 "v1.$TAG"
wait_for 180 "app v1 running" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v1')"
wait_for 60 "device up to date" "st['updated']['status'] == 'UpToDate'"
config_stored 40 1 "config file written" "config file not written"

log "app update v1 -> v2 (no restart)"
pid_before=$AGENT_PID
apply_fleet 40 "v2.$TAG"
wait_for 180 "app v2 running" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v2')"
kill -0 "$pid_before" 2>/dev/null || fail "agent restarted"
grep -q "updating app alarm" "$WORK/agent.log" || fail "no in-place update in the agent log"
echo "ok: updated in place"

log "config change"
before=$(rendered)
apply_fleet 35 "v2.$TAG"
wait_for 180 "new config applied" "d['metadata']['annotations']['device-controller/renderedVersion'] != '$before' and st['config']['renderedVersion'] == d['metadata']['annotations']['device-controller/renderedVersion']"
config_stored 35 2 "config file updated" "new config not written"

log "unsigned app rejected"
apply_fleet 35 "v2.$TAG-unsigned"
wait_for 180 "unsigned app rejected" "st['updated']['status'] == 'OutOfDate' and 'not signed' in st['updated'].get('info', '')"
wait_for 10 "signed app still running" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v2')"

log "registry down during an update"
docker stop fc-registry-tls >/dev/null
apply_fleet 35 "v1.$TAG"
wait_for 120 "download failure reported" "st['updated']['status'] == 'OutOfDate' and 'registry' in st['updated'].get('info', '')"
docker start fc-registry-tls >/dev/null
wait_for 180 "update done once the registry is back" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v1') and st['updated']['status'] == 'UpToDate'"

log "app removal"
apply_fleet 35 ""
wait_for 180 "app removed" "not apps and st['applicationsSummary']['status'] == 'NoApplications'"

log "second example app (trend)"
apply_fleet 35 "v1.$TAG" trend
wait_for 120 "trend app reporting" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').split(' ')[0] in ('rising', 'falling', 'steady')"
apply_fleet 35 ""
wait_for 120 "trend app removed" "not apps"

log "resource alerts"
# Any memory in use is above 0%: Warning after 10 s, sampled every 5 s.
RESOURCES='      resources:
        - monitorType: Memory
          samplingInterval: 5s
          alertRules:
            - severity: Warning
              percentage: 0
              duration: 10s
              description: e2e memory alert'
apply_fleet 35 ""
wait_for 120 "memory Warning reported" "st['resources']['memory'] == 'Warning' and st['summary']['status'] == 'Degraded' and 'Memory' in st['summary'].get('info', '')"
RESOURCES=""
apply_fleet 35 ""
wait_for 120 "alert cleared with the default monitors" "st['resources']['memory'] == 'Healthy' and st['summary']['status'] == 'Online'"

log "management certificate renewal"
# Issued at enrollment for CERT_TTL seconds; the agent renews it after a
# quarter of that. The device must keep reporting after the first one expires.
custom() { flightctl get "device/$DEVICE" -o json | python3 -c "import json,sys; print(json.load(sys.stdin)['status']['systemInfo']['customInfo'].get('$1',''))"; }
first=$(custom mgmtCertNotAfter)
[[ -n "$first" ]] || fail "no mgmtCertNotAfter in the status"
echo "first certificate valid until $first"
wait_for $CERT_TTL "certificate renewed" "ci.get('mgmtCertNotAfter', '$first') > '$first'"
grep -q "management certificate renewed" "$WORK/agent.log" || fail "no renewal in the agent log"
expiry=$(python3 -c "import calendar,time; print(calendar.timegm(time.strptime('$first','%Y-%m-%dT%H:%M:%SZ')))")
wait_s=$(( expiry - $(date +%s) + 5 ))
(( wait_s > 0 )) && { echo "waiting ${wait_s}s for the first certificate to expire"; sleep "$wait_s"; }
# The counters in customInfo change once an hour: a status accepted since is
# shown by the next renewal, every quarter of CERT_TTL.
last=$(custom mgmtCertNotAfter)
wait_for 90 "status accepted after the first certificate expired" "ci.get('mgmtCertNotAfter', '') > '$last' and st['summary']['status'] == 'Online'"

# Where the device reports its heap (the Zephyr build): how low it has been,
# after two apps, their updates and the certificate renewal.
[[ -n "$(custom memMinFreeBytes)" ]] && echo "heap: free $(custom memFreeBytes), least free $(custom memMinFreeBytes)"

log "server restart"
# Kept connections die with the server: the agent must reconnect, report and
# take new specs.
kubectl -n flightctl rollout restart deployment/flightctl-api >/dev/null
kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null
last=$(custom mgmtCertNotAfter)
wait_for 120 "status accepted after the restart" "ci.get('mgmtCertNotAfter', '') > '$last' and st['summary']['status'] == 'Online'"
before=$(rendered)
apply_fleet 30 ""
wait_for 330 "spec applied after the restart" "st['config']['renderedVersion'] == d['metadata']['annotations']['device-controller/renderedVersion'] != '$before'"

log "certificate expired while the server was down: the device enrolls again"
# The certificate lives CERT_TTL seconds and cannot be renewed with the server
# down. Once it has expired the device starts over under a new key, keeping
# its apps and config; the allow list approves the new request.
first=$(custom mgmtCertNotAfter)
expiry=$(python3 -c "import calendar,time; print(calendar.timegm(time.strptime('$first','%Y-%m-%dT%H:%M:%SZ')))")
starts_before=$(starts)
kubectl -n flightctl scale deployment/flightctl-api --replicas=0 >/dev/null
wait_s=$(( expiry - $(date +%s) + 70 ))
(( wait_s > 0 )) && { echo "server down, waiting ${wait_s}s for the certificate to expire"; sleep "$wait_s"; }
kubectl -n flightctl scale deployment/flightctl-api --replicas=1 >/dev/null
kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null
OLD_DEVICE=$DEVICE
if [[ "$DEVICE_KIND" == zephyr ]]; then
  # The Zephyr device reboots by itself and comes back under its new name,
  # possibly before the server is back.
  await_device "$starts_before" 180
  grep -q "enrolling again as a new device" "$WORK/agent.log" || fail "no re-enrollment in the agent log"
  echo "ok: the device dropped its expired identity and rebooted"
else
  for i in $(seq 1 60); do
    kill -0 "$AGENT_PID" 2>/dev/null || break
    (( i == 60 )) && fail "the device did not give up its expired certificate"
    sleep 3
  done
  grep -q "enrolling again as a new device" "$WORK/agent.log" || fail "no re-enrollment in the agent log"
  echo "ok: the device dropped its expired identity"
  start_agent   # the agent stops for the restart; here we start it again
fi
[[ "$DEVICE" != "$OLD_DEVICE" ]] || fail "same device name after re-enrollment"
for i in $(seq 1 30); do
  flightctl get "enrollmentrequest/$DEVICE" >/dev/null 2>&1 && break
  (( i == 30 )) && fail "no enrollment request from the new identity"
  sleep 2
done
printf 'devices:\n  - name: %s\n    labels:\n      stage: ci\n' "$DEVICE" > "$WORK/allowlist.yaml"
tools/auto-approve/auto-approve.py "$WORK/allowlist.yaml"
wait_for 120 "new identity Online" "st.get('summary', {}).get('status') == 'Online'"
if [[ "$DEVICE_KIND" == zephyr ]]; then
  echo "config across re-enrollment: not checked, the file is inside the flash image"
else
  grep -q '"threshold_c": 30' "$WORK/data/files/_etc_fc-app_alarm.json" || fail "config lost across re-enrollment"
  echo "ok: config kept"
fi

log "fleet from Git, the registry's credentials from Kubernetes secrets"
# The same fleet as a file in a Git repository, imported by a ResourceSync:
# tools/fleet/git-export.py spells out the registry and mounts its CA and
# password from secrets (tools/fleet/registry-secrets.sh) instead of putting
# them in the file. The repository is served by git daemon in a container.
mkdir -p "$WORK/git/src"
fleet_yaml 25 "v1.$TAG" > "$WORK/git/e2e.yaml"
tools/fleet/git-export.py "$REGISTRY" "$WORK/git/src/fleets" "$WORK/git/e2e.yaml" >/dev/null
grep -q "auth.json\|BEGIN CERTIFICATE" "$WORK/git/src/fleets/e2e.yaml" && fail "credentials in the exported fleet"
git -C "$WORK/git/src" init -q -b main
git -C "$WORK/git/src" add fleets
git -C "$WORK/git/src" -c user.name=e2e -c user.email=e2e@localhost commit -qm "e2e fleet"
git clone -q --bare "$WORK/git/src" "$WORK/git/fleets.git"
GIT_SERVER="fc-e2e-git-$TAG"
docker run -d --rm --name "$GIT_SERVER" -p "${GIT_PORT:-9418}:9418" -v "$WORK/git:/git:ro" alpine:3.20 sh -c \
  "apk add -q --no-cache git-daemon && git config --global safe.directory '*' &&
   exec git daemon --reuseaddr --export-all --base-path=/git --listen=0.0.0.0 /git" >/dev/null
for i in $(seq 1 30); do
  git ls-remote "git://127.0.0.1:${GIT_PORT:-9418}/fleets.git" >/dev/null 2>&1 && break
  (( i == 30 )) && fail "git daemon not serving the repository"
  sleep 2
done
tools/fleet/registry-secrets.sh >/dev/null
writes_before=$(grep -c 'config: wrote /etc/containers/auth.json' "$WORK/agent.log" || true)
flightctl delete "fleet/$FLEET" >/dev/null   # the ResourceSync owns it from now on
flightctl apply -f - >/dev/null <<EOF
apiVersion: flightctl.io/v1beta1
kind: Repository
metadata:
  name: $FLEET-fleets
spec:
  type: git
  url: git://$IP:${GIT_PORT:-9418}/fleets.git
---
apiVersion: flightctl.io/v1beta1
kind: ResourceSync
metadata:
  name: $FLEET
spec:
  repository: $FLEET-fleets
  targetRevision: main
  path: /fleets
EOF
wait_for 420 "fleet from Git applied, app pulled" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v1') and st['updated']['status'] == 'UpToDate'"
(( $(grep -c 'config: wrote /etc/containers/auth.json' "$WORK/agent.log") > writes_before )) \
  || fail "the credentials did not come from the secret"
grep -q 'config: wrote /etc/containers/certs.d/' "$WORK/agent.log" || fail "the registry CA did not come from the secret"
echo "ok: credentials mounted from the secrets"
config_stored 25 1 "config from Git written" "config from Git not written"

log "decommission"
# Started, then Completed, then the device wipes its key and certificate and
# restarts. It does not enroll again: that takes erasing its storage.
flightctl decommission "device/$DEVICE" >/dev/null
wait_for 120 "device Decommissioned" "st.get('lifecycle', {}).get('status') == 'Decommissioned'"
if [[ "$DEVICE_KIND" != zephyr ]]; then
  for i in $(seq 1 30); do
    kill -0 "$AGENT_PID" 2>/dev/null || break
    (( i == 30 )) && fail "the device did not restart after decommissioning"
    sleep 2
  done
  run_device   # the host agent exits to restart; the Zephyr build reboots
fi
for i in $(seq 1 60); do
  grep -q "decommissioned (Unenroll): not enrolling" "$WORK/agent.log" && break
  (( i == 60 )) && fail "the device did not come back decommissioned"
  sleep 2
done
starts_before=$(starts)
sleep 15
(( $(starts) == starts_before )) || fail "the decommissioned device started enrolling again"
echo "ok: decommissioned, and not enrolling again"
if [[ "$DEVICE_KIND" != zephyr ]]; then
  [[ ! -f "$WORK/data/mgmt.crt" && ! -f "$WORK/data/device.key" ]] || fail "key or certificate left behind"
  echo "ok: key and certificate wiped"
fi

log "PASS"
