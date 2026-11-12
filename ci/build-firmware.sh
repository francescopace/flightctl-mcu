#!/usr/bin/env bash
# CI only: builds the firmware for one chip inside the espressif/idf image
# (firmware/build.sh does the same through docker on a workstation), with a
# merged flash image. The firmware has no credentials in it.
# Usage: ci/build-firmware.sh <target> [version] [variant: release, broken, secure, secure-release, ...]
#   version gets "-<variant>" appended; output in firmware/build-<target>[-<variant>]
# Secure variants are signed with /keys/secure_boot_signing_key.pem; CI makes a
# throwaway key there (the emulated chips burn whatever key the image has).
set -euo pipefail
TARGET="${1:?target}"; VERSION="${2:-ci}"; VARIANT="${3:-}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$IDF_PATH/export.sh" >/dev/null
# ccache (in the ESP-IDF image): CI keeps .ccache between runs.
export IDF_CCACHE_ENABLE=1 CCACHE_DIR="$ROOT/.ccache" CCACHE_MAXSIZE=500M
cd "$ROOT/firmware"
BUILD="build-$TARGET${VARIANT:+-$VARIANT}"
DEFAULTS="sdkconfig.defaults"
IFS=- read -ra VARIANTS <<<"$VARIANT"
for v in ${VARIANTS[@]+"${VARIANTS[@]}"}; do DEFAULTS="$DEFAULTS;sdkconfig.$v"; done
# SECURE_VERSION: the release's security version (anti-rollback), see build.sh
if [[ -n "${SECURE_VERSION:-}" ]]; then
  mkdir -p "$BUILD"
  echo "CONFIG_BOOTLOADER_APP_SECURE_VERSION=$SECURE_VERSION" > "$BUILD/sdkconfig.secure_version"
  DEFAULTS="$DEFAULTS;$BUILD/sdkconfig.secure_version"
fi
if [[ "$VARIANT" == secure* && ! -f /keys/secure_boot_signing_key.pem ]]; then
  mkdir -p /keys
  espsecure generate-signing-key --version 2 --scheme rsa3072 /keys/secure_boot_signing_key.pem >/dev/null
fi
SDKCONFIG_DEFAULTS="$DEFAULTS" idf.py -B "$BUILD" -D IDF_TARGET="$TARGET" \
  -D SDKCONFIG="$BUILD/sdkconfig" -D FW_VERSION="$VERSION${VARIANT:+-$VARIANT}" build merge-bin
# Secure Boot leaves the bootloader out of merge-bin: put it in.
[[ "$VARIANT" == secure* ]] && python merge-secure.py "$BUILD"
ls -l "$BUILD/flightctl_mcu_fw.bin" "$BUILD/merged-binary.bin"
ccache --show-stats | grep -iE 'hits|misses' | head -3 || true
