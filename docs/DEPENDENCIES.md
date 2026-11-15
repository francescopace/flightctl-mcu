# Dependencies

What the project is built with and tested against, at which version, and where
each version is set. Everything is pinned in the repository: a build does not
pick up a newer release by itself. Checked against the latest releases on
12 November 2026.

## In the core, on every port

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | 4.1.1 | comes with ESP-IDF and with Zephyr; `MBEDTLS_VERSION` in the workflows for the host builds | The core needs 4.1 or later (`CMakeLists.txt`). Not 4.2.0, the latest: neither ESP-IDF 6.0.3 nor Zephyr 4.5 has it. The host build takes the system's (4.2.0 from Homebrew works). |
| [cJSON](https://github.com/DaveGamble/cJSON) | 1.7.19 | a copy in `third_party/cjson`, unmodified | On every port, ESP-IDF 6 included (it no longer has its own). |
| [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) | 2.4.5 | `third_party/fetch-wamr.sh` | Fetched at build time. |
| [detools](https://github.com/eerimoq/detools) (Python package) | 0.53.1 | `tools/fw-push.py`, the workflow | Makes firmware deltas, on the host only; the device decodes their format with its own code ([DELTA.md](DELTA.md)). |

## Host port (Linux, macOS)

The simulated device, `port/host`: the core above on POSIX, with nothing else
linked in.

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
| [Zephyr](https://github.com/zephyrproject-rtos/zephyr) | 4.5.0 | `third_party/fetch-zephyr.sh` (tag and commit) | Its modules (mbedTLS 4.1.1, TF-PSA-Crypto, MCUboot 2.5.0, Espressif's HAL, picolibc, littlefs, zcbor, CMSIS) at the revisions its manifest names. |
| Zephyr SDK | 1.0.1 | `third_party/fetch-zephyr-sdk.sh` | The ARM and Xtensa (ESP32-S3) toolchains, for the builds for boards only. The same script fetches the ESP32-S3's binary WiFi firmware, at the version its HAL module names. |
| Zephyr's build image | `ci-base` 0.30.0 | `port/zephyr/build.sh`, `port/zephyr/run.sh`, the fetch scripts, the workflows | Also the container of the host and fuzzing jobs: it has clang, lld and ninja. |

## Tested against

| Dependency | Version | Set in | Notes |
|---|---|---|---|
| [Flight Control](https://github.com/flightctl/flightctl) (service and CLI) | 1.4.0 | `tools/kind/up.sh`, `FLIGHTCTL_VERSION` in the workflow | What changed for the devices since 1.3.1 is in [RESULTS.md](RESULTS.md#flight-control-140). |
| Envoy Gateway | 1.2.5 | `tools/kind/up.sh` | The version Flight Control 1.4.0's installation guide names, not the latest. |
| [esp-emulator](https://github.com/espressif/esp-emulator) | 0.51.0 | `ESP_EMU_VERSION` in the workflow | Checked against the release's sha256. |
| OCI registry | `registry:3` | `tools/registry/up.sh` | |
| kubectl image for Flight Control's Helm jobs | `alpine/k8s` 1.37.2 | `tools/kind/cluster-cli/Containerfile` | On arm64 machines only, where `origin-cli` has no image. |

Not pinned: kind, helm and kubectl (the runner's), oras and cosign (the
latest; cosign 2 and 3 both accept the signatures `tools/signing/sign.py`
makes), and the Python packages `esp_prov` needs (protobuf and
cryptography).

## Licenses

- [cJSON](https://github.com/DaveGamble/cJSON) (MIT), vendored in `third_party/cjson`
- [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) (Apache 2.0 with LLVM exception), fetched at build time
- [mbedTLS](https://github.com/Mbed-TLS/mbedtls) (Apache 2.0), from ESP-IDF, Zephyr or your system

## Updating one

Change the version where the table says, then run what covers it:

- core libraries (mbedTLS, cJSON, WAMR): `tests/unit.sh`, `tests/fuzz.sh`,
  `tests/e2e/e2e.sh`, and a firmware build;
- any of them: the SBOM takes the new version by itself ([SBOM.md](SBOM.md)), except the
  mbedTLS of a new ESP-IDF release, which `MBEDTLS_IN_ESP_IDF` in `tools/sbom.py` names;
- ESP-IDF and its components: the firmware builds for every chip and
  `tests/e2e/e2e-emu.sh` (plain and `SECURE=1`, or the end-to-end workflow
  run by hand on the CI run). A new component version
  changes the lock files: commit them;
- Zephyr: `port/zephyr/build.sh` and `DEVICE_KIND=zephyr tests/e2e/e2e.sh`; the build for the
  ESP32-S3 is not in CI ([ZEPHYR.md](ZEPHYR.md));
- Flight Control: all the end-to-end tests. A new minor release can add
  defaults that meet what the devices report (see
  [RESULTS.md](RESULTS.md#flight-control-140)).

mbedTLS, ESP-IDF and Zephyr move together: the core is written for the mbedTLS
that both ports' releases carry.
