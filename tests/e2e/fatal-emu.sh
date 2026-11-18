#!/usr/bin/env bash
# What a crash was, in the status after the restart (customInfo.lastFatalError),
# on the ESP-IDF firmware in Espressif's emulator against Flight Control: a
# test build (sdkconfig.faulttest) writes to address 0 from a task of its own
# 30 s after power-on; after the restart the status must name the fault, the
# task and a pc that addr2line decodes, against the build's ELF, to the
# function that faulted, with its caller among the return addresses.
#
#   tests/e2e/fatal-emu.sh                 CHIP=esp32c3 (RISC-V)
#   CHIP=esp32s3 tests/e2e/fatal-emu.sh    Xtensa: a real backtrace
#   MODE=abort ...                         abort() instead (sdkconfig.aborttest): the pc
#                                          is abort()'s caller, not the panic's own frame
#   MODE=iwdt ...                          a loop with interrupts off, which the interrupt
#                                          watchdog ends (sdkconfig.iwdttest): resetReason int_wdt
#   MODE=twdt ...                          a loop that starves the idle task, which the task
#                                          watchdog, set to panic, ends (sdkconfig.twdttest):
#                                          resetReason task_wdt
#   PREBUILT=1 ...                         use port/espidf/build-<chip>-<variant> as it is
#
# Needs Flight Control running and logged in (tests/e2e/e2e-emu.sh deploys it),
# esp-emu, docker (ESP-IDF image) and python3. About 3 minutes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
source tools/local-ip.sh
IP="${HOST_IP:-$(local_ip)}"
CHIP="${CHIP:-esp32c3}"
IDF_IMAGE="${IDF_IMAGE:-espressif/idf:v6.0.3}"
MODE="${MODE:-fault}"
case "$MODE" in
  fault) VARIANT=faulttest RESET=panic REASON='' ;;
  abort) VARIANT=faulttest-aborttest RESET=panic REASON='abort() (' ;;
  iwdt) VARIANT=faulttest-iwdttest RESET=int_wdt REASON='Interrupt wdt timeout on CPU' ;;
  twdt) VARIANT=faulttest-twdttest RESET=task_wdt REASON='task watchdog (' ;;
  *) echo "MODE: fault, abort, iwdt or twdt" >&2; exit 2 ;;
esac
BUILD="port/espidf/build-$CHIP-$VARIANT"
WORK="$ROOT/build/fatal-emu"
EMU_PID=""
DEVICE=""

