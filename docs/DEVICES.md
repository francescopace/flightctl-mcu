# Devices

## What a device needs

- A CPU supported by WAMR, at least 384 KB SRAM and enough free heap for the intended apps. [RAM measurements](RESULTS.md#ram)
- Flash for two OTA slots, credentials and data. The [ESP-IDF layout](../port/espidf/partitions.csv) needs 4 MiB. The [Zephyr S3 layout](../port/zephyr/boards/esp32s3_devkitc_procpu.overlay) needs 8 MiB.
- TCP/IP, a random generator and a source of real time for TLS. Physical boards use Wi-Fi. Ethernet has been tested on Arm in QEMU.

## Devices and ports

Boards marked **HW** are tested on physical hardware for every release. **Simulation** covers host processes and QEMU. Hardware tests for the three boards on order are pending. [Board results](RESULTS.md#what-works) · [Planned hardware tests](ROADMAP.md#blocked-on-hardware-availability)

| MCU / target | SRAM | Architecture | Agent port | Test Level |
|---|---|---|---|---|
| ESP32 | 520 KB | Xtensa LX6 | ESP-IDF | HW |
| ESP32-C3 | 400 KB | 32-bit RISC-V | ESP-IDF | HW |
| ESP32-C5 | 384 KB | 32-bit RISC-V | ESP-IDF | HW |
| ESP32-C6 | 512 KB | 32-bit RISC-V | ESP-IDF | HW |
| ESP32-S3 | 512 KB | Xtensa LX7 | ESP-IDF | HW |
| ESP32-S3 | 512 KB | Xtensa LX7 | Zephyr | HW |
| Linux / macOS host | Host RAM | Host CPU | POSIX | Simulation |
| `native_sim` | Host RAM | Host CPU | Zephyr | Simulation |
| MPS2 AN385 | 384 KB test budget | Arm Cortex-M3 | Zephyr | Simulation |
| ST NUCLEO-F767ZI  | 512 KB | Arm Cortex-M7 | Zephyr | HW pending |
| Raspberry Pi Pico 2 W  | 520 KB | Arm Cortex-M33 or RISC-V Hazard3 | Zephyr | HW pending |
| Nordic nRF7002-DK  | 512 KB on app core | Arm Cortex-M33 | Zephyr | HW pending |

## Adding another device

### Another ESP32 chip

If ESP-IDF supports the chip, it is mostly configuration:

1. Build for it: `TARGET=esp32c61 FW_VERSION="${NEW_TAG:?set the new firmware tag}" port/espidf/build.sh`. The build directory is `port/espidf/build-<target>`.
2. Put board-specific defaults in `port/espidf/sdkconfig.defaults.<target>`. For example the LED pin: see `sdkconfig.defaults.esp32c3` or `sdkconfig.defaults.esp32`.
3. On chips without a temperature sensor (`SOC_TEMP_SENSOR_SUPPORTED` is false), ESP-IDF reports a simulated temperature in `customInfo`. WASM reads of `die_temp` return `FC_ENOTSUP`.
4. `tools/fw-push.py` picks up every `build-*` directory, so the chip ends up in the next firmware index on its own.

Per-device differences, such as a different LED pin on a clone board, don't need a separate build. Set a label on the device (`led-gpio=38`, `led-order=rgb`, `led-ws2812=false`) and the fleet template passes it into the config.

### Another Zephyr board

If Zephyr supports the board and it meets [the list above](#what-a-device-needs), start with its configuration in `port/zephyr/boards/`:

1. `<board>.overlay`: the `fc_creds` (16 KB) and `fc_storage` partitions, usually carved out of the board's storage partition, and the nodes Zephyr leaves off (the WiFi chip, the random generator).
2. `<board>.conf`: the network driver, MCUboot's image manager, and the memory the board can give: the kernel heap (thread stacks come from it), the C heap, the agent's stacks.
3. Build with `BOARD=<board> port/zephyr/build.sh`, write the credentials with `tools/provision.py --format zephyr --board <board files>`. Firmware updates need MCUboot: the script builds with it for a board named `.../mcuboot` and for Espressif's, and for any other with `MCUBOOT=1`.
4. For OTA, add the board’s `CONFIG_FC_FW_PLATFORM` (the board name by default) to `ZEPHYR_BOARD_MODULES` in [tools/sbom.py](../tools/sbom.py). List its board-specific modules, or use an empty tuple if the common modules cover it. Pass the same platform name to `tools/fw-push.py` as `PLATFORM=path/to/zephyr.signed.bin`.

A CPU family that is new here needs its WAMR target named in `port/zephyr/CMakeLists.txt`, and a chip without a random generator needs an entropy driver. What the ESP32-S3 took is in [ZEPHYR.md](ZEPHYR.md).

### Another RTOS (NuttX, RT-Thread, ThreadX, ...)

The core needs:

- POSIX threads (mutex, condition variable, threads with a configurable stack);
- BSD sockets, reachable through `mbedtls_net_*`;
- `time()` that returns the real time (certificates are validated);
- mbedTLS 4.1.1 (TLS 1.3; its cryptography is PSA only);
- a platform layer for WAMR. WAMR already has one for Zephyr, NuttX, RT-Thread and RIOT, among others.

Implement the [port interface](../core/include/fc_port.h). It covers persistent keys and config files, status and resource measurements, OTA slots and rollback, hardware keys and logging. A port without `fc_port_fw_read` support returns -1 and downloads full images.

Use the [host](../port/host/fc_port_host.c), [Zephyr](../port/zephyr/fc_port_zephyr.c) and [ESP-IDF](../port/espidf/fc_port_espidf.c) implementations as references. Network setup, time sync and physical peripheral drivers belong in the port.
