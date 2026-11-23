# Tests and CI

[CI](../.github/workflows/ci.yml) builds the host agent, ESP-IDF firmware, Zephyr and NuttX targets, and example WASM apps on pull requests and code pushes to `main`. Both RTOS builds cover the NUCLEO-F767ZI over RJ45, both nRF5340 cores on the Nordic nRF7002-DK, and the Raspberry Pi Pico 2 W with MCUboot. Firmware artifacts contain no credentials. The CI secure builds require [signing](SECURITY.md#signing-a-ci-build). Artifacts are retained for 90 days.

The [end-to-end workflow](../.github/workflows/e2e.yml) uses those artifacts and builds its own broken rollback and crash-test images. It runs nightly on the latest successful CI commit unless that commit already passed. To run it manually, select a CI run in Actions or rerun that run's `e2e` job.

Each NuttX artifact includes the matching `nuttx.elf` for decoding backtraces.

| CI artifact | Firmware files |
|---|---|
| `firmware-nuttx-esp32s3-n4r2`, `firmware-nuttx-esp32s3-n8r8`, `firmware-nuttx-esp32s3-n16r8` | Choose the artifact for your module. `mcuboot-esp32s3.bin` is the bootloader, `nuttx.bin` is the confirmed image for the primary slot, and `nuttx-ota.bin` is the compact OTA image. [ESP32-S3 installation](NUTTX.md#firmware-ota-with-mcuboot) |
| `firmware-nuttx-nucleo-f767zi-eth` (RJ45), `firmware-nuttx-pico2w` | Sign `nuttx.bin` with the key trusted by the board. The OTA and confirmed images in `ci-signed/` use the temporary key from that CI run. [Arm image signing](NUTTX.md#signed-firmware-updates-on-arm) |
| `firmware-nuttx-nrf7002dk` | Sign the application core’s `nuttx.bin` with the board’s MCUboot key for OTA or direct installation. The images in `ci-signed/` use the temporary CI key. Install `network-core/nuttx.bin` separately over J-Link. Its matching ELF is in the same directory. [Nordic firmware updates](NUTTX.md#signed-firmware-updates-on-arm) |

The corresponding Zephyr artifacts are `firmware-zephyr-nucleo-f767zi-eth`, `firmware-zephyr-nrf7002dk` and `firmware-zephyr-pico2w`. Each contains MCUboot and application images signed for that run’s bootloader, plus the unsigned application binary and its ELF. The Nordic artifact also includes the network core image and ELF under `hci_ipc/zephyr/`. The archives preserve the paths in the [Zephyr installation guide](ZEPHYR.md).

For the ESP32-S3, `firmware-zephyr-esp32s3` contains MCUboot, the unsigned and signed application binaries and the matching ELF. The ESP-IDF artifacts are `firmware-esp32s3` and `firmware-esp32s3-secure`.

## NuttX release checks on hardware

Before each release, run these checks on both the [ESP32-S3 N16R8](NUTTX.md#verified-ota-on-the-n16r8) and the [NUCLEO-F767ZI over ST-LINK](NUTTX.md#signed-firmware-updates-on-arm). Start with an enrolled board running a signed app. Record its Flight Control device name, running image digest, boot ID and an observable app output.

| Check | Pass condition on each board |
|---|---|
| Update | Deploy the release candidate through Flight Control. After reboot, the device reports the candidate digest, confirms the image and reaches `UpToDate`. |
| Rollback | Deploy a trial image that boots but cannot reach Flight Control to confirm itself. MCUboot restores the confirmed candidate automatically. The agent reports the rollback and the candidate digest. |
| Identity and app | After each transition, the original device returns to `Online` with a new boot ID and no new enrollment. The same app resumes and produces fresh output. |

## Unit tests and fuzzing

| Suite | Covers |
|---|---|
| [Unit tests](../tests/unit.sh), the C core with ASan and UBSan against an in-memory port | Management, OCI, deltas, apps, hardware, console, MQTT, metrics and recovery |
| WASM suite of the unit tests | Fills and restores the sample FIFO, counts record writes, withholds MQTT receipts and exercises two local apps with the broker offline |
| Python tests | Publishing and provisioning tools |
| [Fuzz targets](../tests/fuzz.sh) | HTTP responses, OCI manifests and signatures, rendered specs, certificates, deltas and console input |

| Fuzz run | Time per target | Corpus |
|---|---|---|
| CI on code pushes | 30 s | — |
| [Nightly workflow](../.github/workflows/fuzz-nightly.yml) | 20 min by default | Retained |

The FIFO test also checks that peeking, rejecting a stale acknowledgement and pushing to a full queue produce no writes. Regression inputs for the fuzzers live in `tests/fuzz/corpus`. [FIFO write volume and reboot results](APPS.md#persistent-sample-queues)

## End-to-end tests

| Target | Runs on | Check | Manual or not covered |
|---|---|---|---|
| [Host fleet scenario](../tests/e2e/e2e.sh) | Host | Enrollment and approval, status, config | |
| Host fleet scenario | Host | Signed app install, update and removal | |
| Host fleet scenario | Host | Unsigned app rejection | |
| Host fleet scenario | Host | Retry after a registry outage | |
| Host fleet scenario | Host | Resource alerts, gateway metrics, console and Logs | |
| Host fleet scenario | Host | Git-managed fleets and secret-backed registry credentials | |
| Host identity and recovery | Host | Renewal of a two-minute management certificate | |
| Host identity and recovery | Host | Reconnect after a server restart | |
| Host identity and recovery | Host | Re-enrollment under a new name after expiry offline | |
| Host identity and recovery | Host | API `FactoryReset` removes the app and the device credentials | |
| Zephyr fleet scenario | `native_sim`, Arm `mps2/an385` in QEMU | The host fleet scenario with `DEVICE_KIND=zephyr` or `qemu` | |
| Zephyr fleet scenario | `native_sim`, Arm `mps2/an385` in QEMU | Stored config, through logs | |
| Zephyr fleet scenario | `native_sim`, Arm `mps2/an385` in QEMU | CLI `Unenroll` survives reboot | |
| Zephyr fleet scenario | `native_sim`, Arm `mps2/an385` in QEMU | Erased keys in flash | Not covered |
| Zephyr fleet scenario | `native_sim`, Arm `mps2/an385` in QEMU | Config across re-enrollment | Not covered |
| [ESP-IDF emulator](../tests/e2e/e2e-emu.sh) | C5, S3 and C6, each plain and secure | Signed delta and full image over TLS | |
| ESP-IDF emulator | C5, S3 and C6, each plain and secure | Boot slot confirmation | |
| ESP-IDF emulator | C5, S3 and C6, each plain and secure | Mismatched variant and unsigned app rejected | |
| ESP-IDF emulator | C5, S3 and C6, each plain and secure | Rollback of unreachable firmware | |
| ESP-IDF emulator | C5, S3 and C6, each plain and secure | Decommission | |
| ESP-IDF emulator | Plain C5 only | Certificate renewal | |
| ESP-IDF emulator | C3 | Enrollment and OTA | Manual, with `CHIP=esp32c3` |
| ESP-IDF emulator, provisioning | Plain images, which start with a proof of possession and no network | An incorrect proof is rejected | |
| ESP-IDF emulator, provisioning | Plain images | Enrollment after the test supplies the emulated network | |
| [Host provisioning protocol](../tests/e2e/prov-host.sh) | Host | Wrong Wi-Fi credentials, scanning and radios that must close their AP before joining | |
| ESP-IDF emulator, Secure Boot | Secure images with disposable signing keys | First-boot eFuses, encrypted flash and migration of factory credentials | |
| ESP-IDF emulator, Secure Boot | Secure images with disposable signing keys | Signed OTA, rejected signatures and variants | |
| ESP-IDF emulator, Secure Boot | Secure images with disposable signing keys | Anti-rollback and key rotation | |
| ESP-IDF emulator, Secure Boot | C5 | Signs with its ECDSA peripheral | |
| ESP-IDF emulator, Secure Boot | S3 and C6 | Report a storage key | |
| [Battery telemetry](../tests/espidf-hw/battery.py) | Emulated ESP32-C3 and S3 | Two ADC voltages through a 2:1 divider move `low_battery` from 1 to 0 and back to 1 at the gateway (3,600 mV threshold) | |
| Battery telemetry | Emulated ESP32-C3 and S3 | A simulated Smart Battery gauge reports voltage, charge and charging from a +14 mA current | |
| Battery telemetry | A physical pack | Accuracy and charging | Not covered |
| [ESP-IDF crash reporting](../tests/e2e/fatal-emu.sh) | S3 and C5 | Fault and task reported after reboot | |
| ESP-IDF crash reporting | S3 and C5 | PC and caller decoded with the release ELF | |
| ESP-IDF crash reporting | S3 and C5 | Previous boot log | |
| ESP-IDF crash reporting | S3 and C5 | Panic, abort and interrupt watchdog | |
| ESP-IDF crash reporting | S3 and C5 | Task watchdog | Manual, with `MODE=twdt` |

- The emulator is Espressif's [esp-emulator](https://github.com/espressif/esp-emulator), run as `esp-emu`. It must be on `PATH` or in `~/.local/bin`.
- The emulator's station network uses `10.0.2.0/24` so the firmware can use `192.168.4.0/24` for provisioning.
- Physical verification limits for Secure Boot are in [Device security](SECURITY.md#not-yet-done).
- [Battery readings in emulation](APPS.md#battery-channels)

To reuse an existing cluster, run either test with `SKIP_DEPLOY=1`.

```bash
SKIP_DEPLOY=1 tests/e2e/e2e.sh
SKIP_DEPLOY=1 CHIP=esp32c5 tests/e2e/e2e-emu.sh
```

To boot a local C3 build in the emulator, provision a merged image with the emulator's Wi-Fi credentials.

```bash
tools/provision.py --image port/espidf/build-esp32c3/merged-binary.bin --wifi-ssid myssid --wifi-password mypassword
esp-emu --chip esp32c3 --firmware port/espidf/build-esp32c3/merged-binary.bin --net user,net=10.0.2.0/24
```

### Management timings

Time an operation as the elapsed time observed in Flight Control on a local network: from a spec change to the confirmed state, or from boot to the start of a cached app with management offline. [Results](APPS.md#how-an-app-runs)

## Peripheral tests

| Test | Runs on | Check |
|---|---|---|
| [ESP-IDF fixtures](../tests/espidf-hw), which call the production drivers | Emulated S3 | Read I²C and SPI sensors |
| ESP-IDF fixtures | Emulated S3 | Wait for a UART reply |
| ESP-IDF fixtures | Emulated S3 | Detect button edges |
| ESP-IDF fixtures | Emulated S3 | Generate 1–2 ms PWM pulses |
| ESP-IDF fixtures | Emulated S3, second boot with the wrong chip-select | Must fail |
| ESP-IDF fixtures | Emulated S3 | The first WS2812 pixel on GPIO48 after setting its colour and closing the LED |
| ADC fixture | Emulated S3 and C3 | Driver readings with the potentiometer on GPIO3 set to 1,650 and 1,980 mV |
| [Die-temperature fixture](../tests/espidf-hw/main/temperature.c) | Emulated ESP32-C3, C5, C6 and S3 | Consecutive ready events and an out-of-window event |
| Die-temperature fixture | Emulated ESP32-C3, C5, C6 and S3 | Exclusive ownership, closing and subscribing again |
| Die-temperature fixture | Emulated C5 and C6 | Arming and disarming the hardware comparator |
| Die-temperature fixture | Original ESP32 under Espressif QEMU | The driver must return `FC_ENOTSUP` and leave the output untouched |
| Zephyr button fixture, built with Simple Boot by the Zephyr S3 job next to the MCUboot agent | Emulated S3 | One WASM app reads the same button by its devicetree name and by its fleet alias |
| [Zephyr driver tests](../tests/zephyr-hw.sh) | `native_sim` | GPIO, ADC, PWM, I²C, SPI and UART |
| Zephyr driver tests, RS-485 peer | `native_sim` | Driver enable through the last transmitted bit, timeouts, close and shared claims |
| [App runtime tests](../tests/zephyr-apps.sh) | `native_sim` and Arm QEMU | libc, float formatting and stack usage |

[ADC readings](APPS.md#battery-channels) · [Catalog app and temperature checks on boards](DEVICES.md#app-checks-on-hardware)

Run the ESP-IDF fixtures from `tests/espidf-hw` with esp-emu and its examples installed.

```bash
idf.py -D IDF_TARGET=esp32s3 build merge-bin
python3 run.py build/merged-binary.bin /path/to/esp-emu-1.2.0-examples

idf.py -B build-c3 -D SDKCONFIG=build-c3/sdkconfig -D IDF_TARGET=esp32c3 build merge-bin
python3 run.py --fixture adc build-c3/merged-binary.bin /path/to/esp-emu-1.2.0-examples
```

Build the Zephyr button fixture in the west workspace, then run the second command from `tests/espidf-hw`.

```bash
west build -b esp32s3_devkitc/esp32s3/procpu -d /tmp/button-s3 /path/to/repo/tests/zephyr-apps
python3 run.py --fixture zephyr /tmp/button-s3/zephyr/zephyr.bin /path/to/esp-emu-1.2.0-examples
```

## App benchmark methods

| Workload | Parameter | Value |
|---|---|---|
| Timing probes for `tinyml` (forest), `tinymlp` (neural), `trend` and `battery` | Firmware and clock | Standard firmware and 1 ms device clock |
| Timing probes | Operations per batch | 256, and 32 for the neural network |
| Timing probes | Reported value | Median of seven batches after two warm-ups, divided by the operations per batch |
| Timing probes | Includes | Preemption |
| Timing probes | Excludes | Callback entry |
| Timing probes | Classifier input | Both classifiers process the same 16 temperature windows and must return every expected class. [Input vectors](../wasm-apps/tinyml/vectors.h) |
| Timing probes | `trend256` | The `trend` probe. It calls the production `trend` sample function 256 times between two `fc_time_ms` calls. |
| Periodic callback | Timed interval | Elapsed time around the WASM call |
| Periodic callback | Reported value | Median of 32 callbacks per mode after warm-up, alternating modes in one boot |
| Periodic callback | Classic ESP32 | `battery` without a pack, because `alarm` disables its timer without a die sensor |
| `mqttdemo` command | Timed interval | From command to matching application acknowledgement over established TLS connections |
| `mqttdemo` command | Samples | 48 QoS 1 round trips per board |
| Instruction limit | Timed interval | A counting loop of five million interpreter instructions. [Times by chip](APPS.md#size-and-limits) |

[App benchmarks](BENCHMARKS.md#catalog-apps)

### Separating temperature acquisition from the app

The [matched-input figures](BENCHMARKS.md#temperature-with-matched-input) come from one S3 N8R8 (ESP32-S3 DevKitC, 8 MiB flash, 8 MiB octal PSRAM) with one 240 MHz core on each port. Each port ran two separate installations with live readings and one control installation with the fixed input. Each installation of the signed `trend256` module runs two warm-up batches, then seven measured batches. Diagnostic firmware reads the CPU cycle counter at the batch boundaries. Divide the elapsed cycles by 240 and by 256 for µs per sample. The independent millisecond times in the app state cover the same seven batches.

| Term | Definition |
|---|---|
| Complete sample | Cycles between the batch boundaries |
| Physical sensor | Cycles around each native `die_temp` callback, summed over the batch |
| Rest of `trend` | Complete sample minus the sensor callbacks. It includes WASM dispatch, metrics calls, formatting, state updates, console output and any preemption in those intervals. |
| Live input | `trend` receives the measured temperature, so the number of state and log updates depends on the reading |
| Fixed input | After the timed physical read, the callback returns alternating blocks of 32 samples at 40 and 41 °C, which fixes the input to the filter and the number of state and log updates |
| State and output counters | Native `set_state`, including its log and status notification, and the native path that writes app stdout |
| NuttX driver counters | `open`, `read` and `close` on `/dev/temp`, plus the raw register read and ready wait nested inside `read` |

Firmware counts calls and read errors and keeps the batches in RAM. The agent console retrieves them after the run. [Per-batch counters](benchmarks/esp32s3-n8r8-temperature.json)

#### NuttX descriptor reuse under WASM load

Source of the NuttX row in the [separate instrumented runs](BENCHMARKS.md#temperature-with-matched-input). [Cycle counts and device-clock timings](benchmarks/esp32s3-nuttx-temp-fd.json)

1. Build the original and descriptor-reuse firmware, both with the stock NuttX temperature driver.
2. Boot one N8R8 four separate times, in the order original, reuse, reuse, original, with Wi-Fi and the management agent active.
3. In each boot, install the same signed `trend256` probe twice, with the order of live and fixed input reversed on alternate boots.
4. Discard the first two batches in each installation.
5. For each build and input, take the median of 14 batch averages from two boots.
6. Count opens, closes, read errors and state writes in the same intervals.

Every sample calls the physical driver. With fixed input, the firmware substitutes the 40/41 °C trace after the timed sensor call. This keeps the real acquisition cost in the total while giving the app identical work. The probe's initialization also reads the sensor before starting its batches.

### Comparing the 2,000-byte workload with Toit

Both sides run on the classic ESP32 at 240 MHz. [EEMBC reference and run rules](https://github.com/eembc/coremark/tree/1f483d5b8316753a742cbf5590caf5bd0a4e4777#run-rules)

WASM:

1. Install the signed 64 KiB wasm3 CoreMark module three times, one app at a time, with `FC_ENTRY=run` and metrics export disabled.
2. Record the iterations per second returned by each installation. The module calibrates its iteration count.

Toit:

1. Compile the [local Toit port](../wasm-apps/runtime-bench/kernels.toit) with Toit 2.0.0, `-O2 --enable-asserts`, and run it as a boot container on the same board. The program checks all three first-iteration CRCs with each of the two seed sets.
2. Time three batches of 320 iterations with performance seeds `0, 0, 0x66`. Each batch initializes a fresh workload before starting its timer.
3. Divide 320 by the elapsed seconds for each batch and take the median rate.
4. Check the final CRC against the reference C program at 320 iterations (`0x988c`).

The Toit port uses the reference list, matrix and state-machine inputs with a logical workload size of 2,000 bytes. Toit objects and arrays use the VM's own memory representation.

### Richards and DeltaBlue

| Workload | One iteration | Check |
|---|---|---|
| Richards | The scheduler with an idle count of 10,000 | 23,246 queued packets and 9,297 holds |
| DeltaBlue | Ten chain and projection pairs | Propagation after changing the source, destination, scale and offset |
| DeltaBlue chain | 50 equality constraints | — |
| DeltaBlue projection | 50 source/destination pairs | — |

- Toit: use the [Toit benchmark suite at `2a95116`](https://github.com/toitlang/benchmarks/tree/2a951160d1c65e01463abf8de644a366195a6df5). Compile snapshots at the default `-O1` and at `-O2`, with `--enable-asserts` for both.
- MicroPython: use the same suite. Check the Richards return value and treat each failed DeltaBlue check as an error. Reset the Richards task list and collect garbage before each batch.
- Flight Control MCU: [build the C ports as standalone WASM modules](../wasm-apps/runtime-bench/build.sh) and run one signed module at a time with `FC_ENTRY=run` and metrics export disabled. Each module logs three batch times and returns the median batch time.

1. On the same classic ESP32 at 240 MHz, time three batches of ten iterations on each runtime. Start the clock after loading the program and stop it before printing.
2. Divide each batch time by ten and report the median in milliseconds per iteration.

The C ports use the app SDK allocator and run under the normal agent scheduler. The times include differences in the benchmark implementations, allocation and interpreter scheduling.

| Build command | Result |
|---|---|
| `wasm-apps/runtime-bench/build.sh --release v0.9.5` | Both catalog benchmark modules, listed under `richards` and `deltablue` in the `stable` channel of the lab catalog |
| `wasm-apps/runtime-bench/build.sh` | The first-fit modules, with the SDK v0.9.4 allocator |

#### DeltaBlue allocation profile

The four [profile builds](../wasm-apps/runtime-bench/profile/build.sh):

- `phases` times graph construction (including adding edit constraints), plan extraction and the loops that execute each plan. The remainder covers graph reset, removal of constraints, destruction of plans and checks outside those loops. Each batch's four phase times sum to its elapsed time.
- `counts` counts each candidate block header that the first-fit loop inspects, including the selected block. A visit can revisit a previously inspected block. The count excludes the end sentinel and look-ahead reads when coalescing adjacent free blocks. Calls from `realloc` that fall back to `malloc` are included.
- `timed` brackets every `malloc` with the device's millisecond clock and accumulates elapsed time before logging.
- `empty` is the control. It takes both timestamps immediately before each allocation.

1. Run the first-fit DeltaBlue module and the four profile builds with the same arena, inputs, propagation checks and batch size.
2. Subtract the `empty` control's median accumulated interval from the `timed` median.
3. Divide by the first-fit module's median elapsed time to estimate the share spent in `malloc`.

The estimate includes preemption inside the interval. Standalone `free` and in-place `realloc` work are outside it.

#### DeltaBlue free-list comparison

| Both modules | Value |
|---|---|
| Arena | 40,960 bytes |
| Stack | 2,048 bytes |
| WASM memory | One fixed 65,536-byte page |

Both modules run the same propagation checks, with timing only around each ten-iteration batch.

1. Run `wasm-apps/runtime-bench/build.sh` and then `wasm-apps/runtime-bench/build.sh --freelist` with the same compiler. The second build reuses the first build's DeltaBlue source and substitutes an address-ordered list of free blocks.
2. Install the signed modules in the order first-fit, free list, first-fit, free list on the same ESP32 boot. Each installation starts a fresh module and logs three batches.
3. Divide each batch time by ten and take the median of all six values for each allocator.
4. Compare these medians with the previously measured Toit `-O2` median on that board.

#### Fragmentation and time spent in free

| Parameter | Value |
|---|---|
| Workload | [fragmentation.c](../wasm-apps/runtime-bench/fragmentation/fragmentation.c), built with `wasm-apps/runtime-bench/fragmentation/build.sh` |
| Modules | Two standalone modules, with the SDK v0.9.4 (first-fit) and SDK v0.9.5-rc.1 (free list) allocator headers |
| Arena | 40,960 bytes, one static arena for the entire installation |
| A successful run returns | 40,948, the largest single user allocation |
| Mixed workload | 12 cycles of 2,048 deterministic operations over 80 slots |
| Allocation sizes | 1–2,048 B |
| Timer | The app timer reads milliseconds |

1. Run each module with `FC_ENTRY=run`.
2. Each run starts with a capacity check. The modules check the first payload address, fill all usable bytes, and verify that a further one-byte allocation fails.
3. The run then executes the mixed workload. Both variants receive the same slot numbers, operations and sizes. Each live allocation carries a slot-specific pattern, checked before reuse, after resizing and at teardown. Each cycle ends by requesting the whole usable arena again.
4. For the timed cases, allocate four-byte objects until the arena is full and free alternating blocks. Verify that a five-byte request fails with 2,559 isolated holes.
5. Measure freeing the last live block past 0, 128, 1,024 and 2,559 holes, then measure a release with 2,558 free predecessors and free blocks on both sides. The module restores the affected metadata after each timed call to reproduce the same layout. A checksum covers the entire arena before and after each case.
6. A final sweep frees every live block and repeats the capacity check.
7. For each case collect seven pairs of batches, alternating which batch runs first. Subtract the time for an empty indirect call and the identical metadata restore, then divide by the call count.
8. A separate series times 256 individual calls per case. Its maximum includes scheduling and the timer's 1 ms granularity.

[On-device measurements](WAMR.md#fragmentation-on-classic-esp32)

## Registry tokens

The manual [registry test](../tests/registry-bearer.sh) needs Docker, ORAS, OpenSSL, htpasswd with bcrypt, Python 3, CMake, Ninja and mbedTLS 4. It runs `registry:3` with same-host and cross-host Bearer realms.

```bash
tests/registry-bearer.sh
QUAY=1 tests/registry-bearer.sh
```

| Option | Effect |
|---|---|
| `QUAY=1` | Also tests a pull-only robot account on Quay, which needs about 1 GB |
| `QUICK=1` | Skips the 70 s token-expiry wait |
| `REG_PORT`, `AUTH_PORT`, `PROXY_PORT`, `QUAY_PORT` | Override the ports |

Checks include signed multi-chip firmware and WASM, public and private repositories, refused credentials, denied scopes, untrusted CAs and token expiry mid-pull. Output must not contain the password. [Registry configuration](REGISTRY.md)

## MQTT broker

Run `tests/mqtt-broker.sh` with Docker, OpenSSL, CMake, Ninja and mbedTLS 4. The manual test runs the host client against Mosquitto on plain TCP and TLS. It checks authentication, CA validation, both topic prefixes, QoS 1, oversized messages, keep-alive and resubscription after a broker restart. Allow about two minutes. [Device MQTT configuration and delivery counts](APPS.md#mqtt)
