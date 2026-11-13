#!/usr/bin/env bash
# Trustify for the lab's Flight Control on kind, and Flight Control's
# vulnerability reporting pointed at it (docs/SBOM.md).
#  - Trustify 0.4.12, the API Flight Control 1.4.0's client is generated from
#    (internal/trustify/v2/openapi.yaml): 0.5 and later moved it to /api/v3.
#    Its "PM mode" binary, with an embedded PostgreSQL, checked against the
#    sha256 GitHub gives for the release asset; in a container on kind's Docker
#    network, where Flight Control reaches it as http://fc-trustify:8080. On this
#    machine: http://127.0.0.1:8095 (TRUSTIFY_URL for tools/fw-push.py).
#  - No authentication: for a lab only.
#  - Flight Control asks it every minute (vulnerabilityReporting.syncInterval).
# Data in the Docker volume fc-trustify. Remove: docker rm -f fc-trustify.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${TRUSTIFY_VERSION:-0.4.12}"
FC_VERSION="${FC_VERSION:-1.4.0}"
case "$(uname -m)" in arm64|aarch64) ARCH=aarch64 ;; *) ARCH=x86_64 ;; esac
NAME="trustd-pm-$VERSION-$ARCH-unknown-linux-gnu"
DL="$ROOT/build/trustify-dl"
mkdir -p "$DL"
if [[ ! -x "$DL/$NAME/trustd-pm" ]]; then
  want="$(gh api "repos/guacsec/trustify/releases/tags/v$VERSION" --jq ".assets[] | select(.name==\"$NAME.tar.gz\") | .digest")"
  [[ "$want" == sha256:* ]] || { echo "no $NAME.tar.gz in Trustify v$VERSION" >&2; exit 1; }
  curl -fsSL -o "$DL/$NAME.tar.gz" "https://github.com/guacsec/trustify/releases/download/v$VERSION/$NAME.tar.gz"
  echo "${want#sha256:}  $DL/$NAME.tar.gz" | shasum -a 256 -c - >/dev/null
  tar xzf "$DL/$NAME.tar.gz" -C "$DL"
fi
# The embedded PostgreSQL needs these libraries and the time zones.
docker build -q -t fc-trustify-base:local - >/dev/null <<'DOCKERFILE'
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends libgssapi-krb5-2 libldap2 libxml2 libicu74 ca-certificates tzdata && rm -rf /var/lib/apt/lists/*
DOCKERFILE
docker volume create fc-trustify >/dev/null
# PostgreSQL does not run as root; an empty volume takes the image's owner.
docker run --rm -v fc-trustify:/data ubuntu:24.04 sh -c 'chown 1000:1000 /data && touch /data/.keep && chown 1000:1000 /data/.keep'
docker rm -f fc-trustify >/dev/null 2>&1 || true
docker run -d --name fc-trustify --restart unless-stopped --network kind --user 1000:1000 \
  -e HOME=/data -e TZ=UTC -e AUTH_DISABLED=true -e HTTP_SERVER_BIND_ADDR=0.0.0.0 -w /data \
  -v fc-trustify:/data -v "$DL/$NAME:/opt/trustify:ro" -p 127.0.0.1:8095:8080 \
  fc-trustify-base:local /opt/trustify/trustd-pm >/dev/null
for _ in $(seq 120); do
  [[ "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:8095/api/v2/sbom)" == 200 ]] && break
  sleep 2
done
helm upgrade flightctl "oci://quay.io/flightctl/charts/flightctl:${FC_VERSION}" -n flightctl --reuse-values \
  --set vulnerabilityReporting.enabled=true --set vulnerabilityReporting.syncInterval=1m \
  --set vulnerabilityReporting.trustify.endpoint=http://fc-trustify:8080 \
  --set vulnerabilityReporting.trustify.auth.mode=none >/dev/null
echo "Trustify $VERSION on http://127.0.0.1:8095 (TRUSTIFY_URL), Flight Control's vulnerability reporting on"
