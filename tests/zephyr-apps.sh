#!/usr/bin/env bash
# The apps' C library on Zephyr (tests/zephyr-apps): builds the test for
# BOARD (native_sim/native/64 by default, or mps2/an385, run in QEMU) and
# runs it, in the Zephyr build image unless this machine has west. It prints
# each app's state, its log and the stack its thread used ("STACK"), then
# PASS or FAIL. Exit status: 0 on PASS.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BOARD="${BOARD:-native_sim/native/64}"
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.30.0}"
BUILD="tests/zephyr-apps/build-${BOARD//\//_}"
APP=tests/zephyr-apps BOARD="$BOARD" port/zephyr/build.sh "$@"
OUT="$BUILD/run.log"
LOCAL=""
command -v west >/dev/null && [[ -z "${ZEPHYR_DOCKER:-}" ]] && LOCAL=1
if [[ "$BOARD" == native_sim* ]]; then
  # A fresh flash file each run, in the build directory.
  rm -f "$BUILD/flash.bin"
  CMD=("./$BUILD/zephyr/zephyr.exe" --flash="$BUILD/flash.bin" --seed-random)
else
  # QEMU does not end with the test (nor when its output is no longer read):
  # stopped once the log has PASS or FAIL, or after 5 minutes. QEMU from the
  # Zephyr SDK's host tools, or this machine's.
  RUN='Q=$(ls third_party/zephyr-sdk/hosttools/sysroots/*/usr/bin/qemu-system-arm 2>/dev/null | head -1)
    "${Q:-qemu-system-arm}" -cpu cortex-m3 -machine mps2-an385 -nographic -vga none -kernel "$1/zephyr/zephyr.elf" -nic none > "$1/qemu.out" 2>&1 &
    P=$!; i=0
    while [ $i -lt 300 ] && ! tr -d "\r" < "$1/qemu.out" | grep -qE "^(PASS|FAIL [0-9]+)$"; do sleep 1; i=$((i + 1)); done
    kill $P; tr -d "\r" < "$1/qemu.out"'
  CMD=(sh -c "$RUN" qemu "$BUILD")
fi
if [[ -n "$LOCAL" ]]; then
  "${CMD[@]}" 2>&1 | tee "$OUT"
else
  docker run --rm -v "$ROOT:/project" -w /project "$ZEPHYR_IMAGE" "${CMD[@]}" 2>&1 | tee "$OUT"
fi
grep -qx PASS "$OUT"
