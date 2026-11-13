# Devices

What the agent asks of a device, where it runs today, and how to add another one.

## What a device needs

The agent is not tied to a chip or a vendor: it is a C core and a thin port for each system it runs on. What it asks of a device:

- a CPU that [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) has a target for. In use here: Xtensa, RISC-V, Arm Cortex-M0+, and AArch64 and x86-64 on a host;
- about 256 KB of RAM: the smallest board it runs on has 264 KB, and uses nearly all of it;
- flash for two firmware slots and some storage, 2 MB at the least (images are 0.7 to 1.5 MB);
- an IP network with TCP. Nothing in the core is about WiFi, but WiFi is the only link tried on hardware: a Zephyr board with Ethernet is two files ([below](#another-zephyr-board)), not tried;
- a random generator and a way to learn the time (SNTP), for TLS;
- mbedTLS 4.1 or later.

## Where it runs

Three stacks have a port today:

| Stack | Devices | What works there |
|---|---|---|
| **ESP-IDF** 6.0<br/>`port/espidf` | Espressif's WiFi chips. Six run it: ESP32, S2, S3, C3, C5, C6. Another one ESP-IDF supports is mostly configuration. | Everything, and the most complete: also Secure Boot, flash encryption, keys in hardware, WiFi provisioning over SoftAP and BLE ([SECURITY.md](SECURITY.md), [PROVISIONING.md](PROVISIONING.md)). On six boards, and on emulated chips in CI. |
| **Zephyr** 4.5<br/>`port/zephyr` | Any board Zephyr supports that meets the list above: Zephyr brings the drivers, the network stack and MCUboot. Two run it: a Raspberry Pi Pico W (RP2040) and an ESP32-S3-DevKitC. Likely next, not tried: the Raspberry Pi Pico 2 W, boards with Ethernet such as ST's Nucleo-144 (F7, H7) or NXP's i.MX RT kits. | Enrollment, monitoring, config, WASM apps, firmware through MCUboot, whole or as a delta ([ZEPHYR.md](ZEPHYR.md)). On two boards, and as a Linux process (`native_sim`) in CI. A new board is two files. |
| **Linux, macOS**<br/>`port/host` | Anything with POSIX threads and sockets: a gateway, a single-board computer, a container, your laptop. | Runs as a process. Enrollment, monitoring, config and WASM apps are real; a firmware update only saves the image to a file. It is the simulated device of the tests, and where the protocol work was done. A Linux device that can run bootc and Podman should run Flight Control's own agent instead. |

Any other RTOS (NuttX, RT-Thread, ThreadX, FreeRTOS outside ESP-IDF, ...) needs a port, which is about 20 functions: see [Adding another device](#adding-another-device). None is written.

"Everything" means enrollment, monitoring, config, firmware OTA with rollback and WASM apps. The same `.wasm` file runs on every one of these, whatever the CPU.

## Boards tried

With ESP-IDF 5.5.6, firmware v3.0 (both agent connections kept open). The firmware now builds with ESP-IDF 6.0.3 and mbedTLS 4, which were tried on the emulated chips and on two of these boards, the S3 ([RESULTS.md](RESULTS.md#esp-idf-603-on-a-board-esp32-s3)) and the C3 (release v3.1, [RESULTS.md](RESULTS.md#the-registry-with-tls-and-decommissioning-on-boards)); the table is from the earlier run:

| Chip | Core | Status | Free heap with an app running |
|---|---|---|---|
| ESP32-S3 | Xtensa LX7 | everything works | ~118 KB |
| ESP32-C3 | RISC-V | everything works; also tested in the emulator in CI | ~57 KB |
| ESP32-C5 | RISC-V | everything works; also tested in the emulator in CI | ~59 KB |
| ESP32 (classic) | Xtensa LX6 | everything works; temperature is simulated because there is no supported internal sensor | ~73 KB |
| ESP32-C6 | RISC-V | everything except rollback, which was not exercised | ~163 KB |
| ESP32-S2 | Xtensa LX7 | everything works, using the PSRAM in the package (S2FNR2, as on the Lolin S2 Mini): internal RAM alone is not enough for TLS; no serial log on the S2 Mini, because the console is on UART0 | ~28 KB internal + 2 MB PSRAM |

The S2 Mini and the C6 DevKitC flash only over their native USB port, started in download mode (plug them in holding BOOT).

PSRAM is not used, so a single image runs on every board variant of a chip. The exception is the S2, which needs its PSRAM (see the table).

With ESP-IDF 6.0.3 (release v3.1, debug build) the firmware images are 1,133 to 1,330 KB, leaving 20–32% of the 1,664 KB OTA slot free, and 1,502 KB with BLE provisioning built in (C3), which leaves 10% ([RESULTS.md](RESULTS.md#firmware-size)). Most of that size is the WiFi stack and cryptography. The agent itself is about 31 KB (45 KB with cJSON, detools and its ESP-IDF port; C3, debug build) and WAMR about 68 KB.

With Zephyr 4.5 and firmware updates through MCUboot ([ZEPHYR.md](ZEPHYR.md)):

| Board | Core | Status | Free heap with an app running |
|---|---|---|---|
| Raspberry Pi Pico W (RP2040) | Arm Cortex-M0+ | everything works | ~31 KB |
| ESP32-S3 | Xtensa LX7 | everything works | ~49 KB |

## Adding another device

### Another ESP32 chip

If ESP-IDF supports the chip, it is mostly configuration:

1. Build for it: `TARGET=esp32c61 FW_VERSION=v1 port/espidf/build.sh`. The build directory is `port/espidf/build-<target>`.
2. Put board-specific defaults in `port/espidf/sdkconfig.defaults.<target>`. For example the LED pin: see `sdkconfig.defaults.esp32c3` or `sdkconfig.defaults.esp32`.
3. If the chip has no temperature sensor, `port/espidf` already falls back to a simulated one (`SOC_TEMP_SENSOR_SUPPORTED`).
4. `tools/fw-push.py` picks up every `build-*` directory, so the chip ends up in the next firmware index on its own.

Per-device differences, such as a different LED pin on a clone board, don't need a separate build. Set a label on the device (`led-gpio=38`, `led-order=rgb`, `led-ws2812=false`) and the fleet template passes it into the config.

### Another Zephyr board

If Zephyr supports the board and it meets [the list above](#what-a-device-needs), it is two files in `port/zephyr/boards/`, as for the two boards that are there:

1. `<board>.overlay`: the `fc_creds` (16 KB) and `fc_storage` partitions, usually carved out of the board's storage partition, and the nodes Zephyr leaves off (the WiFi chip, the random generator).
2. `<board>.conf`: the network driver, MCUboot's image manager, and the memory the board can give: the kernel heap (thread stacks come from it), the C heap, the agent's stacks.
3. Build with `BOARD=<board> port/zephyr/build.sh`, write the credentials with `tools/provision.py --format zephyr --board <board files>`. Firmware updates need MCUboot: the script builds with it for a board named `.../mcuboot` and for Espressif's, and for any other with `MCUBOOT=1`.

A CPU family that is new here needs its WAMR target named in `port/zephyr/CMakeLists.txt`, and a chip without a random generator needs an entropy driver (the Pico W has one in `port/zephyr`). What each of the two boards took is in [ZEPHYR.md](ZEPHYR.md).

### Another RTOS (NuttX, RT-Thread, ThreadX, ...)

The core needs:

- POSIX threads (mutex, condition variable, threads with a configurable stack);
- BSD sockets, reachable through `mbedtls_net_*`;
- `time()` that returns the real time (certificates are validated);
- mbedTLS 4.1 or later (TLS 1.3; its cryptography is PSA only);
- a platform layer for WAMR. WAMR already has one for Zephyr, NuttX, RT-Thread and RIOT, among others.

Then implement `core/include/fc_port.h` for your platform:

| Function | What it does |
|---|---|
| `fc_port_kv_get/set/del` | Persistent key/value storage: device key, certificate, config files, WASM modules |
| `fc_port_sysinfo`, `fc_port_custom_info` | Hardware and OS facts for `status.systemInfo` |
| `fc_port_write_file`, `fc_port_read_file` | Where `spec.config` files end up, and reading them back (the registry's CA and credentials, [REGISTRY.md](REGISTRY.md)) |
| `fc_port_fw_platform` | The `platform.architecture` to pick from a firmware index, e.g. `esp32c3` |
| `fc_port_ota_begin/write/end` | Write an image into the inactive slot and make it the next boot image |
| `fc_port_ota_pending_verify/confirm/rollback`, `fc_port_reboot` | Confirm a new image or go back. On Zephyr this is MCUboot's test and confirm. |
| `fc_port_fw_read` | Read the running image, for [delta updates](DELTA.md). A port that cannot returns -1, and its updates are whole images. |
| `fc_port_hwkey_bits/seal/pk/forget` | A device key kept in hardware ([SECURITY.md](SECURITY.md)). A port without one says so, and the key stays in storage. |
| `fc_port_log` | Logging |

`port/host/fc_port_host.c` (250 lines), `port/zephyr/fc_port_zephyr.c` (480) and `port/espidf/fc_port_espidf.c` (1000, with the keys in hardware) are the three references. The firmware wiring in `port/espidf/main/` covers WiFi, time sync, the demo app and its host functions; it is ESP-IDF specific and would be rewritten for another platform.
