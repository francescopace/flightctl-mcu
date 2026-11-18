# AGENTS.md

Notes for coding agents working in this repository. The code and `docs/` explain what the agent does; this file holds what they don't: the checks CI runs, the traps in the builds, and how we work.

## Before a push

When the user requests a push, record the SHA of the final commit to publish. Run the checks below once in a temporary detached worktree at that SHA. Run these checks on an intermediate commit only if explicitly requested. If the cumulative diff from the remote tip contains only Markdown, files under `docs/` or the root `LICENSE`, skip builds and tests as CI does. Pass `--include-docs` if the user asks for the checks on a documentation-only push. After the applicable checks pass, push the selected SHA and remove the temporary worktree. Report the SHA and which checks passed. If fuzzing was requested, include the duration per target.

The `tests` job in [the CI workflow](.github/workflows/ci.yml) builds the host provisioning executable before exercising it. For code changes, export `MBEDTLS_DIR` pointing to the directory containing `MbedTLSConfig.cmake` for the project's mbedTLS build. The optional local fuzz run on macOS uses Homebrew LLVM because Apple's clang lacks libFuzzer.

```bash
remote_tip=$(git rev-parse origin/main)  # after fetching the remote tip
python3 tests/pre-push.py "$remote_tip"
```

- The runner copies WAMR and the Espressif provisioning tools from a cache shared by worktrees (`build/pre-push-cache` in the main checkout, or `FC_CHECK_CACHE`). It keys each copy on its fetch script, version override and patches, and reuses the provisioning virtualenv for the same Python interpreter. The Docker ESP32-S3 build keeps compiler objects in the `flightctl-zephyr-s3-ccache` volume.
- CTest runs two executables at a time. With `--fuzz`, the pre-push runner runs up to three fuzzers at a time and keeps each log and any failure input in `build/fuzz/findings/<target>/`. Use `CTEST_PARALLEL_LEVEL=1` or `FUZZ_JOBS=1` for serial execution.
- Fuzzing runs in CI (30 seconds per target on code pushes) and nightly (20 minutes per target by default). Only build and run the fuzzers locally when explicitly requested: pass `--fuzz` for 3 seconds per target, or `FUZZ_SECONDS=30` with `--fuzz` for the CI duration.
- The fuzzers link the core against their own port stubs, in `tests/fuzz/`, apart from the unit tests' `tests/unit/port_stub.c`. A new `fc_port_*` function needs a stub there too, or the fuzz build breaks while `tests/unit.sh` stays green.
- If the cumulative diff from the remote tip to the commit being pushed touches the core's statics, thread stacks or port code, also link the Zephyr build for the ESP32-S3 in the same worktree, then delete `port/zephyr/build-esp32s3-check`:

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
- Run one heavy build at a time. For containers, use `docker ps` to identify the build command and `docker inspect --format '{{range .Mounts}}{{println .Source}}{{end}}' <id>` to identify the checkout. For native Zephyr builds, check `pgrep -fl '[w]est build'` and match the application or build directory to a project worktree. Wait for the matching build to finish.
- Don't run builds while `tests/e2e/e2e-emu.sh` is running: the emulated clock falls behind and the test's 2-minute certificates expire before the device renews them.

## Boundaries

- No changes to Flight Control's server. The agent uses only its public device API; gaps go upstream (`docs/FLIGHTCTL-LIMITATIONS.md` lists them), not into local patches.
- `.secrets/` holds signing keys, enrollment credentials and registry passwords. Don't read, print or copy them; scripts that need them load them by path.
- Real boards: never burn eFuses or flash a `secure` build to them; both are irreversible. Before opening a serial port, match the board by its USB serial number, since several boards share one Mac and port names change.
- Push only when the user asks for it.

## Git

- Work on `main`; no feature branches.
- Coordinate with other agents so only one writer stages and commits on `main` at a time. If `git diff --cached` shows someone else's work, leave it staged and wait for its owner to finish. Starting with a clean index, stage a patch containing only your hunks with `git apply --cached`, review the staged diff and commit it. Don't use `git add -p` or `git add -A`.
- Commit subjects say what changed in behaviour, as a sentence, optionally after an area (`Agent: …`, `Tests: …`, `Docs: …`). See `git log` for the tone.

## Docs

`docs/` is written for people who deploy the agent, not for our bench. Give what was verified and the figures (heap, timings, sizes), the limits, and what is not done yet. Keep the history of how a bug was found out of them. `docs/ROADMAP.md` lists only what is open: remove an item when it's done, and put its figures in `docs/RESULTS.md`.
