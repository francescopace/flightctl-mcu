#!/usr/bin/env bash
# Fetches WAMR (WebAssembly Micro Runtime) at a pinned release into
# third_party/wamr (git-ignored), and applies the patches in
# third_party/patches/wamr-*.patch. The firmware builds it as an ESP-IDF
# component, and the Zephyr build from the same tree.
#   wamr-unaligned-select.patch: the fast interpreter's loader rewrites a
#   `select` of 64-bit values with a plain 32-bit store into code that is only
#   2-byte aligned on chips without unaligned access. On a Cortex-M0+ (the
#   Pico W) that is a HardFault while the app loads. The loader's other
#   stores use STORE_U32, which splits them; the patch does the same.
#   wamr-espidf-realloc.patch: on ESP-IDF, os_malloc puts the data 4 or 8
#   bytes into the block (8-byte aligned) and the block's address before it;
#   os_realloc let realloc move the block and then took the new block's
#   offset, which may differ: the data came back shifted by 4 bytes, with the
#   address written over one word. The loader's constant table, grown that
#   way, lost a constant, so its two passes disagreed on the code's size and
#   the second wrote past its buffer: an ESP32-C6 restarted at every boot
#   while loading alarm:v2.3, other chips by the luck of the heap's layout.
#   The patch moves the data to the new offset.
#   wamr-metering-stop.patch: with instruction metering and no thread manager,
#   a call in progress cannot be stopped from another thread (the count is a
#   local of the interpreter). The patch checks, at branches and calls, as the
#   thread manager does, whether the exec env's limit was set to 0, and then
#   traps: an app's run-once export (FC_ENTRY), which has no budget, can be
#   removed while it runs.
#   wamr-espidf-psram-interp.patch: on an ESP32-S3 built with PSRAM, WAMR
#   asked for every linear memory in PSRAM only (meant for AOT code, which
#   this firmware does not build): where the image found no PSRAM, no app
#   could start. Now only with AOT; the interpreter's memory comes from
#   internal RAM first, PSRAM when no internal block is large enough.
#   wamr-bytecode-placement.patch: on SRAM-only C5/C6 let the app loader
#   resize its heap reservation after reading the memory declaration and
#   before allocating the fast interpreter's decoded functions.
#   wamr-instantiation-hook.patch: the code a module runs as it is
#   instantiated (its start function, __post_instantiate, __wasm_call_ctors)
#   ran in an exec env WAMR made for it, which the embedder never saw: with
#   no instruction limit (a loop there was never stopped, and the app could
#   be neither updated nor removed) and no user data (a native called from
#   there found no app). wasm_runtime_set_instantiation_hook() is called with
#   that exec env before and after, so the firmware runs it as a call of the
#   app. The interpreter only; this firmware builds no AOT.
#   wamr-thumb-stack-align.patch: invokeNative for Cortex-M without a
#   floating-point unit (mps2/an385 in QEMU) aligned the stack to 8 bytes,
#   then took 4 more for a native function with arguments on the stack (five
#   or more): the function ran with the stack 4 bytes off what C requires.
#   An app's snprintf (five arguments) then gave a double to the C library's
#   own snprintf from the wrong place, and %f printed nonsense.
# None is in upstream as of WAMR 2.4.5 and main.
set -euo pipefail
WAMR_TAG="${WAMR_TAG:-WAMR-2.4.5}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="$HERE/wamr"
if [[ ! -d "$DIR/.git" ]]; then
  git clone --depth 1 --branch "$WAMR_TAG" https://github.com/bytecodealliance/wasm-micro-runtime "$DIR"
fi
for p in "$HERE"/patches/wamr-*.patch; do
  # The libc subset changes the context immediately following the const
  # table declaration. Check the earlier patch's actual edits in that tree.
  if [[ ${p##*/} == wamr-const-natives.patch ]] &&
     git -C "$DIR" apply --reverse --check -C0 "$p" 2>/dev/null; then
    continue
  fi
  if git -C "$DIR" apply --reverse --check "$p" 2>/dev/null; then
    continue # already applied
  fi
  git -C "$DIR" apply "$p"
  echo "WAMR: applied $(basename "$p")"
done
echo "WAMR in $DIR ($(git -C "$DIR" describe --tags))"
