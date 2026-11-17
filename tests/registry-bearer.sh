#!/usr/bin/env bash
# The device's registry client (core/src/fc_oci.c and fc_http.c, built for
# the host as fc-oci-pull) against registries that ask for Bearer tokens, as
# Harbor, Quay, GitLab and GHCR do. No Flight Control, no board.
#
# In Docker, on a network of their own:
#   - cesanta/docker_auth, the token service, on https :AUTH_PORT;
#   - registry:3 with token auth on https :REG_PORT, its realm on the token
#     service's host (another host, as GitLab has it);
#   - registry:3 and the token service behind nginx on :PROXY_PORT, the realm
#     on the registry's own host and port (as Harbor, Quay and GHCR have it);
#   - with QUAY=1, Quay itself (quay.io/projectquay/quay, QUAY_IMAGE) with
#     Postgres and Redis on :QUAY_PORT: an organization per repository, the
#     device's account a robot account with read access to flightctl-mcu
#     (about 1 GB of memory, and a minute or two to start).
# A CA of its own, an account that pushes and a pull-only one for the device.
# The tools push (tools/fw-push.py, signed; oras), then fc-oci-pull pulls as
# a device: multi-chip index, signature, firmware blob, a WASM layer, with
# the CA and auth.json where a fleet puts them; and what fails: a wrong
# password, a repository the account may not read, no credentials, no CA
# for the token host, a token that runs out between the manifests and the
# blob (about 70 s: the token lives 5 s and registry:3 allows 60 s of clock
# skew; QUICK=1 skips it).
#
# Needs docker, oras, openssl, htpasswd, python3, cmake, ninja, mbedTLS 4.
# Usage: [REG_PORT=9443 AUTH_PORT=9444 PROXY_PORT=9445] [QUICK=1] [QUAY=1 QUAY_PORT=9446] \
#          tests/registry-bearer.sh
# Everything is removed at the end, the containers too (KEEP=1 keeps them).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source tools/local-ip.sh
REG_PORT="${REG_PORT:-9443}" AUTH_PORT="${AUTH_PORT:-9444}" PROXY_PORT="${PROXY_PORT:-9445}"
QUAY_PORT="${QUAY_PORT:-9446}"
case "$(uname -m)" in arm64 | aarch64) QUAY_ARCH=-arm64 ;; *) QUAY_ARCH="" ;; esac
QUAY_IMAGE="${QUAY_IMAGE:-quay.io/projectquay/quay:v3.17.3$QUAY_ARCH}"
# oras refuses a realm on a loopback address: the LAN IP, through nip.io.
IP="$(local_ip)"
REG_HOST="reg.$IP.nip.io" AUTH_HOST="auth.$IP.nip.io"
REG="$REG_HOST:$REG_PORT" PROXY="$REG_HOST:$PROXY_PORT"
QUAY_HOST="quay.$IP.nip.io" QUAY="quay.$IP.nip.io:$QUAY_PORT"
W="$ROOT/build/registry-bearer"
NET=fc-bearer-test
CONTAINERS=(fc-bearer-auth fc-bearer-registry fc-bearer-registry2 fc-bearer-nginx fc-bearer-quay fc-bearer-quay-db
            fc-bearer-quay-redis)

