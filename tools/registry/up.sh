#!/usr/bin/env bash
# Local OCI registry for firmware and WASM images, reachable from the LAN over
# plain HTTP (development only: signatures protect the images, not their contents from being read). Port 5000 is taken by AirPlay on macOS, so 5001.
set -euo pipefail
source "$(dirname "$0")/../local-ip.sh"
PORT="${PORT:-5001}"
docker volume create fc-registry >/dev/null
docker rm -f fc-registry >/dev/null 2>&1 || true
docker run -d --name fc-registry --restart unless-stopped \
  -p "0.0.0.0:${PORT}:5000" -v fc-registry:/var/lib/registry \
  -e REGISTRY_STORAGE_DELETE_ENABLED=true \
  docker.io/library/registry:3
echo "registry on http://$(local_ip):${PORT}"
