#!/usr/bin/env bash
# Builds the firmware in the espressif/idf container (no local ESP-IDF needed).
# Runs as root: IDF 5.5 writes /opt/esp/root_managed_components; on macOS the
# bind-mounted build output still ends up owned by the host user.
# One build directory per chip: firmware/build-<target>.
# Chip-specific settings go in sdkconfig.defaults.<target> (loaded automatically).
# Usage: TARGET=esp32c3 FW_VERSION=v2 firmware/build.sh [idf.py args...]
#        (defaults: esp32s3, version "dev", build)
# VARIANT=<name> adds sdkconfig.<name> and builds in build-<target>-<name>
# (e.g. VARIANT=broken for the rollback demo). Several, joined by "-", add
# each in turn: VARIANT=secure-release, VARIANT=secure-broken.
# BUILD_DIR=<dir> overrides the build directory (relative to firmware/).
# SECURE_BOOT_KEY=<pem> is the Secure Boot v2 signing key for the secure
# variants (VARIANT=secure): bootloader and app images are signed with it.
# The firmware has no credentials in it: write them to the device with
# deploy/provision.py. sdkconfig.wifi and sdkconfig.local only give defaults
# for development (WiFi, endpoint, labels) that a provisioned device overrides.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v5.5.6}"
TARGET="${TARGET:-esp32s3}"
VARIANT="${VARIANT:-}"
BUILD_DIR="${BUILD_DIR:-build-$TARGET${VARIANT:+-$VARIANT}}"
VARIANTS=()
[[ -n "$VARIANT" ]] && IFS=- read -ra VARIANTS <<<"$VARIANT"
DEFAULTS="sdkconfig.defaults"
# Local, git-ignored settings: WiFi credentials and your server endpoint.
[[ -f "$ROOT/firmware/sdkconfig.wifi" ]] && DEFAULTS="$DEFAULTS;sdkconfig.wifi"
[[ -f "$ROOT/firmware/sdkconfig.local" ]] && DEFAULTS="$DEFAULTS;sdkconfig.local"
for v in ${VARIANTS[@]+"${VARIANTS[@]}"}; do
  [[ -f "$ROOT/firmware/sdkconfig.$v" ]] || { echo "no firmware/sdkconfig.$v" >&2; exit 1; }
  DEFAULTS="$DEFAULTS;sdkconfig.$v"
done
KEY_MOUNT=()
if [[ -n "${SECURE_BOOT_KEY:-}" ]]; then
  KEY_MOUNT=(-v "$(cd "$(dirname "$SECURE_BOOT_KEY")" && pwd)/$(basename "$SECURE_BOOT_KEY"):/keys/secure_boot_signing_key.pem:ro")
fi
docker run --rm \
  -e SDKCONFIG_DEFAULTS="$DEFAULTS" ${KEY_MOUNT[@]+"${KEY_MOUNT[@]}"} \
  -v "$ROOT:/project" -w /project/firmware "$IDF_IMAGE" \
  idf.py -B "$BUILD_DIR" -D IDF_TARGET="$TARGET" -D SDKCONFIG="$BUILD_DIR/sdkconfig" \
  -D FW_VERSION="${FW_VERSION:-dev}" "${@:-build}"
