#!/usr/bin/env bash
# CI only: throwaway enrollment credentials so the firmware can be built
# (they are embedded at build time). They are useless against a real server.
set -euo pipefail
OUT="${1:-.secrets}"
mkdir -p "$OUT" && cd "$OUT"
openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -subj "/CN=ci-ca" -days 1 -out ca.crt
openssl ecparam -name prime256v1 -genkey -noout -out enroll.key
openssl req -new -key enroll.key -subj "/CN=ci-enrollment" -out enroll.csr
openssl x509 -req -in enroll.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 1 -out enroll.crt
rm -f ca.key enroll.csr ca.srl
