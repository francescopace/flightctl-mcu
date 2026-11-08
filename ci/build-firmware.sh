#!/usr/bin/env bash
# CI only: builds the firmware for one chip inside the espressif/idf image
# (firmware/build.sh does the same through docker on a workstation).
# Usage: ci/build-firmware.sh <target> [version] [variant, e.g. release]
set -euo pipefail
TARGET="${1:?target}"; VERSION="${2:-ci}"; VARIANT="${3:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$IDF_PATH/export.sh" >/dev/null
cd "$ROOT/firmware"
BUILD="build-$TARGET${VARIANT:+-$VARIANT}"
SDKCONFIG_DEFAULTS="sdkconfig.defaults${VARIANT:+;sdkconfig.$VARIANT}" idf.py -B "$BUILD" -D IDF_TARGET="$TARGET" \
  -D SDKCONFIG="$BUILD/sdkconfig" -D FW_VERSION="$VERSION" build
ls -l "$BUILD/flightctl_mcu_fw.bin"
