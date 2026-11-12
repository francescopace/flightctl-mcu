#!/usr/bin/env bash
# Builds the Zephyr application in the Zephyr build image (no local Zephyr
# tools needed), or with this machine's west when it has one.
# Usage: [BOARD=native_sim/native/64] zephyr/build.sh [west build args...] [-- CMake args...]
# The Zephyr tree comes from third_party/fetch-zephyr.sh; the build goes to
# zephyr/build-<board>. The default board is native_sim: the agent as a Linux
# process on Zephyr's kernel, network stack and file system.
# A board named .../mcuboot (rpi_pico/rp2040/w/mcuboot) or an Espressif one
# (esp32s3_devkitc/esp32s3/procpu) is built with MCUboot, for firmware updates:
# the bootloader in <build>/mcuboot, the application in <build>/zephyr, signed
# (zephyr.signed.bin) with .secrets/mcuboot-signing.pem.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.30.0}"
BOARD="${BOARD:-native_sim/native/64}"
BUILD_DIR="${BUILD_DIR:-build-${BOARD//\//_}}"
WS="$ROOT/third_party/zephyr-ws"
[[ -d "$WS/zephyr" ]] || { echo "no Zephyr tree: run third_party/fetch-zephyr.sh" >&2; exit 1; }
# The arguments of west build, then, after "--", those of CMake.
WEST_ARGS=(); CMAKE_ARGS=()
while (( $# )); do
  if [[ "$1" == -- ]]; then shift; CMAKE_ARGS=("$@"); break; fi
  WEST_ARGS+=("$1"); shift
done
NATIVE=""
command -v west >/dev/null && [[ -z "${ZEPHYR_DOCKER:-}" ]] && NATIVE=1
# Where the build sees this repository: here, or /project in the build image.
SEEN="$ROOT"; [[ -n "$NATIVE" ]] || SEEN=/project
if [[ "$BOARD" == */mcuboot || "$BOARD" == esp32* ]]; then
  # A board started by MCUboot (Espressif's always are, for updates): built
  # with it (sysbuild), and signed with this checkout's key, which is made the
  # first time.
  KEY=.secrets/mcuboot-signing.pem
  MAKE_KEY='[[ -f "$1" ]] || { mkdir -p "$(dirname "$1")"; python3 "$2/bootloader/mcuboot/scripts/imgtool.py" keygen -k "$1" -t ecdsa-p256; }'
  WEST_ARGS+=(--sysbuild)
  CMAKE_ARGS+=("-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE=\"$SEEN/$KEY\"")
else
  KEY=""; MAKE_KEY=":"
fi
if [[ -n "$NATIVE" ]]; then
  cd "$WS"
  # native boards build with the host's compiler
  if [[ "$BOARD" == native_sim* ]]; then
    export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-host}"
  else
    export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$ROOT/third_party/zephyr-sdk}"
  fi
  bash -c "$MAKE_KEY" make-key "$ROOT/$KEY" "$WS"
  exec west build -b "$BOARD" -d "$ROOT/zephyr/$BUILD_DIR" ${WEST_ARGS[@]+"${WEST_ARGS[@]}"} "$ROOT/zephyr" \
    ${CMAKE_ARGS[@]+-- "${CMAKE_ARGS[@]}"}
fi
# Real boards build with the Zephyr SDK (third_party/fetch-zephyr-sdk.sh).
TOOLCHAIN=(-e ZEPHYR_SDK_INSTALL_DIR=/project/third_party/zephyr-sdk)
if [[ "$BOARD" == native_sim* ]]; then
  TOOLCHAIN=(-e ZEPHYR_TOOLCHAIN_VARIANT=host)
elif [[ ! -d "$ROOT/third_party/zephyr-sdk" ]]; then
  echo "no Zephyr SDK: run third_party/fetch-zephyr-sdk.sh" >&2
  exit 1
fi
docker run --rm ${TOOLCHAIN[@]+"${TOOLCHAIN[@]}"} -v "$ROOT:/project" -w /project/third_party/zephyr-ws "$ZEPHYR_IMAGE" \
  bash -c 'git config --global --add safe.directory "*"
    bash -c "$1" make-key "/project/$2" /project/third_party/zephyr-ws
    shift 2
    exec west build "$@"' \
  build "$MAKE_KEY" "$KEY" -b "$BOARD" -d "/project/zephyr/$BUILD_DIR" ${WEST_ARGS[@]+"${WEST_ARGS[@]}"} /project/zephyr \
  ${CMAKE_ARGS[@]+-- "${CMAKE_ARGS[@]}"}
