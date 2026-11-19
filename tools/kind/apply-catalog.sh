#!/usr/bin/env bash
# Restore the MCU lab catalog through the kind cluster's Flight Control API.
# Usage: tools/kind/apply-catalog.sh [--if-missing] [HOST_IP]
set -euo pipefail
cd "$(dirname "$0")"
source ../local-ip.sh

if_missing=false
if [[ ${1:-} == --if-missing ]]; then
  if_missing=true
  shift
fi
HOST_IP="${1:-$(local_ip)}"
API="https://api.$HOST_IP.nip.io:8443"
KUBE=(kubectl --context kind-flightctl -n flightctl)
config_dir=$(mktemp -d)
trap 'rm -rf "$config_dir"' EXIT

"${KUBE[@]}" rollout status deployment/flightctl-api --timeout=10m
"${KUBE[@]}" get secret flightctl-ca-bundle \
  -o go-template='{{index .data "ca-bundle.crt" | base64decode}}' > "$config_dir/ca.pem"
# The API pod can be ready before the gateway starts forwarding requests.
curl --silent --show-error --fail --output /dev/null --cacert "$config_dir/ca.pem" \
  --retry 60 --retry-all-errors --retry-delay 2 --max-time 5 --retry-max-time 180 \
  "$API/api/v1/auth/config"

token=$("${KUBE[@]}" create token flightctl-admin --duration=10m)
flightctl login "$API" --config-dir "$config_dir" --certificate-authority "$config_dir/ca.pem" -t "$token" >/dev/null
if [[ $if_missing == true ]]; then
  catalog=$(flightctl get catalogs --config-dir "$config_dir" --field-selector metadata.name=mcu-lab -o name)
  if [[ -n $catalog ]]; then
    echo "Catalog mcu-lab already exists."
    exit 0
  fi
fi
sed "s|\${HOST_IP}|$HOST_IP|g" mcu-lab.yaml | flightctl apply --config-dir "$config_dir" -f -
