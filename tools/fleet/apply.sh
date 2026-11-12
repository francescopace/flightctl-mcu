#!/usr/bin/env bash
# Applies a fleet manifest, replacing ${REGISTRY} with the OCI registry the
# devices pull from (default: this machine's LAN IP, port 5001, plain HTTP).
# For the registry with TLS and a password (tools/registry/up.sh: REGISTRY=
# $(cat .secrets/registry/host)), its CA and the credentials are added to the
# fleet as config files (tools/fleet/registry-config.py). Needs PyYAML.
# Usage: [REGISTRY=host:port] tools/fleet/apply.sh [tools/fleet/idf-lab.yaml]
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../local-ip.sh"
REGISTRY="${REGISTRY:-$(local_ip):5001}"
FILE="${1:-$DIR/idf-lab.yaml}"
sed "s|\${REGISTRY}|$REGISTRY|g" "$FILE" | "$DIR/registry-config.py" "$REGISTRY" | flightctl apply -f -
