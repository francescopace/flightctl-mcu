#!/usr/bin/env bash
# Writes the enrollment credentials that tools/provision.py puts on devices:
#   <out>/ca.crt  <out>/enroll.crt  <out>/enroll.key
# from `flightctl certificate request --signer=enrollment`. Needs a logged-in
# flightctl CLI. The output directory (default .secrets/) is git-ignored.
# Usage: tools/enrollment.sh [out-dir]
#
# One certificate per production batch limits what a leaked one opens:
#   BATCH=2026-11-a EXPIRATION=30d tools/enrollment.sh .secrets/batch-2026-11-a
# BATCH names the certificate (CN enroll-<batch>, also the name of its
# CertificateSigningRequest in Flight Control); EXPIRATION is its lifetime
# (default 365d). Flight Control 1.3.1 cannot revoke an enrollment
# certificate: it checks only the signer and the expiry. Keep the lifetime to
# the batch's factory window plus the longest a device may stay offline
# before enrolling (after which a device whose management certificate expired
# can no longer enroll again), and approve devices from an allow list
# (tools/auto-approve), not by the certificate alone.
set -euo pipefail
OUT="${1:-$(cd "$(dirname "$0")/.." && pwd)/.secrets}"
mkdir -p "$OUT" && chmod 700 "$OUT"
OUT="$(cd "$OUT" && pwd)"
# The CLI also drops ca.crt and client-enrollment.{crt,key} in the current
# directory: run it inside the output directory so nothing lands in the repo.
cd "$OUT"
NAME_ARGS=()
if [[ -n "${BATCH:-}" ]]; then
  [[ "$BATCH" =~ ^[a-z0-9]([a-z0-9-]*[a-z0-9])?$ ]] || { echo "BATCH: lowercase letters, digits and -" >&2; exit 1; }
  NAME_ARGS=(--name "enroll-$BATCH")
fi
flightctl certificate request --signer=enrollment --expiration="${EXPIRATION:-365d}" \
  ${NAME_ARGS[@]+"${NAME_ARGS[@]}"} --output=embedded > "$OUT/enrollment-config.yaml"
python3 - "$OUT" <<'PY'
import base64, os, sys, yaml
out = sys.argv[1]
svc = yaml.safe_load(open(f"{out}/enrollment-config.yaml"))["enrollment-service"]
files = {
    "ca.crt": svc["service"]["certificate-authority-data"],
    "enroll.crt": svc["authentication"]["client-certificate-data"],
    "enroll.key": svc["authentication"]["client-key-data"],
}
for name, data in files.items():
    path = os.path.join(out, name)
    with open(path, "wb") as f:
        f.write(base64.b64decode(data))
    os.chmod(path, 0o600)
print("agent endpoint:", svc["service"]["server"])
PY
openssl x509 -in "$OUT/enroll.crt" -noout -subject -enddate
ls -1 "$OUT"
