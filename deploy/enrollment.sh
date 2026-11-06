#!/usr/bin/env bash
# Writes the enrollment credentials the firmware embeds at build time:
#   <out>/ca.crt  <out>/enroll.crt  <out>/enroll.key
# from `flightctl certificate request --signer=enrollment`. Needs a logged-in
# flightctl CLI. The output directory (default .secrets/) is git-ignored.
# Usage: deploy/enrollment.sh [out-dir]
set -euo pipefail
OUT="${1:-$(cd "$(dirname "$0")/.." && pwd)/.secrets}"
mkdir -p "$OUT" && chmod 700 "$OUT"
OUT="$(cd "$OUT" && pwd)"
# The CLI also drops ca.crt and client-enrollment.{crt,key} in the current
# directory: run it inside the output directory so nothing lands in the repo.
cd "$OUT"
flightctl certificate request --signer=enrollment --expiration=365d --output=embedded \
  > "$OUT/enrollment-config.yaml"
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
print("agent endpoint:", svc["service"]["server"], "(set CONFIG_FC_SERVER_HOST/PORT to match)")
PY
ls -1 "$OUT"
