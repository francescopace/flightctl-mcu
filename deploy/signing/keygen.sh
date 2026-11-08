#!/usr/bin/env bash
# Creates the key pair that signs firmware and WASM apps (ECDSA P-256, the
# same kind cosign uses). The private key stays on the machine that pushes
# images; the public key is built into the firmware and given to the host
# agent with --signing-key.
# Usage: deploy/signing/keygen.sh [DIR]   (default: .secrets)
set -euo pipefail
OUT="${1:-$(cd "$(dirname "$0")/../.." && pwd)/.secrets}"
mkdir -p "$OUT"
if [[ -f "$OUT/signing.key" ]]; then
  echo "$OUT/signing.key already exists" >&2
  exit 1
fi
umask 077
openssl ecparam -name prime256v1 -genkey -noout | openssl pkcs8 -topk8 -nocrypt -out "$OUT/signing.key"
openssl ec -in "$OUT/signing.key" -pubout -out "$OUT/signing.pub" 2>/dev/null
echo "$OUT/signing.key (keep it private), $OUT/signing.pub (goes into the firmware)"
