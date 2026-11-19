# Tests and CI

[CI](../.github/workflows/ci.yml) builds the host agent, ESP-IDF firmware, Zephyr targets and example WASM apps on code pushes to `main`. Firmware artifacts contain no credentials. The CI secure builds require [signing](SECURITY.md#signing-a-ci-build). Artifacts are retained for 90 days.

The [end-to-end workflow](../.github/workflows/e2e.yml) uses those artifacts and builds its own broken rollback and crash-test images. It runs nightly on the latest successful CI commit unless that commit already passed. To run it manually, select a CI run in Actions or rerun that run's `e2e` job.

## Unit tests and fuzzing

- [Unit tests](../tests/unit.sh) run the C core with ASan and UBSan against an in-memory port. They cover management, OCI, deltas, apps, hardware, console, MQTT, metrics and recovery. The WASM suite also fills and restores the sample FIFO, counts record writes, withholds MQTT receipts and exercises two local apps with the broker offline. Python tests cover publishing and provisioning tools.
- [Fuzz targets](../tests/fuzz.sh) exercise HTTP responses, OCI manifests and signatures, rendered specs, certificates, deltas and console input. CI gives each target 30 s on code pushes. The [nightly workflow](../.github/workflows/fuzz-nightly.yml) defaults to 20 min per target and retains its corpus. Regression inputs live in `tests/fuzz/corpus`.

## End-to-end tests

| Target | What the workflow checks |
|---|---|
| [Host](../tests/e2e/e2e.sh) | Enrollment and approval, status, config, signed app install/update/removal, unsigned app rejection and retry after a registry outage. Exercises resource alerts, gateway metrics, console and Logs, Git-managed fleets and secret-backed registry credentials. |
| Host identity and recovery | Renews a two-minute management certificate, reconnects after a server restart and re-enrolls under a new name after expiry offline. API `FactoryReset` must remove the app as well as the device credentials. |
| Zephyr `native_sim` and Arm `mps2/an385` in QEMU | Run the host fleet scenario with `DEVICE_KIND=zephyr` or `qemu`. Check stored config through logs. The test cannot inspect erased keys in flash or verify config across re-enrollment. CLI `Unenroll` must survive reboot. |
| [ESP-IDF emulator](../tests/e2e/e2e-emu.sh) | C5, S3 and C6, each plain and secure. Install a signed delta and a full image over TLS, confirm the boot slot, reject a mismatched variant and an unsigned app, roll back unreachable firmware, then decommission. Only plain C5 runs certificate renewal. C3 enrollment and OTA are available manually with `CHIP=esp32c3`. |
| [ESP32-C3 battery telemetry](../tests/espidf-hw/battery.py) | The signed `battery` app reads a virtual ADC divider and Smart Battery gauge. The gateway must export `low_battery` as 1, 0, then 1 across a 3,600 mV threshold. The I²C sample must export 3,700 mV, 62% charge and active charging. |
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

The [ESP-IDF fixtures](../tests/espidf-hw) call the production drivers. The S3 fixture reads I²C and SPI sensors, checks a UART reply, detects button edges and measures PWM. A second boot with the wrong chip-select must fail. The C3 fixture checks calibrated ADC readings. Run from `tests/espidf-hw` with esp-emu and its examples installed.

```bash
idf.py -D IDF_TARGET=esp32s3 build merge-bin
python3 run.py build/merged-binary.bin /path/to/esp-emu-1.1.0-examples

idf.py -B build-c3 -D SDKCONFIG=build-c3/sdkconfig -D IDF_TARGET=esp32c3 build merge-bin
python3 run.py --fixture adc build-c3/merged-binary.bin /path/to/esp-emu-1.1.0-examples
```

The Zephyr S3 job builds the MCUboot agent and a separate Simple Boot button fixture. Build the fixture in the west workspace, then run the second command from `tests/espidf-hw`.

```bash
west build -b esp32s3_devkitc/esp32s3/procpu -d /tmp/button-s3 /path/to/repo/tests/zephyr-apps
python3 run.py --fixture zephyr /tmp/button-s3/zephyr/zephyr.bin /path/to/esp-emu-1.1.0-examples
```

[Zephyr driver tests](../tests/zephyr-hw.sh) exercise GPIO, ADC, PWM, I²C, SPI and UART on `native_sim`. The RS-485 peer checks driver enable through the last transmitted bit, timeouts, close and shared claims. [App runtime tests](../tests/zephyr-apps.sh) cover libc, float formatting and stack usage on `native_sim` and Arm QEMU. For physical boards, use [hwcheck](APPS.md#examples). [Recorded peripheral results](RESULTS.md#apps-on-the-boards)

## Registry tokens

The manual [registry test](../tests/registry-bearer.sh) needs Docker, ORAS, OpenSSL, htpasswd with bcrypt, Python 3, CMake, Ninja and mbedTLS 4. It runs `registry:3` with same-host and cross-host Bearer realms. Enable Quay to test a pull-only robot account. Allow about 1 GB for Quay.

```bash
tests/registry-bearer.sh
QUAY=1 tests/registry-bearer.sh
```

Checks include signed multi-chip firmware and WASM, public and private repositories, refused credentials, denied scopes, untrusted CAs and token expiry mid-pull. Output must not contain the password. `QUICK=1` skips the 70 s token-expiry wait. Override ports with `REG_PORT`, `AUTH_PORT`, `PROXY_PORT` and `QUAY_PORT`. [Registry configuration](REGISTRY.md)

## MQTT broker

Run `tests/mqtt-broker.sh` with Docker, OpenSSL, CMake, Ninja and mbedTLS 4. The manual test runs the host client against Mosquitto on plain TCP and TLS. It checks authentication, CA validation, both topic prefixes, QoS 1, oversized messages, keep-alive and resubscription after a broker restart. Allow about two minutes. [Device MQTT configuration](APPS.md#mqtt)
