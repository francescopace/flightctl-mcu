# Dependencies

What the project is built with and tested against, at which version, and where each version is set. Everything is pinned in the repository: a build does not pick up a newer release by itself. Checked against the latest releases on 12 November 2026.

## In the core, on every port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 4.1.1 | comes with ESP-IDF and with Zephyr; `MBEDTLS_VERSION` in the workflows for the host builds | The core needs 4.1 or later (`CMakeLists.txt`). Not 4.2.0, the latest: neither ESP-IDF 6.0.3 nor Zephyr 4.5 has it. The host build takes the system's ([below](#host-port-linux-macos)). |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | a copy in `third_party/cjson`, unmodified | On every port, ESP-IDF 6 included (it no longer has its own). |
| [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) | 2.4.5 | `WAMR_TAG` in `third_party/fetch-wamr.sh` | Fetched at build time into `third_party/wamr`, then patched with `third_party/patches/wamr-*.patch` (seven patches, none upstream; the script's header says what each fixes). Built as the fast interpreter only: no AOT, JIT, WASI, SIMD or reference types; instruction metering and tail calls on. Its libc-builtin is built in on every port, but the core registers only the functions of the apps' [C library](APPS.md#the-c-library) (`s_libc` in `core/src/fc_apps.c`), some of them its own. |

Written in the core, with no library besides mbedTLS and BSD sockets: the firmware-delta decoder (detools' sequential format with heatshrink, `core/src/fc_delta.c`, [DELTA-UPDATES.md](DELTA-UPDATES.md)), the MQTT 3.1.1 client for the apps (`core/src/fc_mqtt.c`), the OTLP metrics encoding (`core/src/fc_metrics.c`), HTTP/1.1 and HTTP/2 (`core/src/fc_http.c`) and the provisioning protocol (`core/src/fc_prov.c`). Neither ESP-IDF's nor Zephyr's MQTT library is used.

## Host port (Linux, macOS)

The simulated device, `port/host`: the core above on POSIX, with nothing else linked in.

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| mbedTLS, installed on the system | 4.1 or later, with `MBEDTLS_THREADING_C` | `find_package(MbedTLS 4.1)` in `CMakeLists.txt` | CI builds 4.1.1 from source, the version the devices have (`.github/actions/mbedtls`), with the threading layer on, and links it statically. On macOS, Homebrew's (4.2.0 today), which has it. The core refuses to build without it (`core/include/fc_crypto.h`): its threads share PSA's key table. |
| CMake, Ninja, a C11 compiler, pthreads | CMake 3.16 or later | `CMakeLists.txt` | In CI they come with Zephyr's build image (below), clang as the compiler; Apple clang works. |
| clang with libFuzzer and the sanitizers | the build image's | `tests/unit.sh`, `tests/fuzz.sh` | For the tests only. |
| clang with the `wasm32` target, `wasm-ld` | the build image's | `wasm-apps/build.sh` | For the example WASM apps, on any port. |

## ESP-IDF port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | 6.0.3 | the `espressif/idf:v6.0.3` image: `port/espidf/build.sh`, `tools/provision.py`, the workflow | Not 6.1, the latest: its mbedTLS is 4.1.0, without the security fixes of July 2026. To revisit when a 6.1.x has 4.1.1 or later. |
| `esp_prov`, from Espressif's [network_provisioning](https://components.espressif.com/components/espressif/network_provisioning) | 1.4.0 | `PROV_VERSION` in `tests/e2e/esp-prov.sh` | The installer's side in the tests only: `tests/e2e/esp-prov.sh` fetches it, checked against a sha256. The firmware serves the protocol with its own code (`core/src/fc_prov.c`). |
| esptool | 5.3.1 | the ESP-IDF image; `.github/workflows/e2e.yml` for the emulator jobs, which run outside it | The version ESP-IDF 6.0.3 comes with, not the latest: one esptool builds and signs everywhere. The scripts use the command names of esptool 5 (`sign-data`, `merge-bin`). |
| esp-idf-nvs-partition-gen | 0.3.0 | `.github/workflows/e2e.yml` | For `tools/provision.py`; without it, the script uses the one in the ESP-IDF image. |

## Zephyr port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Zephyr](https://github.com/zephyrproject-rtos/zephyr) | 4.5.0 | `third_party/fetch-zephyr.sh` (tag, commit and `ZEPHYR_MODULES`) | Only the modules the firmware needs (mbedTLS 4.1.1, TF-PSA-Crypto, MCUboot 2.5.0, Espressif's HAL, picolibc, littlefs, zcbor, CMSIS and CMSIS 6), at the revisions Zephyr's own `west.yml` names: the project has no manifest of its own. |
| [picolibc](https://github.com/picolibc/picolibc) | Zephyr 4.5's module | `port/zephyr/prj.conf` | The C library, with `CONFIG_PICOLIBC_IO_FLOAT=y`: the apps' `printf` and `snprintf` format each conversion with it, and `%f` needs it. |
| Zephyr SDK | 1.0.1 | `third_party/fetch-zephyr-sdk.sh` | The ARM and Xtensa (ESP32-S3) toolchains, for the builds for boards only. The same script fetches the ESP32-S3's binary WiFi firmware, at the version its HAL module names. |
| Zephyr's build image | `ci-base` 0.30.0 | `port/zephyr/build.sh`, `port/zephyr/run.sh`, the fetch scripts, the workflows | Also the container of the host and fuzzing jobs: it has clang, lld and ninja. |

## Tested against

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Flight Control](https://github.com/flightctl/flightctl) (service and CLI) | 1.4.0 | `tools/kind/up.sh`, `FLIGHTCTL_VERSION` in the workflow | Its label sync is narrowed for these devices ([FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md#label-sync)). |
| Envoy Gateway | 1.2.5 | `tools/kind/up.sh` | The version Flight Control 1.4.0's installation guide names, not the latest. |
| [esp-emulator](https://github.com/espressif/esp-emulator) | 0.51.0 | `ESP_EMU_VERSION` in the workflow | Checked against the release's sha256. |
| OCI registry | `registry:3` | `tools/registry/up.sh` | |
| kubectl image for Flight Control's Helm jobs | `alpine/k8s` 1.37.2 | `tools/kind/cluster-cli/Containerfile` | On arm64 machines only, where `origin-cli` has no image. |
| MQTT broker: [mosquitto](https://github.com/eclipse-mosquitto/mosquitto) | `eclipse-mosquitto:2.0.22` | `tests/mqtt-broker.sh`, `tools/mqtt/up.sh` | Plain and TLS, with a password ([APPS.md](APPS.md#mqtt)). |
| Registry with token authentication: [docker_auth](https://github.com/cesanta/docker_auth) | `cesanta/docker_auth:1.14.0`, with `registry:3` and `nginx:1.27-alpine` | `tests/registry-bearer.sh` | The token service in front of the registry ([REGISTRY.md](REGISTRY.md)). |
| [Quay](https://github.com/quay/quay) | `quay.io/projectquay/quay:v3.17.3`, with `postgres:15-alpine` and `redis:7-alpine` | `QUAY_IMAGE` in `tests/registry-bearer.sh` | With `QUAY=1` only: a robot account's token. |
| [Trustify](https://github.com/guacsec/trustify) | 0.4.12 | `TRUSTIFY_VERSION` in `tools/trustify/up.sh` | Flight Control's vulnerability reporting; the API version its client takes ([SBOM.md](SBOM.md#in-flight-control)). Its release binary, checked against the sha256 GitHub gives, in an `ubuntu:24.04` image. |
| Git server for the fleet from Git | `alpine:3.20` | `tests/e2e/e2e.sh` | |

No Prometheus server is needed: `tests/e2e/e2e.sh` reads the metrics from the Telemetry Gateway's own Prometheus endpoint.

## Tools

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [detools](https://github.com/eerimoq/detools) (Python package) | 0.53.1 | `DETOOLS_VERSION` in `tools/fw-push.py`; `.github/workflows/e2e.yml` | Makes the firmware deltas, on the host only. `tools/fw-push.py --delta-from` installs it in `build/detools-venv` when the Python it runs on lacks it. `tests/unit/gen-delta-vectors.py` makes the unit tests' deltas (`tests/unit/delta_vectors.h`) with it. The device decodes them with its own code (above). |
| Python 3 | the system's | the scripts in `tools/` and `tests/` | The standard library, plus PyYAML for `tests/e2e/e2e.sh`, `tools/fleet/mqtt-config.py` and `tools/auto-approve`. |

Not pinned: kind, helm and kubectl (the runner's); Docker; oras (the latest, `oras-project/setup-oras` in CI); cosign (the one `sigstore/cosign-installer` v4.1.3 installs in CI; cosign 2 and 3 both accept the signatures `tools/signing/sign.py` makes); the `openssl` CLI (`tools/signing/sign.py`, the test certificates); `gh` (`tools/trustify/up.sh` reads the release's sha256 with it); the Python packages `esp_prov` needs (protobuf and cryptography); opentelemetry-proto, only to regenerate `tests/unit/otlp_vectors.h` with `tests/unit/gen-otlp-vectors.py`. `tests/registry-bearer.sh` and `tools/mqtt/up.sh` name the machine through [nip.io](https://nip.io), so they need its DNS.

## Licenses

In the firmware, as each release's SBOM lists them ([SBOM.md](SBOM.md)):

- [cJSON](https://github.com/DaveGamble/cJSON): MIT, vendored in `third_party/cjson`;
- [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime): Apache 2.0 with LLVM exception, fetched at build time and patched;
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) and TF-PSA-Crypto: Apache 2.0 or GPL 2.0 or later, at your choice; from ESP-IDF, Zephyr or your system;
- [ESP-IDF](https://github.com/espressif/esp-idf): Apache 2.0;
- [Zephyr](https://github.com/zephyrproject-rtos/zephyr), MCUboot and zcbor: Apache 2.0; littlefs and picolibc: BSD 3-Clause; the ESP32-S3's binary WiFi firmware: Apache 2.0 (`zephyr/blobs/license.txt` in Espressif's HAL module).

Only run, not shipped: detools (BSD 2-Clause), esptool (GPL 2.0), mosquitto (EPL 2.0 or EDL 1.0), Flight Control, esp-emulator, docker_auth, Quay, Trustify, oras and cosign (Apache 2.0).

## Updating one

Change the version where the table says, then run what covers it:

- core libraries (mbedTLS, cJSON, WAMR): `tests/unit.sh`, `tests/fuzz.sh`, `tests/e2e/e2e.sh`, and a firmware build;
- WAMR: `third_party/fetch-wamr.sh` stops if a patch no longer applies; check each against the new release, drop the ones it has taken upstream, and run `tests/zephyr-apps.sh` too, also with `BOARD=mps2/an385` (the apps' C library on Zephyr; the second in QEMU, on a Cortex-M);
- detools: change it in `tools/fw-push.py` and in the workflow, regenerate `tests/unit/delta_vectors.h` with `tests/unit/gen-delta-vectors.py`, and run `tests/unit.sh`: `core/src/fc_delta.c` reads the format of 0.53;
- mosquitto: `tests/mqtt-broker.sh`; docker_auth and Quay: `tests/registry-bearer.sh` (and `QUAY=1`);
- Trustify: only with Flight Control, whose client names the API it takes;
- any of them: the SBOM takes the new version by itself ([SBOM.md](SBOM.md)), except the mbedTLS of a new ESP-IDF release, which `MBEDTLS_IN_ESP_IDF` in `tools/sbom.py` names;
- ESP-IDF and its components: the firmware builds for every chip and `tests/e2e/e2e-emu.sh` (plain and `SECURE=1`, or the end-to-end workflow run by hand on the CI run). A new component version changes the lock files: commit them;
- Zephyr: `port/zephyr/build.sh` and `DEVICE_KIND=zephyr tests/e2e/e2e.sh`; CI also builds for the ESP32-S3 and runs `DEVICE_KIND=qemu` ([ZEPHYR.md](ZEPHYR.md));
- Flight Control: all the end-to-end tests. A new minor release can add defaults that meet what the devices report (see [FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md#label-sync)).

mbedTLS, ESP-IDF and Zephyr move together: the core is written for the mbedTLS that both ports' releases carry.
