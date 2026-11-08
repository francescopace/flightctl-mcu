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
# BUILD_DIR=<dir> overrides the build directory (relative to firmware/).
# The firmware has no credentials in it: write them to the device with
# deploy/provision.py. sdkconfig.wifi and sdkconfig.local only give defaults
# for development (WiFi, endpoint, labels) that a provisioned device overrides.
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
[[ -n "$VARIANT" ]] && DEFAULTS="$DEFAULTS;sdkconfig.$VARIANT"
docker run --rm \
  -e SDKCONFIG_DEFAULTS="$DEFAULTS" \
  -v "$ROOT:/project" -w /project/firmware "$IDF_IMAGE" \
  idf.py -B "$BUILD_DIR" -D IDF_TARGET="$TARGET" -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
  -D FW_VERSION="${FW_VERSION:-dev}" "${@:-build}"
