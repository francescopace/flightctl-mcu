#!/usr/bin/env bash
# Runs the mps2/an385 build of the Zephyr application in QEMU: an Arm
# Cortex-M3 with Ethernet, in the Zephyr build image or with this machine's
# QEMU. The network is QEMU's user network: the device gets 10.0.2.15 by DHCP
# and reaches the host's network through it.
# Usage: port/zephyr/run-qemu.sh FLASH.bin [QEMU args...]
# FLASH.bin is the device's flash: its credentials (tools/provision.py
# --format zephyr --board mps2_an385 --image FLASH.bin). The flash is SRAM
# here (boards/mps2_an385.overlay): QEMU loads the credentials partition into
# it at every start of the board, and what the device stores survives its
# restarts, not the end of QEMU.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ZEPHYR_IMAGE="${ZEPHYR_IMAGE:-ghcr.io/zephyrproject-rtos/ci-base:v0.30.0}"
BUILD_DIR="${BUILD_DIR:-build-mps2_an385}"
FLASH="${1:?usage: port/zephyr/run-qemu.sh FLASH.bin [QEMU args...]}"
shift
FLASH="$(cd "$(dirname "$FLASH")" && pwd)/$(basename "$FLASH")"
# The binary, not the ELF: the ELF has a segment for the flash simulator's
# SRAM too, which QEMU would load as zeros at every reset.
IMAGE="port/zephyr/$BUILD_DIR/zephyr/zephyr.bin"
[[ -f "$ROOT/$IMAGE" ]] || { echo "no $ROOT/$IMAGE: BOARD=mps2/an385 port/zephyr/build.sh" >&2; exit 1; }
case "$FLASH" in "$ROOT"/*) ;; *) echo "the flash file must be inside $ROOT (it is shared with the container)" >&2; exit 1 ;; esac
# The credentials partition alone (fc_creds, the first 16 KB): QEMU writes
# what it loads again at each reset, which would erase the storage after it.
CREDS="$FLASH.creds"
head -c 16384 "$FLASH" > "$CREDS"
ARGS=(-cpu cortex-m3 -machine mps2-an385 -nographic -vga none
  -kernel "$IMAGE"
  -device "loader,file=${CREDS#"$ROOT"/},addr=0x20300000,force-raw=on"
  -nic user "$@")
# QEMU from the Zephyr SDK (third_party/fetch-zephyr-sdk.sh), for the
# machine it runs on, or this machine's.
if [[ "$(uname)" == Linux && -z "${ZEPHYR_DOCKER:-}" ]]; then
  cd "$ROOT"
  # || true: with pipefail, no SDK would end the script here, silently.
  QEMU="$(ls third_party/zephyr-sdk/hosttools/sysroots/*/usr/bin/qemu-system-arm 2>/dev/null | head -1 || true)"
  exec "${QEMU:-qemu-system-arm}" "${ARGS[@]}"
fi
exec docker run --rm -i ${DOCKER_ARGS:-} -v "$ROOT:/project" -w /project "$ZEPHYR_IMAGE" \
  sh -c 'exec $(ls third_party/zephyr-sdk/hosttools/sysroots/*/usr/bin/qemu-system-arm | head -1) "$@"' qemu "${ARGS[@]}"
