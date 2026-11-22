# Devices

## Hardware compatibility

| Cell | Meaning |
|---|---|
| ✓ | The board enrolled in Flight Control and ran a signed app on this port. Each check links to its hardware evidence. Text after the mark is the board variant or link recorded for that check. |
| Build profile | The cell links to the port's build profile for this module. |
| Unverified | No check is recorded for this port on this module. |
| n/a | Not applicable to this port. |

| Chip or module | CPU | On-chip SRAM (KB) | ESP-IDF | Zephyr | Apache NuttX |
|---|---|---:|---|---|---|
| ESP32 | Xtensa LX6 | 520 | [✓](BENCHMARKS.md#catalog-apps) | [✓](BENCHMARKS.md#managed-devices) | [✓ rev. 1.0 lab profile](BENCHMARKS.md#managed-devices) |
| ESP32-C3 | RISC-V | 400 | [✓](BENCHMARKS.md#catalog-apps) | [✓](BENCHMARKS.md#managed-devices) | [✓](BENCHMARKS.md#managed-devices) |
| ESP32-C5 | RISC-V | 384 | [✓](BENCHMARKS.md#catalog-apps) | [✓ Mini, 4 MiB](BENCHMARKS.md#managed-devices) | [✓ Mini, 4 MiB](NUTTX.md#esp32-c5-mini) |
| ESP32-C6 | RISC-V | 512 | [✓](BENCHMARKS.md#catalog-apps) | [✓](BENCHMARKS.md#managed-devices) | [✓](BENCHMARKS.md#managed-devices) |
| ESP32-S3 Mini N4R2 | Xtensa LX7 | 512 | [✓](BENCHMARKS.md#s3-mini-n4r2) | [✓](BENCHMARKS.md#managed-devices) | [✓](BENCHMARKS.md#managed-devices) |
| ESP32-S3 DevKitC N8R8 | Xtensa LX7 | 512 | [✓](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board) | [✓](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board) | [✓](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board) |
| ESP32-S3 N16R8 | Xtensa LX7 | 512 | [Build profile](ESP-IDF.md#build-profiles) | Unverified | [✓](NUTTX.md#verified-ota-on-the-n16r8) |
| ST NUCLEO-F767ZI | Cortex-M7 | 512 | n/a | [✓ RJ45 and ST-LINK USB](ZEPHYR.md#on-an-st-nucleo-f767zi) | [✓ RJ45 and ST-LINK USB](NUTTX.md#arm-boards) |
| Nordic nRF7002-DK | Cortex-M33 | 512 | n/a | [✓ Wi-Fi](ZEPHYR.md#on-a-nordic-nrf7002-dk) | [✓ J-Link USB](BENCHMARKS.md#managed-devices) |
| Raspberry Pi Pico 2 W | Cortex-M33 | 520 | n/a | [✓ Wi-Fi](ZEPHYR.md#on-a-raspberry-pi-pico-2-w) | [✓ Wi-Fi](NUTTX.md#arm-boards) |

The Nordic SRAM figure is the application core's.

### App checks on hardware

The six boards are those of the [catalog benchmarks](BENCHMARKS.md#catalog-apps): ESP32, C3, C5, C6 and the S3 Mini on ESP-IDF and the S3 DevKitC N8R8 on Zephyr.

| Check | Catalog app | Boards | Result |
|---|---|---|---|
| I²C, SPI, UART, ADC and PWM without external peripherals | [`hwcheck`](APPS.md#examples) | Physical ESP32, C5, C6 and S3 Mini. Emulated C3 | Exercised |
| Die temperature | `alarm` | Five boards | Read. C6 and Zephyr S3 required the grant for `line_temp`. C6 delivered threshold interrupts. Zephyr S3 polled after `FC_ENOTSUP`. |
| Classification of die temperature | `tinyml`, `tinymlp` | ESP-IDF C3, C5, C6 and S3 Mini | Classified |
| Classification of simulated temperature | `tinyml`, `tinymlp` | Physical Zephyr S3 | Classified |
| C library | `libccheck` | All six | `bad=0` |
| Battery without a pack | `battery` | All six | `battery unavailable` |

Physical temperature on Zephyr, read by the signed `trend:v0.9.5` app against a hardware reference:

| Board | Firmware | Reference | Reference reads | Reference range (°C) | App reading (°C) | App after OTA (°C) |
|---|---|---|---:|---:|---:|---:|
| NUCLEO-F767ZI | v0.9.6 | SWD reads on ADC1 channel 18 at 963–966 counts, at nominal 3.3 V using factory calibration | 8 | 32.25–33.21 | 33.3–33.5 | — |
| nRF7002-DK | 0.9.5+nrf.5 | SWD reads of the network core TEMP register over 27 s | 6 | 26.75–27.00 | 26.8–26.9 | — |
| Pico 2 W | v0.9.5 | ADC channel 4 on RP2350 | — | — | 32.2 | 33.4 |

On the Nucleo the app reported its range while its smoothed reading was falling. On the Nordic the app readings cover the same interval as the reference. On the Pico the app resumed after OTA at the last value.

[How the checks run](TESTING.md#peripheral-tests)

## Release images and OTA

The signed v0.9.7 index contains 23 platform images, all built from one commit. The catalog apps are published under the same version, and each module is byte-identical to its v0.9.5 one. The S3 suffix gives flash and PSRAM in MiB (N4R2 = 4 MiB flash, 2 MiB PSRAM). [Release digests and source commit](../tools/kind/releases/v0.9.7.json)

| Port | Images | Published profiles | Hardware OTA evidence |
|---|---:|---|---|
| [ESP-IDF](ESP-IDF.md) | 6 | ESP32, C3, C5, C6, S3, S3-r8 | [Signed updates and rollback](DELTA-UPDATES.md#what-was-verified) |
| [Zephyr](ZEPHYR.md) | 10 | S3 DevKitC, S3 Mini, ESP32, C3, C5 Mini, C6, Nucleo (RJ45 and ST-LINK), Nordic DK, Pico 2 W | [Board results](ZEPHYR.md#memory-and-ota-on-hardware) |
| [Apache NuttX](NUTTX.md) | 7 | S3 N4R2, N8R8, N16R8, Nucleo (RJ45 and ST-LINK), Nordic DK, Pico 2 W | [N16R8 trial boot confirmed](NUTTX.md#verified-ota-on-the-n16r8). [Arm OTA results](NUTTX.md#signed-firmware-updates-on-arm). ESP32, C3, C5 and C6 use Simple Boot and have no image in the index. |

<details>
<summary>Application clocks by published profile (MHz)</summary>

| MCU or board | ESP-IDF (MHz) | Zephyr (MHz) | NuttX (MHz) |
|---|---:|---:|---:|
| ESP32 DevKitC | 240 | — | — |
| ESP32-C3 | 160 | — | — |
| ESP32-C5 | 240 | — | — |
| ESP32-C6 | 160 | — | — |
| ESP32-S3 | 240 | 240 | 240 |
| Raspberry Pi Pico 2 W (RP2350) | — | 150 | — |
| ST NUCLEO-F767ZI | — | 216 | — |
| Nordic nRF7002-DK (nRF5340 application core) | — | 128 | — |

Each value is the chip's rated maximum. A dash means no clock is given for that profile. The Nucleo and Nordic clocks were checked on hardware with their v0.9.6 images.

</details>

## What a device needs

- A CPU supported by WAMR, at least 384 KB SRAM and enough free heap for the intended apps. [ESP-IDF heap](ESP-IDF.md#runtime-memory) · [Zephyr RAM](ZEPHYR.md#memory-and-ota-on-hardware) · [NuttX memory](NUTTX.md#memory-and-flash)
- Flash for firmware, credentials and data. OTA with rollback requires two firmware slots.
- TCP/IP, a random generator and a source of real time for TLS. The verified network paths are Wi-Fi, the Nucleo's onboard Ethernet or ST-LINK USB bridge, and the Nordic DK's J-Link USB bridge on NuttX.

| Port and layout | Flash needed (MiB) | Firmware slots | Slot size (KiB) |
|---|---:|---:|---:|
| [ESP-IDF](../port/espidf/partitions.csv) | 4 | — | — |
| [Zephyr S3](../port/zephyr/boards/esp32s3_devkitc_procpu.overlay) | 8 | — | — |
| [Zephyr Pico 2 W](ZEPHYR.md#on-a-raspberry-pi-pico-2-w) | 4 | 2 | 1,472 |
| NuttX Simple Boot | — | 1 | — |
| [NuttX MCUboot](NUTTX.md#firmware-ota-with-mcuboot), all three S3 profiles | — | 2 | — |

A dash means this page does not give the figure. NuttX Simple Boot keeps its one image in the first 4 MiB.

## Nucleo Ethernet verification

Both agents enrolled over the onboard RJ45 port and ran the signed `trend:v0.9.5` app. After a power cycle, each reconnected with its original enrollment identity and a new boot ID. Flight Control reported `Online` and `UpToDate`, the remote console responded and `trend` resumed. [Zephyr results](ZEPHYR.md#on-an-st-nucleo-f767zi) · [NuttX results](NUTTX.md#arm-boards)

The Nucleo also confirmed signed NuttX firmware updates and automatically rolled back an unhealthy candidate over ST-LINK USB. The Nordic DK and Pico 2 W passed the same OTA checks. [Arm OTA results](NUTTX.md#signed-firmware-updates-on-arm)

## Simulation

| Target | RAM | Architecture | Agent port |
|---|---|---|---|
| Linux / macOS host | Host RAM | Host CPU | POSIX |
| `native_sim` | Host RAM | Host CPU | Zephyr |
| MPS2 AN385 | 384 KB test budget | Arm Cortex-M3 | Zephyr |

## Adding another device

### Another ESP32 chip

If ESP-IDF supports the chip, it is mostly configuration:

1. Build for it: `TARGET=esp32c61 FW_VERSION="${NEW_TAG:?set the new firmware tag}" port/espidf/build.sh`. The build directory is `port/espidf/build-<target>`.
2. Put board-specific defaults in `port/espidf/sdkconfig.defaults.<target>`. For example the LED pin: see `sdkconfig.defaults.esp32c3` or `sdkconfig.defaults.esp32`.
3. On chips without an ESP-IDF die-temperature driver, including the original ESP32, `die_temp` returns `FC_ENOTSUP`. The agent omits `customInfo.temperatureC` and the `device.temperature` metric when it has no measurement.
4. `tools/fw-push.py` picks up every `build-*` directory, so the chip ends up in the next firmware index on its own.

Per-device differences, such as a different LED pin on a clone board, don't need a separate build. Set a label on the device (`led-gpio=38`, `led-order=rgb`, `led-ws2812=false`) and the fleet template passes it into the config.

### Another Zephyr board

If Zephyr supports the board and it meets [the list above](#what-a-device-needs), start with its configuration in `port/zephyr/boards/`:

1. `<board>.overlay`: the `fc_creds` (16 KB) and `fc_storage` partitions, usually carved out of the board's storage partition, and the nodes Zephyr leaves off (the WiFi chip, the random generator).
2. `<board>.conf`: the network driver, MCUboot's image manager, and the memory the board can give: the kernel heap (thread stacks come from it), the C heap, the agent's stacks.
3. Build with `BOARD=<board> port/zephyr/build.sh`, write the credentials with `tools/provision.py --format zephyr --board <board files>`. Firmware updates need MCUboot: the script builds with it for a board named `.../mcuboot` and for Espressif's, and for any other with `MCUBOOT=1`.
4. For OTA, add the board’s `CONFIG_FC_FW_PLATFORM` (the board name by default) to `ZEPHYR_BOARD_MODULES` in [tools/sbom.py](../tools/sbom.py). List its board-specific modules, or use an empty tuple if the common modules cover it. Pass the same platform name to `tools/fw-push.py` as `PLATFORM=path/to/zephyr.signed.bin`.

A CPU family that is new here needs its WAMR target named in `port/zephyr/CMakeLists.txt`, and a chip without a random generator needs an entropy driver. What the ESP32-S3 took is in [ZEPHYR.md](ZEPHYR.md).

### Another RTOS (RT-Thread, ThreadX, ...)

The core needs:

- POSIX threads (mutex, condition variable, threads with a configurable stack);
- BSD sockets, reachable through `mbedtls_net_*`;
- `time()` that returns the real time (certificates are validated);
- mbedTLS 4.1.1 (TLS 1.3; its cryptography is PSA only);
- a platform layer for WAMR. WAMR already has one for Zephyr, NuttX, RT-Thread and RIOT, among others.

Implement the [port interface](../core/include/fc_port.h). It covers persistent keys and config files, status and resource measurements, OTA slots and rollback, hardware keys and logging. A port without `fc_port_fw_read` support returns -1 and downloads full images.

Use the [host](../port/host/fc_port_host.c), [Zephyr](../port/zephyr/fc_port_zephyr.c) and [ESP-IDF](../port/espidf/fc_port_espidf.c) implementations as references. Network setup, time sync and physical peripheral drivers belong in the port.
