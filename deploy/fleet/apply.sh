#!/usr/bin/env bash
# Applies a fleet manifest, replacing ${REGISTRY} with the OCI registry the
# devices pull from (default: this machine's LAN IP, port 5001).
# Usage: [REGISTRY=host:port] deploy/fleet/apply.sh [deploy/fleet/alarm-lab.yaml]
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../local-ip.sh"
REGISTRY="${REGISTRY:-$(local_ip):5001}"
FILE="${1:-$DIR/alarm-lab.yaml}"
sed "s|\${REGISTRY}|$REGISTRY|g" "$FILE" | flightctl apply -f -