cleanup() {
  if [[ -z "${KEEP:-}" ]]; then
    docker rm -f "${CONTAINERS[@]}" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT
cleanup
rm -rf "$W"
mkdir -p "$W/certs" "$W/auth" "$W/nginx" "$W/quay/conf" "$W/quay/storage"
chmod 777 "$W/quay/storage"

{ cmake -S . -B build/oci-pull -G Ninja && cmake --build build/oci-pull --target fc-oci-pull; } > "$W/build.log" 2>&1 ||
  { tail -20 "$W/build.log"; exit 1; }
PULL="$ROOT/build/oci-pull/fc-oci-pull"

# ---- certificates, accounts, the token service's key ----------------------
(
  cd "$W/certs"
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca.key -out ca.crt -days 2 \
    -subj /CN=fc-bearer-test-ca -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
  for n in reg auth; do
    openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout $n.key -out $n.csr \
      -subj "/CN=$n.$IP.nip.io" 2>/dev/null
    printf 'subjectAltName=DNS:%s\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature\n' \
      "$n.$IP.nip.io" > $n.ext
    openssl x509 -req -in $n.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out $n.crt -days 2 \
      -extfile $n.ext 2>/dev/null
  done
  openssl req -newkey rsa:2048 -nodes -keyout quay.key -out quay.csr -subj "/CN=quay.$IP.nip.io" 2>/dev/null
  printf 'subjectAltName=DNS:%s\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature,keyEncipherment\n' \
    "quay.$IP.nip.io" > quay.ext
  openssl x509 -req -in quay.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out quay.crt -days 2 -extfile quay.ext \
    2>/dev/null
  openssl req -x509 -newkey rsa:2048 -nodes -keyout token.key -out token.crt -days 2 -subj /CN=fc-test-auth 2>/dev/null
  chmod 644 ./*.key
  # registry:3 finds the signing key by the token's kid, which docker_auth
  # writes in libtrust's form: a JWKS with that kid.
  openssl x509 -in token.crt -pubkey -noout | openssl pkey -pubin -outform DER > token.spki
  python3 - "$(openssl x509 -in token.crt -noout -modulus | cut -d= -f2)" <<'EOF'
import base64, hashlib, json, sys
b = base64.b32encode(hashlib.sha256(open("token.spki", "rb").read()).digest()[:30]).decode()
kid = ":".join(b[i:i + 4] for i in range(0, len(b), 4))
u = lambda x: base64.urlsafe_b64encode(x).rstrip(b"=").decode()
key = {"kty": "RSA", "kid": kid, "use": "sig", "alg": "RS256",
       "n": u(bytes.fromhex(sys.argv[1])), "e": u((65537).to_bytes(3, "big"))}
json.dump({"keys": [key]}, open("jwks.json", "w"))
EOF
)
openssl ecparam -name prime256v1 -genkey -noout | openssl pkcs8 -topk8 -nocrypt -out "$W/signing.key"
openssl ec -in "$W/signing.key" -pubout -out "$W/signing.pub" 2>/dev/null
DEV_PASS="$(openssl rand -hex 12)" PUSH_PASS="$(openssl rand -hex 12)"
cat > "$W/auth/config.yml" <<EOF
server:
  addr: ":5001"
  certificate: "/certs/auth.crt"
  key: "/certs/auth.key"
token:
  issuer: "fc-test-auth"
  expiration: 5
  certificate: "/certs/token.crt"
  key: "/certs/token.key"
users:
  "device":
    password: "$(htpasswd -nbBC 10 x "$DEV_PASS" | cut -d: -f2)"
  "pusher":
    password: "$(htpasswd -nbBC 10 x "$PUSH_PASS" | cut -d: -f2)"
  "": {}
acl:
  - match: {account: "pusher"}
    actions: ["*"]
  - match: {account: "device", name: "flightctl-mcu/*"}
    actions: ["pull"]
  - match: {account: "", name: "public/*"}
    actions: ["pull"]
EOF
authjson() { # file key user password [plain]: podman's auth.json
  python3 - "$@" <<'EOF'
import base64, json, sys
f, key, user, pw = sys.argv[1:5]
e = {"username": user, "password": pw} if len(sys.argv) > 5 else \
    {"auth": base64.b64encode(f"{user}:{pw}".encode()).decode()}
json.dump({"auths": {key: e}}, open(f, "w"))
EOF
}
authjson "$W/pusher.json" "$REG" pusher "$PUSH_PASS"
python3 -c "import json,sys; a=json.load(open(sys.argv[1])); a['auths'][sys.argv[2]]=a['auths'][sys.argv[3]]; json.dump(a,open(sys.argv[1],'w'))" \
  "$W/pusher.json" "$PROXY" "$REG"
authjson "$W/device.json" "$REG" device "$DEV_PASS"
addauth() { # file key user password: one more registry in an auth.json
  python3 - "$@" <<'EOF'
import base64, json, sys
f, key, user, pw = sys.argv[1:5]
a = json.load(open(f))
a["auths"][key] = {"auth": base64.b64encode(f"{user}:{pw}".encode()).decode()}
json.dump(a, open(f, "w"))
EOF
}
authjson "$W/device-proxy.json" "$PROXY" device "$DEV_PASS" plain
authjson "$W/wrong.json" "$REG" device wrong-password
echo '{"auths":{}}' > "$W/none.json"

# ---- the registries -------------------------------------------------------
cat > "$W/nginx/default.conf" <<EOF
server {
  listen 443 ssl;
  http2 on;
  ssl_certificate /certs/reg.crt;
  ssl_certificate_key /certs/reg.key;
  client_max_body_size 0;
  location /v2/ {
    proxy_pass http://fc-bearer-registry2:5000;
    proxy_set_header Host \$http_host;
    proxy_set_header X-Forwarded-Proto https;
    proxy_buffering off;
    proxy_request_buffering off;
  }
  location /service/token {
    proxy_pass https://fc-bearer-auth:5001/auth;
  }
}
EOF
docker network create "$NET" >/dev/null
docker run -d --name fc-bearer-auth --network "$NET" -p "$AUTH_PORT:5001" -v "$W/certs:/certs:ro" \
  -v "$W/auth:/config:ro" cesanta/docker_auth:1.14.0 --v=2 --alsologtostderr /config/config.yml >/dev/null
registry() { # name realm [tls]
  local tls=()
  [[ -n "${3:-}" ]] && tls=(-p "$REG_PORT:5000" -e REGISTRY_HTTP_TLS_CERTIFICATE=/certs/reg.crt
                            -e REGISTRY_HTTP_TLS_KEY=/certs/reg.key)
  docker run -d --name "$1" --network "$NET" ${tls[@]+"${tls[@]}"} -v "$W/certs:/certs:ro" -e REGISTRY_AUTH=token \
    -e REGISTRY_AUTH_TOKEN_REALM="$2" -e REGISTRY_AUTH_TOKEN_SERVICE=fc-test-registry \
    -e REGISTRY_AUTH_TOKEN_ISSUER=fc-test-auth -e REGISTRY_AUTH_TOKEN_JWKS=/certs/jwks.json registry:3 >/dev/null
}
registry fc-bearer-registry "https://$AUTH_HOST:$AUTH_PORT/auth" tls
registry fc-bearer-registry2 "https://$PROXY/service/token"
docker run -d --name fc-bearer-nginx --network "$NET" -p "$PROXY_PORT:443" -v "$W/certs:/certs:ro" \
  -v "$W/nginx:/etc/nginx/conf.d:ro" nginx:1.27-alpine >/dev/null
for url in "https://$REG/v2/" "https://$AUTH_HOST:$AUTH_PORT/" "https://$PROXY/v2/"; do
  for _ in $(seq 1 40); do
    curl -s -o /dev/null --cacert "$W/certs/ca.crt" "$url" && break
    sleep 0.5
  done
done

if [[ -n "${QUAY:-}" ]]; then
  cp "$W/certs/quay.crt" "$W/quay/conf/ssl.cert"
  cp "$W/certs/quay.key" "$W/quay/conf/ssl.key"
  DB_PASS="$(openssl rand -hex 12)" QUAY_ADMIN_PASS="$(openssl rand -hex 12)"
  cat > "$W/quay/conf/config.yaml" <<EOF
AUTHENTICATION_TYPE: Database
BUILDLOGS_REDIS: {host: fc-bearer-quay-redis, port: 6379}
USER_EVENTS_REDIS: {host: fc-bearer-quay-redis, port: 6379}
DATABASE_SECRET_KEY: $(openssl rand -hex 24)
SECRET_KEY: $(openssl rand -hex 24)
DB_URI: postgresql://quay:$DB_PASS@fc-bearer-quay-db/quay
DEFAULT_TAG_EXPIRATION: 2w
TAG_EXPIRATION_OPTIONS: [2w]
DISTRIBUTED_STORAGE_CONFIG:
  default: [LocalStorage, {storage_path: /datastorage/registry}]
DISTRIBUTED_STORAGE_DEFAULT_LOCATIONS: []
DISTRIBUTED_STORAGE_PREFERENCE: [default]
FEATURE_MAILING: false
FEATURE_USER_INITIALIZE: true
FEATURE_USER_CREATION: false
SUPER_USERS: [quayadmin]
SERVER_HOSTNAME: $QUAY
PREFERRED_URL_SCHEME: https
SETUP_COMPLETE: true
FEATURE_GENERAL_OCI_SUPPORT: true
EOF
  chmod 644 "$W/quay/conf/"*
  docker run -d --name fc-bearer-quay-db --network "$NET" -e POSTGRES_USER=quay -e POSTGRES_PASSWORD="$DB_PASS" \
    -e POSTGRES_DB=quay postgres:15-alpine >/dev/null
  docker run -d --name fc-bearer-quay-redis --network "$NET" redis:7-alpine >/dev/null
  for _ in $(seq 1 60); do
    docker exec fc-bearer-quay-db psql -U quay -d quay -c 'CREATE EXTENSION IF NOT EXISTS pg_trgm;' >/dev/null 2>&1 && break
    sleep 1
  done
  docker run -d --name fc-bearer-quay --network "$NET" -p "$QUAY_PORT:8443" -v "$W/quay/conf:/conf/stack" \
    -v "$W/quay/storage:/datastorage" "$QUAY_IMAGE" >/dev/null
  for _ in $(seq 1 150); do
    [[ "$(curl -s -o /dev/null -w '%{http_code}' --cacert "$W/certs/ca.crt" "https://$QUAY/v2/")" == 401 ]] && break
    sleep 2
  done
  # The first administrator, once the API answers (after the registry does).
  QUAY_API=""
  for _ in $(seq 1 60); do
    QUAY_API="$(curl -sf --cacert "$W/certs/ca.crt" -H 'Content-Type: application/json' -X POST \
      "https://$QUAY/api/v1/user/initialize" \
      -d "{\"username\":\"quayadmin\",\"password\":\"$QUAY_ADMIN_PASS\",\"email\":\"admin@example.invalid\",\"access_token\":true}" |
      python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' 2>/dev/null)" && break
    sleep 2
  done
  [[ -n "$QUAY_API" ]] || { echo "Quay did not start: docker logs fc-bearer-quay" >&2; exit 1; }
  qapi() { # method path [json]: Quay's API as its administrator
    curl -sf --cacert "$W/certs/ca.crt" -H "Authorization: Bearer $QUAY_API" -H 'Content-Type: application/json' \
      -X "$1" "https://$QUAY/api/v1/$2" ${3:+-d "$3"}
  }
  for org in flightctl-mcu public other; do
    qapi POST organization/ "{\"name\":\"$org\",\"email\":\"$org@example.invalid\"}" >/dev/null
  done
  # The device's account: a robot that reads every repository of flightctl-mcu.
  ROBOT_TOKEN="$(qapi PUT organization/flightctl-mcu/robots/device '{"description":"pull only"}' |
    python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')"
  qapi POST organization/flightctl-mcu/prototypes \
    '{"role":"read","delegate":{"kind":"user","name":"flightctl-mcu+device"}}' >/dev/null
  addauth "$W/pusher.json" "$QUAY" quayadmin "$QUAY_ADMIN_PASS"
  authjson "$W/device-quay.json" "$QUAY" flightctl-mcu+device "$ROBOT_TOKEN"
  addauth "$W/wrong.json" "$QUAY" flightctl-mcu+device not-the-token
fi

# ---- push, as a release is pushed -----------------------------------------
head -c 600000 /dev/urandom > "$W/fw-a.bin"
head -c 700000 /dev/urandom > "$W/fw-b.bin"
{ printf '\0asm\1\0\0\0'; head -c 3000 /dev/urandom; } > "$W/app.wasm"
WASM=application/vnd.wasm.content.layer.v1+wasm
push() { # registry
  # tools/fw-push.py, for two made-up chips, with an empty SBOM (the real
  # one reads the ESP-IDF and Zephyr trees).
  REGISTRY_CA="$W/certs/ca.crt" REGISTRY_AUTH_FILE="$W/pusher.json" SIGNING_KEY="$W/signing.key" python3 -c '
import runpy, sys
sys.path.insert(0, "tools")
import sbom
sbom.make_sbom = lambda *a, **k: {"bomFormat": "CycloneDX", "specVersion": "1.6", "components": []}
sys.argv = ["tools/fw-push.py"] + sys.argv[1:]
runpy.run_path("tools/fw-push.py", run_name="__main__")' \
    "$1/flightctl-mcu/fw:t1" chip-a="$W/fw-a.bin" chip-b="$W/fw-b.bin" >/dev/null
  (cd "$W" && oras push --ca-file certs/ca.crt --registry-config pusher.json --disable-path-validation \
     "$1/flightctl-mcu/app:t1" "app.wasm:$WASM" >/dev/null)
  REGISTRY_CA="$W/certs/ca.crt" REGISTRY_AUTH_FILE="$W/pusher.json" \
    tools/signing/sign.py --key "$W/signing.key" "$1/flightctl-mcu/app:t1" >/dev/null
  for repo in public other; do
    oras cp --from-ca-file "$W/certs/ca.crt" --from-registry-config "$W/pusher.json" --to-ca-file "$W/certs/ca.crt" \
      --to-registry-config "$W/pusher.json" "$1/flightctl-mcu/app:t1" "$1/$repo/app:t1" >/dev/null
    REGISTRY_CA="$W/certs/ca.crt" REGISTRY_AUTH_FILE="$W/pusher.json" \
      tools/signing/sign.py --key "$W/signing.key" "$1/$repo/app:t1" >/dev/null
  done
}
push "$REG"
push "$PROXY"
if [[ -n "${QUAY:-}" ]]; then
  push "$QUAY"
  qapi POST repository/public/app/changevisibility '{"visibility":"public"}' >/dev/null
fi

# ---- pull, as a device ----------------------------------------------------
fails=0
tokens() { # tokens given so far: docker_auth's, and Quay's (its nginx's log)
  local a q=0
  a=$(docker logs fc-bearer-auth 2>&1 | grep -c "New token" || true)
  [[ -n "${QUAY:-}" ]] && q=$(docker logs fc-bearer-quay 2>&1 | grep -c '"GET /v2/auth?[^"]*HTTP/2.0" 200' || true)
  echo $((a + q))
}
# pull <name> <auth.json> <expected: ok | error text> <fc-oci-pull args...>
pull() {
  local name="$1" auth="$2" want="$3" out rc=0 t0
  shift 3
  t0=$(tokens)
  rm -rf "$W/dev" "$W/out"
  local cas=(--ca "$REG_HOST=$W/certs/ca.crt" --ca "$QUAY_HOST=$W/certs/ca.crt")
  [[ -z "${NO_AUTH_CA:-}" ]] && cas+=(--ca "$AUTH_HOST=$W/certs/ca.crt")
  out=$("$PULL" --data "$W/dev" "${cas[@]}" --auth "$auth" --key "$W/signing.pub" "$@" 2>&1) || rc=$?
  LAST_TOKENS=$(( $(tokens) - t0 ))
  if [[ "$want" == ok && $rc -eq 0 ]] || [[ "$want" != ok && $rc -ne 0 && "$out" == *"$want"* ]]; then
    echo "ok   $name"
  else
    echo "FAIL $name (exit $rc)"; echo "$out" | tail -3 | sed 's/^/     /'; fails=$((fails + 1))
  fi
  for secret in "$DEV_PASS" "$(printf %s "device:$DEV_PASS" | base64)" ${ROBOT_TOKEN:+"$ROBOT_TOKEN"}; do
    if [[ "$out" == *"$secret"* ]]; then
      echo "FAIL $name: a secret in the output"; fails=$((fails + 1))
    fi
  done
}
same() { # <name> <got> <expected>
  if cmp -s "$2" "$3"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}
expect_tokens() { # <name> <n>
  if [[ "$LAST_TOKENS" -eq "$2" ]]; then echo "ok   $1"; else echo "FAIL $1: $LAST_TOKENS tokens"; fails=$((fails + 1)); fi
}

for r in "$REG" "$PROXY" ${QUAY:+"$QUAY"}; do
  where="realm on the token service's host"
  auth="$W/device.json"
  if [[ "$r" == "$PROXY" ]]; then where="realm on the registry's host, behind nginx"; auth="$W/device-proxy.json"; fi
  if [[ "$r" == "$QUAY" ]]; then where="Quay ($QUAY_IMAGE), a robot account"; auth="$W/device-quay.json"; fi
  echo "-- $r: $where"
  FC_FW_PLATFORM=chip-b pull "firmware of chip-b from the index, signed" "$auth" ok fw "$r/flightctl-mcu/fw:t1" "$W/out"
  same "  the image as pushed" "$W/out" "$W/fw-b.bin"
  expect_tokens "  one token for the pull" 1
  pull "WASM layer, signed" "$auth" ok layer "$r/flightctl-mcu/app:t1" wasm "$WASM" "$W/out"
  same "  the module as pushed" "$W/out" "$W/app.wasm"
  pull "anonymous token for a public repository" "$W/none.json" ok layer "$r/public/app:t1" wasm "$WASM" "$W/out"
  pull "no credentials, private repository" "$W/none.json" "HTTP 401 (no pull access without credentials)" \
    layer "$r/flightctl-mcu/app:t1" wasm "$WASM" "$W/out"
  pull "a repository the account may not read" "$auth" "HTTP 401 (no pull access with these credentials)" \
    layer "$r/other/app:t1" wasm "$WASM" "$W/out"
done
echo "-- $REG: what fails"
pull "wrong password" "$W/wrong.json" "HTTP 401 (credentials refused)" layer "$REG/flightctl-mcu/app:t1" wasm "$WASM" "$W/out"
if [[ -n "${QUAY:-}" ]]; then
  pull "Quay: a wrong robot token" "$W/wrong.json" "HTTP 401 (credentials refused)" \
    layer "$QUAY/flightctl-mcu/app:t1" wasm "$WASM" "$W/out"
fi
NO_AUTH_CA=1 pull "no CA for the token service's host" "$W/device.json" \
  "no CA for its token service (/etc/containers/certs.d/$AUTH_HOST/ca.crt)" layer "$REG/flightctl-mcu/app:t1" wasm "$WASM" "$W/out"
if [[ -z "${QUICK:-}" ]]; then
  pull "a token run out between the manifests and the blob" "$W/device.json" ok \
    expire "$REG/flightctl-mcu/fw:t1" chip-a "" 70 "$W/out"
  same "  the image as pushed" "$W/out" "$W/fw-a.bin"
  expect_tokens "  a second token for the blob" 2
fi
[[ $fails -eq 0 ]] && echo "registry-bearer: all passed" || echo "registry-bearer: $fails failed"
exit $(( fails > 0 ))