fail() {
  echo "FAIL: $*" >&2
  grep -v '^\s*$' "$WORK/emu.log" 2>/dev/null | tail -40 >&2 || true
  exit 1
}
cleanup() {
  [[ -n "$EMU_PID" ]] && kill "$EMU_PID" 2>/dev/null || true
  if [[ -n "$DEVICE" ]]; then
    flightctl delete "device/$DEVICE" >/dev/null 2>&1 || true
    flightctl delete "enrollmentrequest/$DEVICE" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT
custom() {
  flightctl get "device/$DEVICE" -o json 2>/dev/null |
    python3 -c "import json,sys; print(((json.load(sys.stdin).get('status') or {}).get('systemInfo') or {}).get('customInfo', {}).get('$1', ''))" 2>/dev/null || true
}

rm -rf "$WORK" && mkdir -p "$WORK"
if [[ -z "${PREBUILT:-}" ]]; then
  echo "== build ($CHIP, $VARIANT)"
  LOCAL=0 TARGET="$CHIP" VARIANT="$VARIANT" FW_VERSION=faulttest port/espidf/build.sh build merge-bin >"$WORK/build.log" 2>&1 ||
    { tail -30 "$WORK/build.log"; fail "firmware build"; }
fi
[[ -f "$BUILD/merged-binary.bin" ]] || fail "no $BUILD/merged-binary.bin"

echo "== provision and boot"
BATCH="fatal-$(date +%s)" EXPIRATION=1d tools/enrollment.sh "$WORK/secrets" >/dev/null
cp "$BUILD/merged-binary.bin" "$WORK/flash.bin"
tools/provision.py --image "$WORK/flash.bin" --secrets "$WORK/secrets" --server "agent-api.$IP.nip.io:8443" \
  --labels "app=fatal-emu,site=emu" --wifi-ssid myssid --wifi-password mypassword >/dev/null
ESP_EMU="$(command -v esp-emu || echo "$HOME/.local/bin/esp-emu")"
"$ESP_EMU" --chip "$CHIP" --firmware "$WORK/flash.bin" --net user --timeout 900s >"$WORK/emu.log" 2>&1 &
EMU_PID=$!
for i in $(seq 1 90); do
  DEVICE=$(sed -n 's/.*device name: \([a-z0-9]*\).*/\1/p' "$WORK/emu.log" | head -1)
  [[ -n "$DEVICE" ]] && flightctl get "enrollmentrequest/$DEVICE" >/dev/null 2>&1 && break
  (( i == 90 )) && fail "no enrollment request"
  sleep 2
done
printf 'devices:\n  - name: %s\n    labels:\n      stage: emu\n' "$DEVICE" >"$WORK/allowlist.yaml"
tools/auto-approve/auto-approve.py "$WORK/allowlist.yaml" >/dev/null

echo "== the fault, the restart, the status"
for i in $(seq 1 100); do
  FATAL="$(custom lastFatalError)"
  [[ -n "$FATAL" ]] && break
  (( i == 100 )) && fail "no lastFatalError in the status"
  sleep 3
done
echo "lastFatalError: $FATAL"
[[ "$(custom resetReason)" == "$RESET" ]] || fail "resetReason is $(custom resetReason), not $RESET"
[[ "$FATAL" == *'in thread "fc_fault"'* ]] || fail "not the fc_fault task"
[[ "$FATAL" == "$REASON"* ]] || fail "not what $MODE gives: $REASON"
PANIC='Guru Meditation Error|abort\(\) was called|Task watchdog got triggered'
grep -qE "$PANIC" "$WORK/emu.log" || fail "no panic in the emulator's log"
grep -q 'last crash: ' "$WORK/emu.log" || fail "the crash not logged at the next boot"

# The previous boot's log ends with the same text (fc_log.h: kept across the
# restart). A console session can fail to open right after the restart: tried
# again.
for i in $(seq 1 5); do
  PREV=$(python3 -c 'import subprocess, sys; r = subprocess.run(sys.argv[1:], capture_output=True, text=True, timeout=60); print(r.stdout, end=""); sys.stderr.write(r.stderr)' \
    flightctl console "device/$DEVICE" -- journalctl -b -1 -n 3 2>"$WORK/console.err" | grep -v '^\s*$' || true)
  [[ -n "$PREV" ]] && break
  sleep 5
done
echo "$PREV" | tail -1
echo "$PREV" | tail -1 | grep -qF "fatal: ${FATAL%%,*}" ||
  { tail -3 "$WORK/console.err" >&2; fail "the previous boot's log does not end with the crash"; }

# Decoded against the ELF: the pc in the function that faulted, its caller in
# the addresses after it.
PC=$(sed -n 's/.* pc \(0x[0-9a-f]*\).*/\1/p' <<<"$FATAL")
ADDRS=$(sed -n 's/.* pc 0x[0-9a-f]*\(.*\)/\1/p' <<<"$FATAL" | grep -o '0x[0-9a-f]*' | tr '\n' ' ')
[[ "$CHIP" == esp32s3 ]] && A2L=xtensa-esp32s3-elf-addr2line || A2L=riscv32-esp-elf-addr2line
docker run --rm -v "$ROOT:/project" -w /project "$IDF_IMAGE" bash -c \
  "$A2L -pfC -e $BUILD/flightctl_mcu_fw.elf $PC; echo --; $A2L -pfC -e $BUILD/flightctl_mcu_fw.elf $ADDRS" \
  2>/dev/null | grep -v '^\(Checking\|Python\|\*\|Activating\|Setting\|Done\|Go to\|  idf.py\|"python3\)' | grep -v '^$' >"$WORK/decoded.txt"
cat "$WORK/decoded.txt"
head -1 "$WORK/decoded.txt" | grep -q '^fc_fatal_test_fault ' || fail "the pc does not decode to fc_fatal_test_fault"
sed '1,/^--$/d' "$WORK/decoded.txt" | grep -q '^fault_task ' || fail "fault_task not among the return addresses"
echo "ok: $CHIP ($MODE): lastFatalError names the fault and the task; pc and caller decode against the ELF"

if [[ "$MODE" == fault ]] && grep -qx 'CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y' "$BUILD/sdkconfig"; then
  size="$(custom coredumpBytes)"
  [[ "$size" =~ ^[1-9][0-9]*$ ]] || fail "no valid dump size in the device status"
  (
    umask 077
    flightctl console --notty "device/$DEVICE" -- coredump read >"$WORK/crash.bin"
  ) || fail "coredump download"
  (( $(wc -c <"$WORK/crash.bin") == size )) || fail "incomplete coredump download"
  docker run --rm --network none -v "$WORK/crash.bin:/dump.bin:ro" \
    -v "$ROOT/$BUILD/flightctl_mcu_fw.elf:/firmware.elf:ro" "$IDF_IMAGE" \
    bash -ec 'esp-coredump --chip "$1" info_corefile -t raw -c /dump.bin /firmware.elf >/tmp/report
              grep -qE "^#0 .*fc_fatal_test_fault" /tmp/report
              grep -qE "^#1 .*fault_task" /tmp/report' _ "$CHIP" || fail "coredump backtrace"
  echo "ok: $size downloaded bytes decode to the fault and its caller"
fi

# Once: the test build faults only after a boot that was not a crash.
sleep 40
(( $(grep -cE "$PANIC" "$WORK/emu.log") == 1 )) || fail "faulted again after the restart"
echo "ok: one crash, the device on afterwards"
