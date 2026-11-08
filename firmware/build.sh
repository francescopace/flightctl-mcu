#!/usr/bin/env bash
# Builds the firmware in the espressif/idf container (no local ESP-IDF needed).
# Runs as root: IDF 5.5 writes /opt/esp/root_managed_components; on macOS the
# bind-mounted build output still ends up owned by the host user.
# One build directory per chip: firmware/build-<target>.
# Chip-specific settings go in sdkconfig.defaults.<target> (loaded automatically).
# Usage: TARGET=esp32c3 FW_VERSION=v2 firmware/build.sh [idf.py args...]
#        (defaults: esp32s3, version "dev", build)
# VARIANT=<name> adds sdkconfig.<name> and builds in build-<target>-<name>
# (e.g. VARIANT=broken for the rollback demo).
# Tests: EXTRA_SDKCONFIG="<file>[;<file>]" adds defaults files (paths relative
# to firmware/, applied last); FC_SECRETS_DIR=<dir inside the repo> embeds the
# credentials found there instead of .secrets; BUILD_DIR=<dir> overrides the
# build directory (relative to firmware/).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.6}"
TARGET="${TARGET:-esp32s3}"
VARIANT="${VARIANT:-}"
BUILD_DIR="${BUILD_DIR:-build-$TARGET${VARIANT:+-$VARIANT}}"
DEFAULTS="sdkconfig.defaults"
# Local, git-ignored settings: WiFi credentials and your server endpoint.
[[ -f "$ROOT/firmware/sdkconfig.wifi" ]] && DEFAULTS="$DEFAULTS;sdkconfig.wifi"
[[ -f "$ROOT/firmware/sdkconfig.local" ]] && DEFAULTS="$DEFAULTS;sdkconfig.local"
if [[ "${1:-build}" == build && ! -f "$ROOT/firmware/sdkconfig.local" ]]; then
  echo "note: no firmware/sdkconfig.local, the firmware will not know its Flight Control endpoint" >&2
fi
[[ -n "$VARIANT" ]] && DEFAULTS="$DEFAULTS;sdkconfig.$VARIANT"
[[ -n "${EXTRA_SDKCONFIG:-}" ]] && DEFAULTS="$DEFAULTS;$EXTRA_SDKCONFIG"
SECRETS_ENV=()
[[ -n "${FC_SECRETS_DIR:-}" ]] && SECRETS_ENV=(-e "FC_SECRETS_DIR=/project/${FC_SECRETS_DIR#"$ROOT"/}")
docker run --rm \
  -e SDKCONFIG_DEFAULTS="$DEFAULTS" ${SECRETS_ENV[@]+"${SECRETS_ENV[@]}"} \
  -v "$ROOT:/project" -w /project/firmware "$IDF_IMAGE" \
  idf.py -B "$BUILD_DIR" -D IDF_TARGET="$TARGET" -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
  -D FW_VERSION="${FW_VERSION:-dev}" "${@:-build}"
