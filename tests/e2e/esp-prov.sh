#!/usr/bin/env bash
# Runs Espressif's esp_prov against a device's provisioning endpoint, as an
# installer's phone would: Security 1 with a proof of possession, then the
# WiFi network.
#   tests/e2e/esp-prov.sh HOST:PORT --pop SECRET --ssid NAME --passphrase PASS
# esp_prov left ESP-IDF with v6.0: it comes with the network_provisioning
# component, the one the firmware builds with (port/espidf/main/idf_component.yml),
# fetched here from Espressif's registry and checked against its sha256. It
# still takes protocomm's messages from ESP-IDF.
# On a workstation it runs in the espressif/idf image ($IDF_IMAGE), installing
# protobuf there (HOST is then as the container sees it: host.docker.internal
# for this machine on Docker Desktop). With ESP_PROV_LOCAL=1 (CI) it runs on
# this machine's python, with those messages from a sparse checkout of
# ESP-IDF; protobuf and cryptography must be installed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.0.3}"
PROV_VERSION=1.4.0
PROV_SHA256=6e81719e22a1485c3d617e8512a1eff356c333ab3ea0eae3222b2a88d9fd12ad
BASE="$ROOT/build/esp-prov"
checkout() { # network_provisioning in $BASE/network_provisioning and, for a local run, ESP-IDF's protocomm in $BASE/esp-idf; prints $BASE
  local np="$BASE/network_provisioning" idf="$BASE/esp-idf" zip="$BASE/network_provisioning.zip"
  if [[ "$(cat "$np/.version" 2>/dev/null)" != "$PROV_VERSION" ]]; then
    rm -rf "$np" && mkdir -p "$np"
    url="https://components-file.espressif.com/components/espressif/network_provisioning/$PROV_VERSION/espressif__network_provisioning-v$PROV_VERSION.zip"
    if command -v curl >/dev/null; then
      curl -fsSL -o "$zip" "$url"
    else # Zephyr's build image, where the host job runs, has no curl
      python3 -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' "$url" "$zip"
    fi
    echo "$PROV_SHA256  $zip" | shasum -a 256 -c - >/dev/null
    unzip -q "$zip" -d "$np" && rm -f "$zip"
    echo "$PROV_VERSION" > "$np/.version"
  fi
  if [[ "${ESP_PROV_LOCAL:-0}" == 1 && ! -d "$idf/components/protocomm/python" ]]; then
    rm -rf "$idf"
    git clone -q --depth 1 --branch "${IDF_IMAGE##*:}" --filter=blob:none --sparse \
      https://github.com/espressif/esp-idf.git "$idf"
    git -C "$idf" sparse-checkout set components/protocomm/python
  fi
  echo "$BASE"
}
if [[ "${1:-}" == --checkout ]]; then checkout; exit; fi
SERVICE="${1:?HOST:PORT}"; shift
ARGS=(--transport softap --service_name "$SERVICE" --sec_ver 1 "$@")
checkout >/dev/null
if [[ "${ESP_PROV_LOCAL:-0}" == 1 ]]; then
  IDF_PATH="$BASE/esp-idf" python3 "$BASE/network_provisioning/tool/esp_prov/esp_prov.py" "${ARGS[@]}"
else
  NET=()
  [[ "$(uname)" == Linux ]] && NET=(--network host)
  docker run --rm ${NET[@]+"${NET[@]}"} -v "$BASE/network_provisioning:/np:ro" "$IDF_IMAGE" bash -c \
    'pip install -q protobuf >/dev/null 2>&1; python /np/tool/esp_prov/esp_prov.py "$@"' esp_prov "${ARGS[@]}"
fi
