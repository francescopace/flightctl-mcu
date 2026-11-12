#!/usr/bin/env bash
# Pushes a WASM app already built by wasm-apps/build.sh as an OCI artifact
# (CNCF wasm artifact layout: config application/vnd.wasm.config.v0+json, one
# application/vnd.wasm.content.layer.v1+wasm layer), then signs it.
# Usage: wasm-apps/push.sh <app> <build version> [tag] [REGISTRY]
#   pushes wasm-apps/build/<app>-<build version>/<app>.wasm as <app>:<tag>
#   (tag defaults to the build version). REGISTRY defaults to this machine's
#   LAN IP, port 5001. Signed when a signing key is there ($SIGNING_KEY or
#   .secrets/signing.key); NO_SIGN=1 skips it. Needs oras.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
source "$DIR/../tools/local-ip.sh"
APP="${1:?app}"; VER="${2:?build version}"; TAG="${3:-$VER}"; REG="${4:-${REGISTRY:-$(local_ip):5001}}"
OUT="$DIR/build/$APP-$VER"
[[ -f "$OUT/$APP.wasm" ]] || { echo "no $OUT/$APP.wasm: build it with wasm-apps/build.sh $APP $VER" >&2; exit 1; }
cd "$OUT"
printf '{"architecture":"wasm","os":"wasip1"}' > config.json
oras push --plain-http --disable-path-validation ${ORAS_ARGS:-} \
  --config config.json:application/vnd.wasm.config.v0+json \
  "$REG/flightctl-mcu/$APP:$TAG" "$APP.wasm:application/vnd.wasm.content.layer.v1+wasm" | tail -1
KEY="${SIGNING_KEY:-$DIR/../.secrets/signing.key}"
if [[ -z "${NO_SIGN:-}" && -f "$KEY" ]]; then
  "$DIR/../tools/signing/sign.py" --key "$KEY" "$REG/flightctl-mcu/$APP:$TAG"
else
  echo "not signed (no signing key)"
fi
