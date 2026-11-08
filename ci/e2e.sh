#!/usr/bin/env bash
# End-to-end test: the simulated device (build/host/fc-host-agent) against a
# real Flight Control. Checks enrollment, approval, status, inline config,
# a WASM app install, an update v1 -> v2 without restart, a config change,
# the rejection of an unsigned app, an update while the registry is down,
# the app removal, the renewal of the management certificate (the server is
# told to issue 2-minute certificates for the test) and a server restart. Apps are signed with a throwaway key; when cosign is
# installed, it checks that signature too.
#
#   ci/e2e.sh                 deploy Flight Control on kind + registry, then test
#   SKIP_DEPLOY=1 ci/e2e.sh   test against the cluster and registry already running
#
# Needs: kind, helm, kubectl, flightctl, oras, clang + wasm-ld, python3 (PyYAML),
# and the host agent built in build/host.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source deploy/local-ip.sh
IP="${HOST_IP:-$(local_ip)}"
API="https://api.$IP.nip.io:8443"
AGENT="agent-api.$IP.nip.io:8443"
REGISTRY="${REGISTRY:-$IP:5001}"
TAG="e2e$(date +%s)"
WORK="$(mktemp -d)"
echo "work dir: $WORK"
FLEET=e2e
AGENT_PID=""
DEVICE=""
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
  if [[ -z "${KEEP:-}" ]]; then
    flightctl delete "fleet/$FLEET" >/dev/null 2>&1 || true
    [[ -n "$DEVICE" ]] && flightctl delete "device/$DEVICE" >/dev/null 2>&1 || true
    [[ -n "$DEVICE" ]] && flightctl delete "enrollmentrequest/$DEVICE" >/dev/null 2>&1 || true
    rm -rf "$WORK"
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
apply_fleet() {
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
    if [[ -n "$app" ]]; then
      cat <<EOF
      applications:
        - name: alarm
          appType: container
          image: $REGISTRY/flightctl-mcu/$image:$app
          envVars:
            TICK_MS: "100"
EOF
    fi
  } | flightctl apply -f - >/dev/null
}

rendered() {
  flightctl get "device/$DEVICE" -o json | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["metadata"]["annotations"]["device-controller/renderedVersion"])'
}

if [[ -z "${SKIP_DEPLOY:-}" ]]; then
  log "deploy Flight Control on kind ($IP) and the registry"
  deploy/kind/up.sh "$IP"
  deploy/registry/up.sh
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
flightctl login "$API" --insecure-skip-tls-verify \
  -t "$(kubectl create token flightctl-admin -n flightctl --duration=1h)" >/dev/null

log "enrollment credentials, signing key and WASM apps ($REGISTRY)"
deploy/enrollment.sh "$WORK/secrets" >/dev/null
deploy/signing/keygen.sh "$WORK/signing" >/dev/null
export SIGNING_KEY="$WORK/signing/signing.key"
wasm-apps/build.sh alarm "v1.$TAG" "$REGISTRY" >/dev/null
wasm-apps/build.sh alarm "v2.$TAG" "$REGISTRY" >/dev/null
# The unsigned image must differ from every signed one: same content means
# same digest, and a digest that is signed is signed whatever the tag.
(cd "wasm-apps/build/alarm-v2.$TAG" && oras push --plain-http --disable-path-validation \
  --annotation "io.flightctl-mcu.e2e=unsigned-$TAG" --config config.json:application/vnd.wasm.config.v0+json \
  "$REGISTRY/flightctl-mcu/alarm:v2.$TAG-unsigned" alarm.wasm:application/vnd.wasm.content.layer.v1+wasm >/dev/null)
wasm-apps/build.sh trend "v1.$TAG" "$REGISTRY" >/dev/null
if command -v cosign >/dev/null; then
  cosign verify --key "$WORK/signing/signing.pub" --insecure-ignore-tlog --allow-http-registry \
    "$REGISTRY/flightctl-mcu/alarm:v2.$TAG" >/dev/null 2>&1 || fail "cosign does not accept the signature"
  echo "ok: cosign verifies the signature"
fi

