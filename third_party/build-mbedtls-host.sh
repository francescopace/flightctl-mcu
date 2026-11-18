#!/usr/bin/env bash
# Install the host build of the device SDKs' mbedTLS, with pthreads for PSA.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${1:-$ROOT/build/mbedtls-install}"
VERSION=4.1.1
SHA256=3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ARCHIVE="mbedtls-$VERSION.tar.bz2"
url="https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VERSION/$ARCHIVE"
python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$url" "$WORK/$ARCHIVE"
digest="$(cmake -E sha256sum "$WORK/$ARCHIVE")"
[[ "${digest%% *}" == "$SHA256" ]] || { echo "SHA-256 mismatch for $ARCHIVE" >&2; exit 1; }
tar -xjf "$WORK/$ARCHIVE" -C "$WORK"
src="$WORK/mbedtls-$VERSION"
for option in MBEDTLS_THREADING_C MBEDTLS_THREADING_PTHREAD; do
    python3 "$src/tf-psa-crypto/scripts/config.py" -f "$src/tf-psa-crypto/include/psa/crypto_config.h" set "$option"
done
cmake -S "$src" -B "$WORK/build" -G Ninja -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib
cmake --build "$WORK/build" --target install
