#!/usr/bin/env bash
# Runs ESP-IDF's esp_prov (tools/esp_prov) against a device's provisioning
# endpoint, as an installer's phone would: Security 1 with a proof of
# possession, then the WiFi network.
#   ci/esp-prov.sh HOST:PORT --pop SECRET --ssid NAME --passphrase PASS
# On a workstation it runs in the espressif/idf image ($IDF_IMAGE), installing
# protobuf there (HOST is then as the container sees it: host.docker.internal
# for this machine on Docker Desktop). With ESP_PROV_LOCAL=1 (CI) it runs on
# this machine's python, from a sparse checkout of ESP-IDF in build/esp-idf;
# protobuf and cryptography must be installed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.6}"
SERVICE="${1:?HOST:PORT}"; shift
ARGS=(--transport softap --service_name "$SERVICE" --sec_ver 1 "$@")
if [[ "${ESP_PROV_LOCAL:-0}" == 1 ]]; then
  IDF="$ROOT/build/esp-idf"
  if [[ ! -d "$IDF/tools/esp_prov" ]]; then
    rm -rf "$IDF"
    git clone -q --depth 1 --branch "${IDF_IMAGE##*:}" --filter=blob:none --sparse \
      https://github.com/espressif/esp-idf.git "$IDF"
    git -C "$IDF" sparse-checkout set tools/esp_prov components/protocomm/python components/wifi_provisioning/python
  fi
  IDF_PATH="$IDF" python3 "$IDF/tools/esp_prov/esp_prov.py" "${ARGS[@]}"
else
  NET=()
  [[ "$(uname)" == Linux ]] && NET=(--network host)
  docker run --rm ${NET[@]+"${NET[@]}"} "$IDF_IMAGE" bash -c \
    'pip install -q protobuf >/dev/null 2>&1; python "$IDF_PATH/tools/esp_prov/esp_prov.py" "$@"' esp_prov "${ARGS[@]}"
fi
