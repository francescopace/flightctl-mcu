#!/usr/bin/env bash
# CI: builds the host agent (simulated device) on Linux against an installed
# mbedTLS 3.6 (MBEDTLS_DIR, the CMake package directory), and smoke-tests it.
# The binary links mbedTLS statically, so the e2e job can run it as it is.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MBEDTLS_DIR="${MBEDTLS_DIR:-/tmp/mbedtls-install/lib/cmake/MbedTLS}"
cmake -S "$ROOT" -B "$ROOT/build/host" -G Ninja -DMbedTLS_DIR="$MBEDTLS_DIR"
cmake --build "$ROOT/build/host"
# Without arguments the agent prints its usage and exits with 2.
set +e
"$ROOT/build/host/fc-host-agent"
test $? -eq 2
