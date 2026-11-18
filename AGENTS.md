# AGENTS.md

Notes for coding agents working in this repository. The code and `docs/` explain what the agent does; this file holds what they don't: the checks CI runs, the traps in the builds, and how we work.

## Before every commit

CI goes red on things that pass a quick local run, and a red main blocks everyone. Run the same checks as the CI job "unit tests, field provisioning, quick fuzzing" on the exact tree you commit, and say in the commit's report which ones you ran.

```bash
third_party/fetch-wamr.sh
```

```bash
tests/unit.sh
```

```bash
FUZZ_SECONDS=3 CC=/opt/homebrew/opt/llvm/bin/clang tests/fuzz.sh
```

- On macOS, Apple's `/usr/bin/clang` has no libFuzzer and every fuzz target fails to link; use Homebrew's LLVM as above. On Linux, `CC=clang` is enough.
- The fuzzers link the core against their own port stubs, in `tests/fuzz/`, apart from the unit tests' `tests/unit/port_stub.c`. A new `fc_port_*` function needs a stub there too, or the fuzz build breaks while `tests/unit.sh` stays green.
- When a change touches the core's statics, thread stacks or port code, also link the Zephyr build for the ESP32-S3, then delete `port/zephyr/build-esp32s3-check`:

```bash
BOARD=esp32s3_devkitc/esp32s3/procpu BUILD_DIR=build-esp32s3-check port/zephyr/build.sh
```

That build has about 4 KB of DRAM left at link time, so a few KB of new statics or stack is enough to break it.

## Writing tests

Tests must not depend on wall-clock margins: CI's runner is slower than a workstation, and timing checks that passed locally have failed there. Count calls, events or log lines instead of measuring time. If a test reads the stub log, clear it first (`stub_log_clear()`), since the log is a fixed-size buffer.

## Portability traps

- `core/` is portable POSIX C (mbedTLS, cJSON, WAMR). Chip and RTOS specifics go behind `port/` (`espidf`, `zephyr`, `host`); every port implements each `fc_port_*` function.
- Zephyr's Espressif HAL defines `ESP_PLATFORM` too. Code meant for ESP-IDF only needs `#if defined(ESP_PLATFORM) && !defined(__ZEPHYR__)`.
- Zephyr on Xtensa has no 64-bit atomics (`__atomic_store_8` fails to link). Keep shared counters in a `long`.
- WASM apps are built with `-mcpu=mvp` (see `wasm-apps/build.sh`): the device's WAMR has no reference types.

## Builds

- Firmware: `port/espidf/build.sh` (ESP-IDF in Docker); CI uses `port/espidf/build-ci.sh <target> [version] [variant]`. Builds land in `port/espidf/build-<target>[-<variant>]`.
- Zephyr: `port/zephyr/build.sh`, `BOARD=` to pick the board (default `native_sim`).
- Host agent: `port/host/build-ci.sh`. WASM apps: `NO_PUSH=1 wasm-apps/build.sh <app> <version>`.
- Run one heavy build at a time. To wait for another build, check `docker ps` or `pgrep -x docker`; don't grep `ps` output for "docker run", which matches the waiting shell's own command line.
- Don't run builds while `tests/e2e/e2e-emu.sh` is running: the emulated clock falls behind and the test's 2-minute certificates expire before the device renews them.

## Boundaries

- No changes to Flight Control's server. The agent uses only its public device API; gaps go upstream (`docs/FLIGHTCTL-LIMITATIONS.md` lists them), not into local patches.
- `.secrets/` holds signing keys, enrollment credentials and registry passwords. Don't read, print or copy them; scripts that need them load them by path.
- Real boards: never burn eFuses or flash a `secure` build to them; both are irreversible. Before opening a serial port, match the board by its USB serial number, since several boards share one Mac and port names change.
- Push only when the user asks for it.

## Git

- Work on `main`; no feature branches.
- Other agents may be editing the same checkout. Commit only your own hunks: write them to a patch and stage it with `git apply --cached`, then leave the index empty. Don't use `git add -p` or `git add -A`.
- Commit subjects say what changed in behaviour, as a sentence, optionally after an area (`Agent: …`, `Tests: …`, `Docs: …`). See `git log` for the tone.

## Docs

`docs/` is written for people who deploy the agent, not for our bench. Give what was verified and the figures (heap, timings, sizes), the limits, and what is not done yet. Keep the history of how a bug was found out of them. `docs/ROADMAP.md` lists only what is open: remove an item when it's done, and put its figures in `docs/RESULTS.md`.
