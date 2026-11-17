#!/usr/bin/env bash
# CI only: builds the firmware for one chip inside the espressif/idf image
# (port/espidf/build.sh does the same through docker on a workstation), with a
# merged flash image. The firmware has no credentials in it.
# Usage: port/espidf/build-ci.sh <target> [version] [variant: release, broken, secure, secure-release, ...]
#   version gets "-<variant>" appended; output in port/espidf/build-<target>[-<variant>]
# Secure variants are built unsigned (as SECURE_BOOT_SIGNER=external with
# build.sh): bootloader and app padded for a signature, which whoever flashes
# them adds with their own Secure Boot key (docs/SECURITY.md, "Signing a CI
# build"). The end-to-end test signs them with throwaway keys.
set -euo pipefail
TARGET="${1:?target}"; VERSION="${2:-ci}"; VARIANT="${3:-}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$IDF_PATH/export.sh" >/dev/null
# ccache (in the ESP-IDF image): CI keeps .ccache between runs. The compiler
# is checked by content: ESP-IDF 6 copies picolibc.specs into the build
# directory and passes it with -specs=, and by mtime (ccache's default) a new
# build directory, as in every CI run, made every file a miss.
export IDF_CCACHE_ENABLE=1 CCACHE_DIR="$ROOT/.ccache" CCACHE_MAXSIZE=500M CCACHE_COMPILERCHECK=content
cd "$ROOT/port/espidf"
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
if [[ "$VARIANT" == secure* ]]; then
  mkdir -p "$BUILD"
  echo "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES=n" > "$BUILD/sdkconfig.external_signer"
  DEFAULTS="$DEFAULTS;$BUILD/sdkconfig.external_signer"
fi
# The settings come only from the sdkconfig.* files, as in build.sh: ESP-IDF
# keeps an existing sdkconfig's values over new defaults.
rm -f "$BUILD/sdkconfig"
ccache -z >/dev/null   # the stats at the end are this build's
# A merged image of a secure build needs its signed bootloader: made once
# signed (merge-secure.py), not here.
STEPS=(build merge-bin)
[[ "$VARIANT" == secure* ]] && STEPS=(build)
SDKCONFIG_DEFAULTS="$DEFAULTS" idf.py -B "$BUILD" -D IDF_TARGET="$TARGET" \
  -D SDKCONFIG="$BUILD/sdkconfig" -D FW_VERSION="$VERSION${VARIANT:+-$VARIANT}" "${STEPS[@]}"
ls -l "$BUILD/flightctl_mcu_fw.bin" "$BUILD/bootloader/bootloader.bin"
ccache --show-stats | grep -iE 'hits|misses' | head -3 || true
