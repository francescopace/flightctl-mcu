#!/usr/bin/env bash
# Creates a kind cluster running Flight Control, reachable from the LAN.
# Usage: deploy/kind/up.sh [HOST_IP]   (default: this machine's LAN IP)
set -euo pipefail
cd "$(dirname "$0")"
source ../local-ip.sh

HOST_IP="${1:-$(local_ip)}"
FC_VERSION="${FC_VERSION:-1.4.0}"
FC_NAMESPACE=flightctl
FC_BASE_DOMAIN="${HOST_IP}.nip.io"

kind get clusters | grep -qx flightctl || kind create cluster --name flightctl --config kind-flightctl.yaml
kubectl config use-context kind-flightctl

# origin-cli has no arm64 image (e.g. Apple Silicon): build a local replacement.
CLUSTER_CLI_ARGS=()
if [[ "$(uname -m)" == "arm64" || "$(uname -m)" == "aarch64" ]]; then
  docker build -q -f cluster-cli/Containerfile -t localhost/flightctl-cluster-cli:local cluster-cli
  kind load docker-image localhost/flightctl-cluster-cli:local --name flightctl
  CLUSTER_CLI_ARGS=(--set clusterCli.image.image=localhost/flightctl-cluster-cli
                    --set clusterCli.image.tag=local
                    --set clusterCli.image.pullPolicy=IfNotPresent)
fi

# The Gateway API CRDs come with the Envoy Gateway chart: applying them with
# kubectl as well causes server-side apply conflicts with Helm 4.
helm upgrade --install eg oci://docker.io/envoyproxy/gateway-helm --version v1.2.5 \
  -n envoy-gateway-system --create-namespace
kubectl wait --timeout=5m -n envoy-gateway-system deployment/envoy-gateway --for=condition=Available
kubectl apply -f gateway.yaml

helm upgrade --install flightctl "oci://quay.io/flightctl/charts/flightctl:${FC_VERSION}" \
  --namespace "${FC_NAMESPACE}" --create-namespace \
  --set global.baseDomain="${FC_BASE_DOMAIN}" \
  --set global.gateway.gatewayClassName=envoy-gateway \
  --set global.gateway.ports.http=80 \
  --set global.gateway.ports.tls=443 \
  ${CLUSTER_CLI_ARGS[@]+"${CLUSTER_CLI_ARGS[@]}"}

echo "Base domain: ${FC_BASE_DOMAIN}"
echo "UI:          http://ui.${FC_BASE_DOMAIN}:8080"
echo "API:         https://api.${FC_BASE_DOMAIN}:8443"
echo "Agent API:   https://agent-api.${FC_BASE_DOMAIN}:8443"
