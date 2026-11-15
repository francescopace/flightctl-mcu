#!/usr/bin/env bash
# Applies a fleet manifest, replacing ${REGISTRY} with the OCI registry the
# devices pull from: by default the local registry's door with TLS and a
# password (tools/registry/up.sh, .secrets/registry/host), as devices pull
# over TLS only. Its CA and the credentials are added to the fleet as config
# files (tools/fleet/registry-config.py). Needs PyYAML.
# Usage: [REGISTRY=host:port] tools/fleet/apply.sh [tools/fleet/idf-lab.yaml]
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../local-ip.sh"
if [[ -z "${REGISTRY:-}" ]]; then
  HOST_FILE="$DIR/../../.secrets/registry/host"
  [[ -f "$HOST_FILE" ]] || { echo "set REGISTRY, or start the local registry (tools/registry/up.sh)" >&2; exit 1; }
  REGISTRY="$(cat "$HOST_FILE")"
fi
FILE="${1:-$DIR/idf-lab.yaml}"
sed "s|\${REGISTRY}|$REGISTRY|g" "$FILE" | "$DIR/registry-config.py" "$REGISTRY" | flightctl apply -f -
