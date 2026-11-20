# Tests and CI

[CI](../.github/workflows/ci.yml) builds the host agent, ESP-IDF firmware, Zephyr targets and example WASM apps on code pushes to `main`. Firmware artifacts contain no credentials. The CI secure builds require [signing](SECURITY.md#signing-a-ci-build). Artifacts are retained for 90 days.

The [end-to-end workflow](../.github/workflows/e2e.yml) uses those artifacts and builds its own broken rollback and crash-test images. It runs nightly on the latest successful CI commit unless that commit already passed. To run it manually, select a CI run in Actions or rerun that run's `e2e` job.

## Unit tests and fuzzing

- [Unit tests](../tests/unit.sh) run the C core with ASan and UBSan against an in-memory port. They cover management, OCI, deltas, apps, hardware, console, MQTT, metrics and recovery. The WASM suite also fills and restores the sample FIFO, counts record writes, withholds MQTT receipts and exercises two local apps with the broker offline. Python tests cover publishing and provisioning tools.
- [Fuzz targets](../tests/fuzz.sh) exercise HTTP responses, OCI manifests and signatures, rendered specs, certificates, deltas and console input. CI gives each target 30 s on code pushes. The [nightly workflow](../.github/workflows/fuzz-nightly.yml) defaults to 20 min per target and retains its corpus. Regression inputs live in `tests/fuzz/corpus`.

The FIFO test also checks that peeking, rejecting a stale acknowledgement and pushing to a full queue produce no writes. [Queue capacity](RESULTS.md#ram) · [Write volume and reboot results](APPS.md#persistent-sample-queues)

## End-to-end tests

| Target | What the workflow checks |
|---|---|
| [Host](../tests/e2e/e2e.sh) | Enrollment and approval, status, config, signed app install/update/removal, unsigned app rejection and retry after a registry outage. Exercises resource alerts, gateway metrics, console and Logs, Git-managed fleets and secret-backed registry credentials. |
| Host identity and recovery | Renews a two-minute management certificate, reconnects after a server restart and re-enrolls under a new name after expiry offline. API `FactoryReset` must remove the app as well as the device credentials. |
| Zephyr `native_sim` and Arm `mps2/an385` in QEMU | Run the host fleet scenario with `DEVICE_KIND=zephyr` or `qemu`. Check stored config through logs. The test cannot inspect erased keys in flash or verify config across re-enrollment. CLI `Unenroll` must survive reboot. |
| [ESP-IDF emulator](../tests/e2e/e2e-emu.sh) | C5, S3 and C6, each plain and secure. Install a signed delta and a full image over TLS, confirm the boot slot, reject a mismatched variant and an unsigned app, roll back unreachable firmware, then decommission. Only plain C5 runs certificate renewal. C3 enrollment and OTA are available manually with `CHIP=esp32c3`. |
| [ESP32-C3 battery telemetry](../tests/espidf-hw/battery.py) | With a 2:1 divider, ADC inputs of 1,650 and 1,980 mV gave 3,280 and 3,924 mV. The gateway reported `low_battery` as 1, 0, then 1 on return to 3,280 mV (3,600 mV threshold). The simulated gauge supplied 3,700 mV, +14 mA and 62% charge. The app reported charging. |
| [ESP-IDF crash reporting](../tests/e2e/fatal-emu.sh) | S3 and C5 must report the fault and task after reboot. Decode the PC and caller with the release ELF and check the previous boot log. CI covers panic, abort and interrupt watchdog. Task watchdog is manual (`MODE=twdt`). |

**Provisioning.** Plain ESP-IDF e2e images start with a proof of possession and no network. The test rejects an incorrect proof, supplies the emulated network and enrolls. The [host protocol test](../tests/e2e/prov-host.sh) also checks wrong Wi-Fi credentials, scanning and radios that must close their AP before joining.

**Secure Boot.** Secure emulator runs use disposable signing keys. They check first-boot eFuses, encrypted flash and migration of factory credentials, signed OTA, rejected signatures and variants, anti-rollback and key rotation. The C5 signs with its ECDSA peripheral. S3 and C6 report a storage key. Physical verification limits are in [Device security](SECURITY.md#not-yet-done).

To reuse an existing cluster, run either test with `SKIP_DEPLOY=1`. The emulator must be on `PATH` or in `~/.local/bin`.

```bash
SKIP_DEPLOY=1 tests/e2e/e2e.sh
SKIP_DEPLOY=1 CHIP=esp32c5 tests/e2e/e2e-emu.sh
```

To boot a local C3 build in the emulator, provision a merged image with the emulator's Wi-Fi credentials.

```bash
tools/provision.py --image port/espidf/build-esp32c3/merged-binary.bin --wifi-ssid myssid --wifi-password mypassword
esp-emu --chip esp32c3 --firmware port/espidf/build-esp32c3/merged-binary.bin --net user
```

## Peripheral tests

The [ESP-IDF fixtures](../tests/espidf-hw) call the production drivers. The emulated S3 read I²C and SPI sensors, received a UART reply, detected button edges and generated 1–2 ms PWM pulses. A second boot with the wrong chip-select must fail. The emulated C3 returned calibrated ADC readings of 1,640 and 1,962 mV for inputs of 1,650 and 1,980 mV. Run from `tests/espidf-hw` with esp-emu and its examples installed.

```bash
idf.py -D IDF_TARGET=esp32s3 build merge-bin
python3 run.py build/merged-binary.bin /path/to/esp-emu-1.1.0-examples

idf.py -B build-c3 -D SDKCONFIG=build-c3/sdkconfig -D IDF_TARGET=esp32c3 build merge-bin
python3 run.py --fixture adc build-c3/merged-binary.bin /path/to/esp-emu-1.1.0-examples
```

The Zephyr S3 job builds the MCUboot agent and a separate Simple Boot button fixture. In the emulator, one WASM app read the same button by its devicetree name and by its fleet alias. Build the fixture in the west workspace, then run the second command from `tests/espidf-hw`.

```bash
west build -b esp32s3_devkitc/esp32s3/procpu -d /tmp/button-s3 /path/to/repo/tests/zephyr-apps
python3 run.py --fixture zephyr /tmp/button-s3/zephyr/zephyr.bin /path/to/esp-emu-1.1.0-examples
```

[Zephyr driver tests](../tests/zephyr-hw.sh) exercise GPIO, ADC, PWM, I²C, SPI and UART on `native_sim`. The RS-485 peer checks driver enable through the last transmitted bit, timeouts, close and shared claims. [App runtime tests](../tests/zephyr-apps.sh) cover libc, float formatting and stack usage on `native_sim` and Arm QEMU.

### Catalog app checks

- [`hwcheck`](APPS.md#examples) exercised I²C, SPI, UART, ADC and PWM on physical ESP32, C5, C6 and S3 Mini boards without external peripherals, and on an emulated C3.
- Five boards read die temperature with `alarm`. C6 and Zephyr S3 required the grant for `line_temp`. C6 delivered threshold interrupts. Zephyr S3 polled after `FC_ENOTSUP`.
- `tinyml` and `tinymlp` classified die temperature on ESP-IDF C3, C5, C6 and S3 Mini, and simulated temperature on the physical Zephyr S3.
- All six boards passed `libccheck` (`bad=0`) and reported `battery unavailable` without a pack.

## App benchmark methods

| Workload | Measurement |
|---|---|
| `tinyml`, `tinymlp`, `trend`, `battery` | Standard firmware and 1 ms device clock. Median elapsed time of seven batches after two warm-ups, divided by operations per batch (32 for the neural network, 256 otherwise). Includes preemption and excludes callback entry. Both classifiers processed the same 16 temperature windows and returned every expected class. [Input vectors](../wasm-apps/tinyml/vectors.h) |
| Periodic callback | Elapsed time around the WASM call. Median of 32 callbacks per mode after warm-up, alternating modes in one boot. On classic ESP32, `alarm` disables its timer without a die sensor, so the measurement uses `battery` without a pack. |
| `mqttdemo` command | 48 QoS 1 round trips per board from command to matching application acknowledgement over established TLS connections. |

A separate counting loop executed five million interpreter instructions in 1.20 s on ESP32, 1.06 s on C3, 0.66 s on C5, 1.30 s on C6 and 0.90 s on S3 Mini. [App benchmarks](RESULTS.md#app-benchmarks)

### WAMR callback measurements

The ESP-IDF baseline used the FreeRTOS stack scan. Zephyr used its standard implementation in both groups. [Current callback times](RESULTS.md#app-benchmarks) · [Stack lookup patch](../third_party/patches/wamr-espidf-stack-info.patch)

| Board | Baseline callback (µs) | Time saved by direct stack lookup |
|---|---:|---:|
| ESP32 (`battery`) | 1,329.5 | 21.1% |
| C3 (`alarm`) | 830.5 | 38.1% |
| C5 (`alarm`) | 633.5 | 30.2% |
| C6 (`alarm`) | 676.0 | 43.4% |
| S3 Mini (`alarm`) | 722.5 | 30.5% |
| Zephyr S3 (`alarm`, control) | 431.0 | — |

An instrumented ESP-IDF S3 Mini with direct stack lookup and dispatch in flash ran each catalog workload for 24 samples after warm-up. CPU times include synchronous native calls. Both models received identical inputs after the sensor read.

| S3 Mini workload | Median (ms) |
|---|---:|
| `alarm`, CPU per reading | 0.550 |
| `mqttdemo`, CPU per periodic publish | 2.227 |
| `sampler`, CPU per reading and queue write | 7.787 |
| Sensor reading to `localalarm` callback entry, elapsed | 0.399 |
| `tinyml`, CPU per full callback / after sensor read | 1.575 / 1.146 |
| `tinymlp`, CPU per full callback / after sensor read | 2.550 / 2.116 |

## Registry tokens

The manual [registry test](../tests/registry-bearer.sh) needs Docker, ORAS, OpenSSL, htpasswd with bcrypt, Python 3, CMake, Ninja and mbedTLS 4. It runs `registry:3` with same-host and cross-host Bearer realms. Enable Quay to test a pull-only robot account. Allow about 1 GB for Quay.

```bash
tests/registry-bearer.sh
QUAY=1 tests/registry-bearer.sh
```

Checks include signed multi-chip firmware and WASM, public and private repositories, refused credentials, denied scopes, untrusted CAs and token expiry mid-pull. Output must not contain the password. `QUICK=1` skips the 70 s token-expiry wait. Override ports with `REG_PORT`, `AUTH_PORT`, `PROXY_PORT` and `QUAY_PORT`. [Registry configuration](REGISTRY.md)

## MQTT broker

Run `tests/mqtt-broker.sh` with Docker, OpenSSL, CMake, Ninja and mbedTLS 4. The manual test runs the host client against Mosquitto on plain TCP and TLS. It checks authentication, CA validation, both topic prefixes, QoS 1, oversized messages, keep-alive and resubscription after a broker restart. Allow about two minutes. [Device MQTT configuration](APPS.md#mqtt)

On established MQTT/TLS connections, each ESP-IDF board delivered 96 messages in 8 min at 5 s intervals (longest subscriber gap 5.1 s). Zephyr S3 delivered 270 in 45 min at 10 s intervals. During a separate signed update at a 1 s interval, the broker received 20 messages from the original boot after `Updating`, then messages from the new boot. [Command latency by board](RESULTS.md#app-benchmarks)
