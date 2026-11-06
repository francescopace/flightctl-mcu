#!/usr/bin/env bash
# CI only: builds the firmware for one chip inside the espressif/idf image
# (firmware/build.sh does the same through docker on a workstation).
# Usage: ci/build-firmware.sh <target> [version]
set -euo pipefail
TARGET="${1:?target}"; VERSION="${2:-ci}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$IDF_PATH/export.sh" >/dev/null
cd "$ROOT/firmware"
SDKCONFIG_DEFAULTS="sdkconfig.defaults" idf.py -B "build-$TARGET" -D IDF_TARGET="$TARGET" \
  -D SDKCONFIG="build-$TARGET/sdkconfig" -D FW_VERSION="$VERSION" build
ls -l "build-$TARGET/flightctl_mcu_fw.bin"
