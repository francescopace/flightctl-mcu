#!/usr/bin/env bash
# Crea un cluster kind con Flight Control raggiungibile dalla LAN.
# Uso: deploy/kind/up.sh [IP_HOST]   (default: IP di en0)
set -euo pipefail
cd "$(dirname "$0")"

HOST_IP="${1:-$(ipconfig getifaddr en0)}"
FC_VERSION="${FC_VERSION:-1.3.1}"
FC_NAMESPACE=flightctl
FC_BASE_DOMAIN="${HOST_IP}.nip.io"

kind get clusters | grep -qx flightctl || kind create cluster --name flightctl --config kind-flightctl.yaml
kubectl config use-context kind-flightctl

# Su arm64 (es. Mac Apple Silicon) origin-cli non esiste: usiamo un'immagine locale.
CLUSTER_CLI_ARGS=()
if [[ "$(uname -m)" == "arm64" || "$(uname -m)" == "aarch64" ]]; then
  docker build -q -f cluster-cli/Containerfile -t localhost/flightctl-cluster-cli:local cluster-cli
  kind load docker-image localhost/flightctl-cluster-cli:local --name flightctl
  CLUSTER_CLI_ARGS=(--set clusterCli.image.image=localhost/flightctl-cluster-cli
                    --set clusterCli.image.tag=local
                    --set clusterCli.image.pullPolicy=IfNotPresent)
fi

# Le CRD Gateway API arrivano dal chart di Envoy Gateway: applicarle anche con kubectl
# crea conflitti di server-side apply con Helm 4.
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
  "${CLUSTER_CLI_ARGS[@]}"

echo "Base domain: ${FC_BASE_DOMAIN}"
echo "UI:          https://ui.${FC_BASE_DOMAIN}:8443"
echo "API:         https://api.${FC_BASE_DOMAIN}:8443"
echo "Agent API:   https://agent-api.${FC_BASE_DOMAIN}:8443"
