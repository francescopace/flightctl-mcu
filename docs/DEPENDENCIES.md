# Dependencies

Versions used to build and test the agent, checked on 18 November 2026. The tables name the source of each version. For test containers that follow a release series, they also give the version reported by the tested image.

## Release support

Starting with 1.0, each agent release lists the exact Flight Control and ESP-IDF versions verified for its ESP-IDF firmware in this document. Consult the copy that ships with your agent release. The project maintains compatibility with that pair until the next agent release. Fixes target the current release, with no commitment to backport them to earlier releases.

The development builds for 1.0 use Flight Control 1.4.0 and ESP-IDF 6.1.1.

## In the core, on every port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 4.1.1 | supplied by ESP-IDF and Zephyr, checked in `core/include/fc_crypto.h` on every port and in `CMakeLists.txt` for host builds | ESP-IDF 6.1.1 and Zephyr 4.5.0 both carry 4.1.1. ESP-IDF’s `release/v6.2` and `master` and Zephyr’s `main` also report 4.1.1. Host and both SDKs use TF-PSA-Crypto 1.1.1. |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | a copy in `third_party/cjson`, unmodified | On every port, ESP-IDF 6 included (it no longer has its own). |
| [WAMR](https://github.com/wasm-micro-runtime/wasm-micro-runtime) | 2.4.6 | `WAMR_TAG` in `third_party/fetch-wamr.sh` | Fetched at build time into `third_party/wamr`, then patched with `third_party/patches/wamr-*.patch` (the script describes each patch). Built as the fast interpreter only: no AOT, JIT, WASI, SIMD or reference types; instruction metering and tail calls on. Its libc-builtin table retains the 15 WAMR wrappers used on the device, but the core registers only the functions of the apps' [C library](APPS.md#the-c-library) (`s_libc` in `core/src/fc_apps.c`), some of them its own. |

Written in the core, with no library besides mbedTLS and BSD sockets: the firmware-delta decoder (detools' sequential format with heatshrink, `core/src/fc_delta.c`, [DELTA-UPDATES.md](DELTA-UPDATES.md)), the MQTT 3.1.1 client for the apps (`core/src/fc_mqtt.c`), the OTLP metrics encoding (`core/src/fc_metrics.c`), HTTP/1.1 and HTTP/2 (`core/src/fc_http.c`) and the provisioning protocol (`core/src/fc_prov.c`). Neither ESP-IDF's nor Zephyr's MQTT library is used.

## Host port (Linux, macOS)

The simulated device, `port/host`: the core above on POSIX, with nothing else linked in.

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| mbedTLS | 4.1.1 with `MBEDTLS_THREADING_C` | `find_package(MbedTLS 4.1.1 EXACT)` in `CMakeLists.txt` | Run `third_party/build-mbedtls-host.sh` to install it in `build/mbedtls-install`. CI uses the same script and release checksum. PSA’s shared key table needs pthread locking. |
| CMake, Ninja, a C11 compiler, pthreads | CMake 3.16 or later | `CMakeLists.txt` | In CI they come with Zephyr's build image (below), clang as the compiler; Apple clang works. |
| clang with libFuzzer and the sanitizers | the build image's | `tests/unit.sh`, `tests/fuzz.sh` | For the tests only. |
| clang with the `wasm32` target, `wasm-ld` | the build image's | `wasm-apps/build.sh` | For the example WASM apps, on any port. |

## ESP-IDF port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | 6.1.1 | the `espressif/idf:v6.1.1` image: `port/espidf/build.sh`, `tools/provision.py`, the workflow | Includes Espressif’s mbedTLS 4.1.1. |
| `esp_prov`, from Espressif's [network_provisioning](https://components.espressif.com/components/espressif/network_provisioning) | 1.4.0 | `PROV_VERSION` in `tests/e2e/esp-prov.sh` | The installer's side in the tests only: `tests/e2e/esp-prov.sh` fetches it, checked against a sha256. The firmware serves the protocol with its own code (`core/src/fc_prov.c`). |
| esptool | 5.6.0 | the ESP-IDF image; `.github/workflows/e2e.yml` for the emulator jobs, which run outside it | ESP-IDF 6.1.1 ships this version for building and signing. The scripts use the command names of esptool 5 (`sign-data`, `merge-bin`). |
| esp-idf-nvs-partition-gen | 0.3.0 | `.github/workflows/e2e.yml` | The emulator jobs install 0.3.0 from PyPI. The Docker fallback in `tools/provision.py` uses 0.1.9 from the ESP-IDF 6.1.1 image. |

## Zephyr port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Zephyr](https://github.com/zephyrproject-rtos/zephyr) | 4.5.0 | `third_party/fetch-zephyr.sh` (tag, commit and `ZEPHYR_MODULES`) | Only the modules the firmware needs (mbedTLS 4.1.1, TF-PSA-Crypto 1.1.1, MCUboot 2.5.0, Espressif's HAL, picolibc, littlefs 2.11 (API version), zcbor, CMSIS and CMSIS 6), at the revisions Zephyr's own `west.yml` names: the project has no manifest of its own. |
| [picolibc](https://github.com/picolibc/picolibc) | 1.8.10 (Zephyr module) | `port/zephyr/prj.conf` | The C library, with `CONFIG_PICOLIBC_IO_FLOAT=y`: the apps' `printf` and `snprintf` format each conversion with it, and `%f` needs it. |
| Zephyr SDK | 1.0.1 | `third_party/fetch-zephyr-sdk.sh` | The ARM and Xtensa (ESP32-S3) toolchains, for the builds for boards only. The same script fetches the ESP32-S3's binary WiFi firmware, at the version its HAL module names. |
| Zephyr's build image | `ci-base` 0.30.0 | `port/zephyr/build.sh`, `port/zephyr/run.sh`, the fetch scripts, the workflows | Also the container of the host and fuzzing jobs: it has clang, lld and ninja. |

## Tested against

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Flight Control](https://github.com/flightctl/flightctl) (service and CLI) | 1.4.0 | `tools/kind/up.sh`, `FLIGHTCTL_VERSION` in the workflow | Its label sync is narrowed for these devices ([FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md#label-sync)). |
| Envoy Gateway | 1.2.8 | `tools/kind/up.sh` | Patch release in the 1.2 series used by Flight Control 1.4.0’s installation guide. |
| [esp-emulator](https://github.com/espressif/esp-emulator) | 1.1.0 | `ESP_EMU_VERSION` in `ci.yml` and `e2e.yml` | The emulator and its virtual peripherals are checked against the release's SHA-256 sums. |
| OCI registry | `registry:3` | `tools/registry/up.sh` | The tested image reports Distribution 3.1.2. |
| kubectl image for Flight Control's Helm jobs | `alpine/k8s` 1.37.2 | `tools/kind/cluster-cli/Containerfile` | On arm64 machines only, where `origin-cli` has no image. |
| MQTT broker: [mosquitto](https://github.com/eclipse-mosquitto/mosquitto) | `eclipse-mosquitto:2.1.4-alpine` | `tests/mqtt-broker.sh`, `tools/mqtt/up.sh` | Plain and TLS, with a password ([APPS.md](APPS.md#mqtt)). |
| Registry with token authentication: [docker_auth](https://github.com/cesanta/docker_auth) | `cesanta/docker_auth:1.14.0`, with `registry:3` and `nginx:1.30-alpine` | `tests/registry-bearer.sh` | The tested nginx image reports 1.30.7. [Registry setup](REGISTRY.md) |
| [Quay](https://github.com/quay/quay) | `quay.io/projectquay/quay:3.18.3` (multi-architecture image) | `QUAY_IMAGE` in `tests/registry-bearer.sh` | `QUAY=1` tests a robot account against Quay, using `postgres:18-alpine` (18.7) and `redis:8-alpine` (8.10.3). |
| [Trustify](https://github.com/guacsec/trustify) | 0.4.21 | `TRUSTIFY_VERSION` in `tools/trustify/up.sh` | Flight Control 1.4.0 reads the v2 API from the Trustify 0.4 series ([SBOM.md](SBOM.md#in-flight-control)). Its release binary, checked against the sha256 GitHub gives, in an `ubuntu:24.04` image. The bundled PostgreSQL links to ICU 74 and libxml2.so.2. |
| Git server for the fleet from Git | `alpine:3.24` | `tests/e2e/e2e.sh` | |

No Prometheus server is needed: `tests/e2e/e2e.sh` reads the metrics from the Telemetry Gateway's own Prometheus endpoint.

## Tools

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [detools](https://github.com/eerimoq/detools) (Python package) | 0.53.1 | `DETOOLS_VERSION` in `tools/fw-push.py`; `.github/workflows/e2e.yml` | Optional host tool. `tools/fw-push.py --delta-from` uses it to generate OTA deltas and installs the pinned version in `build/detools-venv` if needed. Regenerating the unit test vectors also uses it. The firmware applies deltas with its own [C decoder](../core/src/fc_delta.c). |
| ORAS CLI | 1.4.0 in CI | `oras-project/setup-oras@v2` in `.github/workflows/e2e.yml` | The workflow inherits the [action default at the time of verification](https://github.com/oras-project/setup-oras/blob/ac735097e22fe643015b0d19645f7c9a69942128/action.yml). |
| cosign CLI | 3.1.4 in CI | `sigstore/cosign-installer@v4.1.3` in `.github/workflows/e2e.yml` | Checks signatures produced by `tools/signing/sign.py`. [Installer default](https://github.com/sigstore/cosign-installer/blob/v4.1.3/action.yml) |
| Python 3 | the system's | the scripts in `tools/` and `tests/` | The standard library, plus PyYAML for `tests/e2e/e2e.sh`, `tools/fleet/mqtt-config.py` and `tools/auto-approve`. |

The runner supplies kind, Helm, kubectl, Docker, the `openssl` CLI for signing and test certificates, and `gh` for retrieving Trustify release checksums. The provisioning tests use protobuf and cryptography. Regenerating `tests/unit/otlp_vectors.h` requires opentelemetry-proto. `tests/registry-bearer.sh` and `tools/mqtt/up.sh` name the machine through [nip.io](https://nip.io), so they need its DNS.

## Licenses

In the firmware, as each release's SBOM lists them ([SBOM.md](SBOM.md)):

- [cJSON](https://github.com/DaveGamble/cJSON): MIT, vendored in `third_party/cjson`;
- [WAMR](https://github.com/wasm-micro-runtime/wasm-micro-runtime): Apache 2.0 with LLVM exception, fetched at build time and patched;
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) and TF-PSA-Crypto: Apache 2.0 or GPL 2.0 or later, at your choice; from ESP-IDF, Zephyr or the pinned host build;
- [ESP-IDF](https://github.com/espressif/esp-idf): Apache 2.0;
- [Zephyr](https://github.com/zephyrproject-rtos/zephyr), MCUboot and zcbor: Apache 2.0; littlefs and picolibc: BSD 3-Clause; the ESP32-S3's binary WiFi firmware: Apache 2.0 (`zephyr/blobs/license.txt` in Espressif's HAL module).