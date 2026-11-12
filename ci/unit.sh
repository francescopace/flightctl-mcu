#!/usr/bin/env bash
# Unit tests: the C tests (tests/unit, with AddressSanitizer and UBSan) and the
# Python tests of the deploy scripts (tests/python). Needs cmake, ninja, a C
# compiler with sanitizers, mbedTLS 4 (MbedTLS_DIR) and python3 with PyYAML.
# Usage: ci/unit.sh [cmake args...]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
cmake -S . -B build/unit -G Ninja -DFC_UNIT=ON -DFC_SANITIZE=ON "$@" >/dev/null
cmake --build build/unit
ctest --test-dir build/unit --output-on-failure
python3 -m unittest discover -s tests/python -v 2>&1 | tail -n +1
