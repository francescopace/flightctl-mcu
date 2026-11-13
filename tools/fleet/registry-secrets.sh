#!/usr/bin/env bash
# Creates (or updates) the Kubernetes secrets that fleets exported with
# tools/fleet/git-export.py mount: the registry's CA (fc-registry-ca, key
# ca.crt) and its credentials (fc-registry-auth, key auth.json), from
# .secrets/registry (tools/registry/up.sh) or $REGISTRY_CA / $REGISTRY_AUTH_FILE.
# Flight Control's worker reads them when it renders a device's spec. Its
# chart does not let it read secrets (worker.clusterLevelSecretAccess: false):
# a Role here lets it read these two, and nothing else.
# Usage: [FLEET_SECRETS_NAMESPACE=flightctl] tools/fleet/registry-secrets.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NS="${FLEET_SECRETS_NAMESPACE:-flightctl}"
SECRETS="${REGISTRY_SECRETS:-$ROOT/.secrets/registry}"
CA="${REGISTRY_CA:-$SECRETS/ca.crt}"
AUTH="${REGISTRY_AUTH_FILE:-$SECRETS/auth.json}"
[[ -f "$CA" ]] || { echo "no registry CA at $CA (tools/registry/up.sh)" >&2; exit 1; }
kubectl -n "$NS" create secret generic fc-registry-ca --from-file=ca.crt="$CA" \
  --dry-run=client -o yaml | kubectl apply -f -
if [[ -f "$AUTH" ]]; then
  kubectl -n "$NS" create secret generic fc-registry-auth --from-file=auth.json="$AUTH" \
    --dry-run=client -o yaml | kubectl apply -f -
fi
kubectl -n "$NS" apply -f - <<EOF
apiVersion: rbac.authorization.k8s.io/v1
kind: Role
metadata:
  name: flightctl-worker-registry-secrets
rules:
  - apiGroups: [""]
    resources: ["secrets"]
    resourceNames: ["fc-registry-ca", "fc-registry-auth"]
    verbs: ["get"]
---
apiVersion: rbac.authorization.k8s.io/v1
kind: RoleBinding
metadata:
  name: flightctl-worker-registry-secrets
roleRef:
  apiGroup: rbac.authorization.k8s.io
  kind: Role
  name: flightctl-worker-registry-secrets
subjects:
  - kind: ServiceAccount
    name: flightctl-worker
    namespace: ${WORKER_NAMESPACE:-flightctl}
EOF
