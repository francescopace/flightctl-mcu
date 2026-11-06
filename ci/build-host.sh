#!/usr/bin/env bash
# CI only: builds the host agent on Linux inside the espressif/idf image,
# against the mbedTLS 3.6 sources shipped with ESP-IDF, and smoke-tests it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$IDF_PATH/export.sh" >/dev/null   # puts cmake and ninja on PATH
M="$IDF_PATH/components/mbedtls/mbedtls"
cmake -S "$M" -B /tmp/mbedtls -G Ninja -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/tmp/mbedtls-install
cmake --build /tmp/mbedtls --target install
cmake -S "$ROOT" -B "$ROOT/build/host" -G Ninja -DMbedTLS_DIR=/tmp/mbedtls-install/lib/cmake/MbedTLS
cmake --build "$ROOT/build/host"
# Without arguments the agent prints its usage and exits with 2.
set +e
"$ROOT/build/host/fc-host-agent"
test $? -eq 2
