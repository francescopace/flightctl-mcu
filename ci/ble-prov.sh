#!/usr/bin/env bash
# Runs ci/ble-prov.py (BLE field provisioning against the emulator) in the
# espressif/idf image ($IDF_IMAGE), installing bumble, protobuf and
# cryptography there. The emulator connects to port 9544 of this machine:
#   ci/ble-prov.sh --pop SECRET --ssid NAME --passphrase PASS [--wrong-pop OTHER]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.0.3}"
# esp_prov comes with the network_provisioning component (ci/esp-prov.sh).
# ESP_PROV_LOCAL=1 (CI): on this machine's python (bumble, protobuf and
# cryptography installed), with protocomm's messages from a sparse ESP-IDF
# checkout.
BASE="$("$ROOT/ci/esp-prov.sh" --checkout)"
if [[ "${ESP_PROV_LOCAL:-0}" == 1 ]]; then
  IDF_PATH="$BASE/esp-idf" ESP_PROV_DIR="$BASE/network_provisioning/tool/esp_prov" \
    exec python3 -u "$ROOT/ci/ble-prov.py" "$@"
fi
NET=(-p 9544:9544)
[[ "$(uname)" == Linux ]] && NET=(--network host)
docker run --rm "${NET[@]}" -v "$ROOT/ci:/ci:ro" -v "$BASE/network_provisioning:/np:ro" \
  -e ESP_PROV_DIR=/np/tool/esp_prov "$IDF_IMAGE" bash -c \
  'pip install -q bumble protobuf cryptography >/dev/null 2>&1; python -u /ci/ble-prov.py "$@"' ble-prov "$@"
