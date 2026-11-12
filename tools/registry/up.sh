#!/usr/bin/env bash
# Local OCI registry for firmware and WASM images: one storage, two doors.
#  - Plain HTTP on PORT (5001; 5000 is taken by AirPlay on macOS), for
#    development and for firmware older than TLS: anyone on the LAN can read
#    and push. Signatures protect the images from tampering, not from being
#    read.
#  - TLS with a password on TLS_PORT (5443), as registry.<LAN IP>.nip.io: what
#    devices pull from in a deployment (docs/REGISTRY.md).
# The CA, the server certificate, the password and the auth.json for devices
# and push tools are made once in .secrets/registry/ (SECRETS); the
# certificate again when the LAN IP has changed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/tools/local-ip.sh"
IP="${HOST_IP:-$(local_ip)}"
PORT="${PORT:-5001}"
TLS_PORT="${TLS_PORT:-5443}"
SECRETS="${SECRETS:-$ROOT/.secrets/registry}"
HOST="registry.$IP.nip.io"
USER_NAME=device
mkdir -p "$SECRETS"
chmod 700 "$SECRETS"
cd "$SECRETS"

if [[ ! -f ca.key ]]; then
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out ca.key 2>/dev/null
  openssl req -x509 -new -key ca.key -sha256 -days 3650 -subj "/CN=flightctl-mcu registry CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.crt
  rm -f server.crt
fi
if [[ ! -f server.crt ]] || ! openssl x509 -in server.crt -noout -checkhost "$HOST" 2>/dev/null | grep -q 'does match'; then
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out server.key 2>/dev/null
  openssl req -new -key server.key -subj "/CN=$HOST" -out server.csr
  printf 'subjectAltName=DNS:%s,DNS:localhost,IP:%s,IP:127.0.0.1\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature\nbasicConstraints=critical,CA:FALSE\n' \
    "$HOST" "$IP" > server.ext
  openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -sha256 \
    -extfile server.ext -out server.crt 2>/dev/null
  rm -f server.csr server.ext
fi
if [[ ! -f password ]]; then
  (umask 077; openssl rand -hex 16 > password)
fi
if command -v htpasswd >/dev/null; then
  htpasswd -Bbn "$USER_NAME" "$(cat password)" > htpasswd
else
  docker run --rm docker.io/library/httpd:2 htpasswd -Bbn "$USER_NAME" "$(cat password)" > htpasswd
fi
# podman's auth.json, which the Linux agent reads too: for devices (a config
# file in the fleet, tools/fleet/apply.sh) and for the push tools.
(umask 077; python3 - "$HOST:$TLS_PORT" "$USER_NAME" "$(cat password)" > auth.json <<'PY'
import base64, json, sys
host, user, password = sys.argv[1:]
print(json.dumps({"auths": {host: {"auth": base64.b64encode(f"{user}:{password}".encode()).decode()}}}))
PY
)
echo "$HOST:$TLS_PORT" > host

docker volume create fc-registry >/dev/null
docker rm -f fc-registry fc-registry-tls >/dev/null 2>&1 || true
docker run -d --name fc-registry --restart unless-stopped \
  -p "0.0.0.0:${PORT}:5000" -v fc-registry:/var/lib/registry \
  -e REGISTRY_STORAGE_DELETE_ENABLED=true \
  docker.io/library/registry:3 >/dev/null
docker run -d --name fc-registry-tls --restart unless-stopped \
  -p "0.0.0.0:${TLS_PORT}:5000" -v fc-registry:/var/lib/registry \
  -v "$SECRETS/server.crt:/certs/server.crt:ro" -v "$SECRETS/server.key:/certs/server.key:ro" \
  -v "$SECRETS/htpasswd:/auth/htpasswd:ro" \
  -e REGISTRY_HTTP_TLS_CERTIFICATE=/certs/server.crt -e REGISTRY_HTTP_TLS_KEY=/certs/server.key \
  -e REGISTRY_AUTH=htpasswd -e REGISTRY_AUTH_HTPASSWD_REALM=flightctl-mcu \
  -e REGISTRY_AUTH_HTPASSWD_PATH=/auth/htpasswd \
  -e REGISTRY_STORAGE_DELETE_ENABLED=true \
  docker.io/library/registry:3 >/dev/null
echo "registry on http://$IP:${PORT} and https://$HOST:${TLS_PORT} (user $USER_NAME, $SECRETS)"
