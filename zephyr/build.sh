#!/usr/bin/env bash
# Builds the Zephyr application in the Zephyr build image (no local Zephyr
# tools needed), or with this machine's west when it has one.
# Usage: [BOARD=native_sim/native/64] zephyr/build.sh [west build args...]
# The Zephyr tree comes from third_party/fetch-zephyr.sh; the build goes to
# zephyr/build-<board>. The default board is native_sim: the agent as a Linux
# process on Zephyr's kernel, network stack and file system.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.29.1}"
BOARD="${BOARD:-native_sim/native/64}"
BUILD_DIR="${BUILD_DIR:-build-${BOARD//\//_}}"
WS="$ROOT/third_party/zephyr-ws"
[[ -d "$WS/zephyr" ]] || { echo "no Zephyr tree: run third_party/fetch-zephyr.sh" >&2; exit 1; }
if command -v west >/dev/null && [[ -z "${ZEPHYR_DOCKER:-}" ]]; then
  cd "$WS"
  # native boards build with the host's compiler
  [[ "$BOARD" == native_sim* ]] && export ZEPHYR_TOOLCHAIN_VARIANT="${ZEPHYR_TOOLCHAIN_VARIANT:-host}"
  exec west build -b "$BOARD" -d "$ROOT/zephyr/$BUILD_DIR" "$ROOT/zephyr" "$@"
fi
TOOLCHAIN=()
[[ "$BOARD" == native_sim* ]] && TOOLCHAIN=(-e ZEPHYR_TOOLCHAIN_VARIANT=host)
docker run --rm ${TOOLCHAIN[@]+"${TOOLCHAIN[@]}"} -v "$ROOT:/project" -w /project/third_party/zephyr-ws "$ZEPHYR_IMAGE" \
  bash -c 'git config --global --add safe.directory "*"; west build -b "$0" -d "/project/zephyr/$1" /project/zephyr "${@:2}"' \
  "$BOARD" "$BUILD_DIR" "$@"
