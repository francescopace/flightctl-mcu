# Dependencies

Build and test versions checked on 18 November 2026, the NuttX port and the Zephyr modules on 22 November 2026. For containers following a release series, the tables include the tested image version.

## Release support

The 1.0 development builds use Flight Control 1.4.0 and ESP-IDF 6.1.1. From 1.0 onward, consult this page in the release you deploy for its verified pair. Compatibility lasts until the next agent release. Fixes target the current release.

## In the core, on every port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 4.1.1 | supplied by ESP-IDF and Zephyr, fetched as a release archive for NuttX, checked in `core/include/fc_crypto.h` on every port and in `CMakeLists.txt` for host builds | Host and both SDKs use TF-PSA-Crypto 1.1.1. |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | a copy in `third_party/cjson`, unmodified | On every port, ESP-IDF 6 included (it no longer has its own). |
| [WAMR](https://github.com/wasm-micro-runtime/wasm-micro-runtime) | 2.4.6 | `WAMR_TAG` in `third_party/fetch-wamr.sh` | Fetched and patched at build time. Fast interpreter with instruction metering and tail calls. ESP-IDF, NuttX and 32-bit Zephyr MCU builds use [compact dispatch](WAMR.md#interpreter-dispatch). AOT, JIT, WASI, SIMD and reference types are disabled. [App libc](APPS.md#the-c-library) |

The core implements HTTP, MQTT, OTLP encoding, field provisioning and the firmware delta decoder.

## Host port (Linux, macOS)

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

For the Pico 2 W, setting `BOARD=rpi_pico2/rp2350a/m33_0/w/mcuboot` (or `rpi_pico2/rp2350a/m33_0/w` for the standalone build) when fetching Zephyr adds the Raspberry Pi and Infineon HAL modules. The SDK setup fetches the CYW43439 Wi-Fi firmware named by the Infineon module. The Zephyr fetch script patches the GPIO driver to send both the pin mask and the output value required by the chip’s `gpioout` command. [Pico 2 W setup](ZEPHYR.md#on-a-raspberry-pi-pico-2-w)

For the Nordic DK, set `BOARD=nrf7002dk/nrf5340/cpuapp` for both fetch scripts. This selects Nordic’s HAL, Wi-Fi driver and firmware, the WPA supplicant and the OpenAMP and libmetal modules for inter-core communication. [Nordic DK setup](ZEPHYR.md#on-a-nordic-nrf7002-dk)

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Zephyr](https://github.com/zephyrproject-rtos/zephyr) | 4.5.0 | `third_party/fetch-zephyr.sh` (tag, commit and `ZEPHYR_MODULES`) | Only the modules the firmware needs, listed below, at the revisions Zephyr's own `west.yml` names: the project has no manifest of its own. |
| Zephyr SDK | 1.0.1 | `third_party/fetch-zephyr-sdk.sh` | The ARM and Xtensa (ESP32-S3) toolchains, for the builds for boards only. The same script fetches the ESP32-S3's binary WiFi firmware, at the version its HAL module names. |
| Zephyr's build image | `ci-base` 0.30.0 | `port/zephyr/build.sh`, `port/zephyr/run.sh`, the fetch scripts, the workflows | Also the container of the host and fuzzing jobs: it has clang, lld and ninja. |
| [gvproxy](https://github.com/containers/gvisor-tap-vsock) | 0.9.0 | Not pinned by a script. The install command in [the Nucleo setup](ZEPHYR.md#on-an-st-nucleo-f767zi) names it. | Host-side network for the ST-LINK and J-Link serial bridges, on Zephyr and NuttX. `port/zephyr/stlink-network.py` uses its default addresses. |

Zephyr modules for every board. A revision is the one `west.yml` names in Zephyr 4.5.0.

| Zephyr module | Version | Notes |
|---|---|---|
| mbedTLS | 4.1.1 | |
| TF-PSA-Crypto | 1.1.1 | |
| MCUboot | 2.5.0 | Its `scripts/imgtool.py` signs the Zephyr images. |
| Espressif HAL (`hal_espressif`) | revision `cc81db00` | Names the version of the ESP32-S3's binary WiFi firmware. |
| [picolibc](https://github.com/picolibc/picolibc) | 1.8.10 | The C library, with `CONFIG_PICOLIBC_IO_FLOAT=y` in `port/zephyr/prj.conf`: the apps' `printf` and `snprintf` format each conversion with it, and `%f` needs it. |
| littlefs | 2.11 (API version) | |
| zcbor | 0.9.1 | |
| CMSIS | revision `512cc7e8` | |
| CMSIS 6 | revision `1c1840af` | |

## NuttX port

[Build and provisioning](NUTTX.md#build-and-provisioning)

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Apache NuttX](https://github.com/apache/nuttx) and [nuttx-apps](https://github.com/apache/nuttx-apps) | 13.1.0 (tag `nuttx-13.1.0`) | `port/nuttx/build.sh` | Every board except the Pico 2 W. The build compares the checkout with the tag and applies the patches in `port/nuttx/`. One starts the NTP exchange after DHCP without requiring a gateway ping. NuttX fetches the board HAL and LittleFS at the revisions it names. |
| Apache NuttX and nuttx-apps for the Pico 2 W | commits `d50b7688` and `7196658d` | `port/nuttx/build.sh` | The build compares the checkouts with the full commit hashes. |
| mbedTLS | 4.1.1 | `port/nuttx/build.sh` | Release archive, checked against its SHA-256 digest. |
| Espressif Xtensa toolchain | the one in `espressif/idf:v6.0.3` | Not pinned by a script. The image the S3 builds ran in. | The RISC-V builds use the Espressif `riscv32-esp-elf-` toolchain when it is on PATH. |
| Espressif HAL for the C5 (`esp-hal-3rdparty`) | revision `b90b1837` | `ESP_HAL_3RDPARTY_VERSION` in `port/nuttx/build.sh` | Patched from `port/nuttx/c5/`. |
| MCUboot for the S3 | the revision NuttX 13.1.0 names | `make bootloader` in `port/nuttx/build.sh` with `OTA=1` | `port/nuttx/mcuboot-build.patch` changes how NuttX builds the bootloader, not MCUboot itself. |
| CYW43439 firmware for the Pico 2 W | `w43439A0_7_95_49_00_combined.h` at cyw43-driver `055d6427` | `port/nuttx/build-pico2w.sh` | Checked against its SHA-256 digest. |
| Arm GNU toolchain (`arm-none-eabi-gcc`) | the build host's | Not pinned by a script. | Pico 2 W, NUCLEO-F767ZI and nRF7002-DK. |
| imgtool | 2.2.0 | Not pinned by a script. Install it in the build environment. | Signs the MCUboot images in `port/nuttx/build.sh` with `OTA=1` and in `port/nuttx/sign-arm.sh`. |

The serial bridges for the NUCLEO-F767ZI and nRF7002-DK use gvproxy, listed under the [Zephyr port](#zephyr-port).

## Tested against

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Flight Control](https://github.com/flightctl/flightctl) (service and CLI) | 1.4.0 | `tools/kind/up.sh`, `FLIGHTCTL_VERSION` in the workflow | The fleet maps only stable device fields to labels. [Label sync](FLIGHTCTL-LIMITATIONS.md#11-a-change-in-the-status-is-a-label-event) |
| Envoy Gateway | 1.2.8 | `tools/kind/up.sh` | Patch release in the 1.2 series used by Flight Control 1.4.0’s installation guide. |
| [esp-emulator](https://github.com/espressif/esp-emulator) | 1.2.0 | `ESP_EMU_VERSION` in `ci.yml` and `e2e.yml` | The emulator and its virtual peripherals are checked against the release's SHA-256 sums. |
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

The nRF7002-DK build embeds Nordic’s Wi-Fi firmware. Its license permits use with Nordic integrated circuits and covers software updates for products using those chips. J-Link is a separate host tool for USB programming. Distributing the SEGGER library in a tool bundle requires prior written authorization. [Nordic firmware license](https://github.com/zephyrproject-rtos/nrf_wifi/blob/fc0872109ec4c006d32843a202b129db33cf16b9/zephyr/LICENSE.txt) · [SEGGER terms](https://www.segger.com/products/debug-probes/j-link/tools/terms-of-use/)
