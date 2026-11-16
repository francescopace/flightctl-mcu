#!/usr/bin/env bash
# The Zephyr port's hardware for apps on native_sim's emulated drivers
# (tests/zephyr-hw): builds the ztest suite and runs it, in the Zephyr build
# image unless this machine has west. Exit status: the suite's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
APP=tests/zephyr-hw BOARD=native_sim/native/64 port/zephyr/build.sh
EXE=tests/zephyr-hw/build-native_sim_native_64/zephyr/zephyr.exe
if command -v west >/dev/null && [[ -z "${ZEPHYR_DOCKER:-}" ]]; then
  "$EXE"
else
  docker run --rm -v "$ROOT:/project" -w /project "${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.30.0}" "./$EXE"
fi
