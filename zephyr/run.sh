#!/usr/bin/env bash
# Runs the native_sim build of the Zephyr application: in the Zephyr build
# image, or directly on Linux.
# Usage: zephyr/run.sh FLASH.bin [zephyr.exe args...]
# FLASH.bin is the device's flash: its credentials (deploy/provision.py
# --format zephyr --image FLASH.bin) and what it stores. --seed-random is
# always passed: without it native_sim's random generator starts from a fixed
# seed, and every instance would make the same device key.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.29.1}"
BUILD_DIR="${BUILD_DIR:-build-native_sim_native_64}"
FLASH="${1:?usage: zephyr/run.sh FLASH.bin [args...]}"
shift
FLASH="$(cd "$(dirname "$FLASH")" && pwd)/$(basename "$FLASH")"
EXE="$ROOT/zephyr/$BUILD_DIR/zephyr/zephyr.exe"
[[ -x "$EXE" ]] || { echo "no $EXE: run zephyr/build.sh" >&2; exit 1; }
if [[ "$(uname)" == Linux && -z "${ZEPHYR_DOCKER:-}" ]]; then
  exec "$EXE" --flash="$FLASH" --seed-random "$@"
fi
case "$FLASH" in "$ROOT"/*) ;; *) echo "the flash file must be inside $ROOT (it is shared with the container)" >&2; exit 1 ;; esac
exec docker run --rm ${DOCKER_ARGS:-} -v "$ROOT:/project" -w /project "$ZEPHYR_IMAGE" \
  "/project/zephyr/$BUILD_DIR/zephyr/zephyr.exe" --flash="/project/${FLASH#"$ROOT"/}" --seed-random "$@"