log "start the simulated device"
build/host/fc-host-agent --server "$AGENT" --ca "$WORK/secrets/ca.crt" \
  --enroll-cert "$WORK/secrets/enroll.crt" --enroll-key "$WORK/secrets/enroll.key" \
  --signing-key "$WORK/signing/signing.pub" \
  --data-dir "$WORK/data" --label app=e2e --label site=ci -v >"$WORK/agent.log" 2>&1 &
AGENT_PID=$!
for i in $(seq 1 30); do
  DEVICE=$(sed -n 's/.*device name: \([a-z0-9]*\).*/\1/p' "$WORK/agent.log" | head -1)
  [[ -n "$DEVICE" ]] && break
  sleep 1
done
[[ -n "$DEVICE" ]] || fail "agent did not start"
echo "device: $DEVICE"

log "enrollment"
for i in $(seq 1 30); do
  flightctl get "enrollmentrequest/$DEVICE" >/dev/null 2>&1 && break
  (( i == 30 )) && fail "no enrollment request"
  sleep 2
done
# Approved by the allow-list script, as a production setup would.
printf 'defaults:\n  labels:\n    site: ci\ndevices:\n  - name: %s\n    labels:\n      stage: ci\n' "$DEVICE" > "$WORK/allowlist.yaml"
deploy/auto-approve/auto-approve.py "$WORK/allowlist.yaml"
wait_for 120 "device Online" "st.get('summary', {}).get('status') == 'Online'"
wait_for 30 "status accepted with system info" "ci.get('agent') == 'flightctl-mcu' and st['systemInfo'].get('agentVersion')"

log "fleet: config + app v1"
apply_fleet 40 "v1.$TAG"
wait_for 180 "app v1 running" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v1')"
wait_for 60 "device up to date" "st['updated']['status'] == 'UpToDate'"
grep -q '"threshold_c": 40' "$WORK/data/files/_etc_fc-app_alarm.json" || fail "config file not written"
echo "ok: config file written"

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
grep -q '"threshold_c": 35' "$WORK/data/files/_etc_fc-app_alarm.json" || fail "new config not written"
echo "ok: config file updated"

log "unsigned app rejected"
apply_fleet 35 "v2.$TAG-unsigned"
wait_for 180 "unsigned app rejected" "st['updated']['status'] == 'OutOfDate' and 'not signed' in st['updated'].get('info', '')"
wait_for 10 "signed app still running" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v2')"

log "registry down during an update"
docker stop fc-registry >/dev/null
apply_fleet 35 "v1.$TAG"
wait_for 120 "download failure reported" "st['updated']['status'] == 'OutOfDate' and 'registry' in st['updated'].get('info', '')"
docker start fc-registry >/dev/null
wait_for 180 "update done once the registry is back" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').startswith('v1') and st['updated']['status'] == 'UpToDate'"

log "app removal"
apply_fleet 35 ""
wait_for 180 "app removed" "not apps and st['applicationsSummary']['status'] == 'NoApplications'"

log "second example app (trend)"
apply_fleet 35 "v1.$TAG" trend
wait_for 120 "trend app reporting" "apps and apps[0]['status'] == 'Running' and ci.get('app.alarm', '').split(' ')[0] in ('rising', 'falling', 'steady')"
apply_fleet 35 ""
wait_for 120 "trend app removed" "not apps"

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
tx=$(custom netTxBytes)
wait_for 90 "status accepted after the first certificate expired" "int(ci.get('netTxBytes', 0)) > $tx and st['summary']['status'] == 'Online'"

log "server restart"
# Kept connections die with the server: the agent must reconnect, report and
# take new specs.
kubectl -n flightctl rollout restart deployment/flightctl-api >/dev/null
kubectl -n flightctl rollout status deployment/flightctl-api --timeout=10m >/dev/null
tx=$(custom netTxBytes)
wait_for 120 "status accepted after the restart" "int(ci.get('netTxBytes', 0)) > $tx and st['summary']['status'] == 'Online'"
before=$(rendered)
apply_fleet 30 ""
wait_for 330 "spec applied after the restart" "st['config']['renderedVersion'] == d['metadata']['annotations']['device-controller/renderedVersion'] != '$before'"

log "PASS"
