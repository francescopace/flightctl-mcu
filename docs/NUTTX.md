# Apache NuttX

The Espressif, Nucleo and Nordic builds use Apache NuttX 13.1.0. The Pico 2 W uses a [separately pinned NuttX kernel and apps revision](DEPENDENCIES.md#nuttx-port). All use mbedTLS 4.1.1 and the project's patched WAMR fast interpreter. On the S3, the CPU runs at 240 MHz. NuttX places the user heap in PSRAM and the kernel heap in internal SRAM. The agent reports the user heap.

| S3 profile | Board | Flash (MiB) | PSRAM (MiB) | PSRAM type |
|---|---|---:|---:|---|
| N4R2 | ESP32-S3 Mini | 4 | 2 | Quad |
| N8R8 | DevKit | 8 | 8 | Octal |
| N16R8 | DevKit | 16 | 8 | Octal |

[Hardware matrix](DEVICES.md#hardware-compatibility) · [Managed app benchmarks](BENCHMARKS.md#managed-devices) · [Standalone Cortex-M benchmarks](BENCHMARKS.md#standalone-runtime) · [Versions](DEPENDENCIES.md#nuttx-port) · [Not verified](#not-verified)

## Build and provisioning

| Build host | Requirement |
|---|---|
| Shell | Linux, with the Espressif Xtensa toolchain on PATH |
| Environment the build was run in | `espressif/idf:v6.0.3` with the IDF environment exported |
| Tools | GNU Make, CMake, Ninja, Git, curl, kconfig-frontends, genromfs, xxd and esptool |
| Fetched by the build | NuttX, its applications, the board HAL, LittleFS and mbedTLS |
| Build directory | A dedicated one for the NuttX and applications trees |

Set `BOARD=` from the table below and pass the same name as `--board` to the provisioning tool. The N8R8 profile clocks the flash at 80 MHz and octal PSRAM at 40 MHz.

| Board | `BOARD=` | Toolchain | Network | Console | Boot | Firmware offset |
|---|---|---|---|---|---|---|
| ESP32-S3 Mini N4R2 | `esp32s3-n4r2` (default) | Espressif Xtensa | Wi-Fi | USB Serial/JTAG | Simple Boot, or MCUboot with `OTA=1` | `0x0`, or the [primary slot](#firmware-ota-with-mcuboot) |
| ESP32-S3 DevKit N8R8 | `esp32s3-n8r8` | Espressif Xtensa | Wi-Fi | USB Serial/JTAG | Simple Boot, or MCUboot with `OTA=1` | `0x0`, or the [primary slot](#firmware-ota-with-mcuboot) |
| ESP32-S3 DevKit N16R8 | `esp32s3-n16r8` | Espressif Xtensa | Wi-Fi | USB Serial/JTAG | Simple Boot, or MCUboot with `OTA=1` | `0x0`, or the [primary slot](#firmware-ota-with-mcuboot) |
| ESP32, revision 3.0 or later | `esp32-devkitc` | Espressif Xtensa | Wi-Fi | UART | Simple Boot | `0x1000` |
| ESP32, revision 1.0 | `esp32-devkitc-rev1` | Espressif Xtensa | Wi-Fi | UART | Simple Boot | `0x1000` |
| ESP32-C3 | `esp32c3-devkit` | Espressif RISC-V | Wi-Fi | USB Serial/JTAG | Simple Boot | `0x0` |
| ESP32-C5 Mini | `esp32c5-devkitc` | Espressif RISC-V | Wi-Fi | USB Serial/JTAG | Simple Boot | `0x2000` |
| ESP32-C6 | `esp32c6-devkitc` | Espressif RISC-V | Wi-Fi | USB Serial/JTAG | Simple Boot | `0x0` |
| Raspberry Pi Pico 2 W | `pico2w` | Arm GNU (`arm-none-eabi-gcc`) | Wi-Fi (CYW43439) | — | MCUboot | `0x10020000` |
| ST NUCLEO-F767ZI | `nucleo-f767zi` | Arm GNU (`arm-none-eabi-gcc`) | ST-LINK virtual serial port | — | MCUboot | `0x08040000` |
| ST NUCLEO-F767ZI | `nucleo-f767zi-eth` | Arm GNU (`arm-none-eabi-gcc`) | Onboard RJ45, DHCP | — | MCUboot | `0x08040000` |
| Nordic nRF7002-DK | `nrf7002dk` and `nrf7002dk-net` | Arm GNU (`arm-none-eabi-gcc`) | J-Link USB serial bridge | — | MCUboot | `0x00010000` |

Simple Boot is the image format without MCUboot: one image and no second firmware slot. A dash means this guide does not name a console. Arm offsets are CPU flash addresses.

| Espressif flash region | Offset | Size | Contents |
|---|---|---|---|
| Factory partition | `0x2fc000` | 16 KiB | FCC1 format: the tag `FCC1`, a length and a JSON object, written by `tools/provision.py --format nuttx` |
| LittleFS | `0x300000` through `0x3fffff` | 1 MiB | The device identity, downloaded applications and managed files |

All profiles keep the same factory and LittleFS offsets within the first 4 MiB.

1. Build. The default output is `build/nuttx/nuttx/nuttx.bin`. Set `NUTTX_WORK` to change the build directory.

```sh
third_party/fetch-wamr.sh
port/nuttx/build.sh
```

2. Prepare a factory image using the deployment CA, enrollment credentials, signing public key and Wi-Fi settings, then flash both. Set `WIFI_SSID` and `WIFI_PASSWORD` for factory Wi-Fi, or use the same local Wi-Fi configuration as the ESP-IDF build.

```sh
tools/provision.py --format nuttx --out factory.bin \
    --server "${AGENT_ENDPOINT}" --labels "${DEVICE_LABELS}"
esptool --chip esp32s3 --port "${PORT}" write-flash \
    0x0 build/nuttx/nuttx/nuttx.bin 0x2fc000 factory.bin
```

- This layout replaces an existing ESP-IDF or Zephyr installation.
- NuttX mounts LittleFS at boot and formats its storage partition on the first mount of an unformatted volume.
- The same signing key verifies the catalog applications on all three MCU ports.

### ESP32, ESP32-C3 and ESP32-C6

- All three use the factory and LittleFS regions described above. The default build directory is `build/nuttx-<chip>`.
- These profiles run portable WASM workloads with Simple Boot. The C3 and C6 expose the on-chip temperature sensor.
- `esp32-devkitc-rev1` is for laboratory testing on a revision 1.0 board. This profile runs one CPU at 240 MHz with internal SRAM and uses the upstream `CONFIG_ESP32_IGNORE_CHIP_REVISION_CHECK` option for older silicon. This board enrolled over Wi-Fi and completed CoreMark, Richards and DeltaBlue on one boot. [Measured performance](BENCHMARKS.md#managed-devices)
- On the C3 and the classic rev. 1.0 profile, the agent waits for a successful NTP sample at startup. The NTP task then exits. Both profiles keep the AES lookup tables in flash and limit the Wi-Fi and network buffer pools to leave space in internal SRAM for a 64 KiB WASM linear memory.

### ESP32-C5 Mini

| Item | On the ESP32-C5 Mini |
|---|---|
| Build | `BOARD=esp32c5-devkitc port/nuttx/build.sh` |
| Board and SoC support for NuttX 13.1.0 | Supplied by this repository |
| Flash on the tested Mini | 4 MiB, DIO |
| Crystal on the tested Mini (MHz) | 48 |
| CPU clock (MHz) | 240 |
| Clock check | `cat /proc/cpuinfo` reports `240.000` MHz |
| Provisioning | `--format nuttx --board esp32c5-devkitc` |
| Firmware | `build/nuttx-esp32c5/nuttx/nuttx.bin`, starting at **`0x2000`** |
| Factory partition and LittleFS | The same offsets as on the S3 |

Its Wi-Fi and network buffer limits leave internal SRAM for a 64 KiB WASM linear memory.

### Arm boards

| Board | Enrolls over | Die temperature source | Signed `trend` reading (°C) |
|---|---|---|---:|
| Pico 2 W | The CYW43439 Wi-Fi interface | RP2350 die sensor | 38.8 |
| NUCLEO-F767ZI | Onboard Ethernet or its ST-LINK virtual serial port | ADC1 channel 18, using the factory calibration | 35.1 |
| nRF7002-DK | Its J-Link USB serial bridge | Network core, over RPMsg | — |

The Pico and the Nucleo installed signed WASM applications from a private registry and reported their results to Flight Control. The nRF7002-DK ran signed CoreMark, Richards and DeltaBlue apps on hardware. Its network core also supplies hardware entropy to the application core over RPMsg. [Benchmark measurements and firmware hashes](benchmarks/nuttx-managed-cortex-m.json)

Use an Arm GNU embedded toolchain with `arm-none-eabi-gcc` on PATH. Pass the `BOARD=` name to `tools/provision.py --format nuttx --out factory.bin` as well.

- **Pico 2 W.** It needs Wi-Fi credentials.
- **Nucleo over ST-LINK.** Run `port/zephyr/stlink-network.py --serial <ST-LINK-serial> --link ipv4 --gvproxy <path-to-gvproxy>` on the USB host and keep that process running.
- **Nucleo over onboard Ethernet.** Build with `BOARD=nucleo-f767zi-eth port/nuttx/build.sh` and use `nucleo-f767zi-eth` for provisioning and signing. This profile requests a DHCP lease on `eth0` through the LAN8742A PHY. It shares the MCUboot slot and factory layout with the ST-LINK profile. The signed Ethernet image passed verification against the bootloader public key.
- **nRF7002-DK.** Build the two cores separately with `BOARD=nrf7002dk` and `BOARD=nrf7002dk-net`. Provision the application core with `--board nrf7002dk` and start the IPv4 bridge with `--pace-uart`.

Over RJ45, firmware `0.9.6+136` on the Nucleo enrolled, reached `Online` and `UpToDate`, and ran the signed `trend:v0.9.5` app from the private registry. After a power cycle, the agent reconnected with the same identity and a new boot ID. The remote console responded and `trend` resumed, reporting 32.9 °C compared with 34.5 °C before shutdown.

| Board | MCUboot application slot (KiB) | FCC1 factory partition | Persistent agent data |
|---|---:|---|---|
| Pico 2 W | 1472 | `0x10300000` | LittleFS from `0x10304000` |
| NUCLEO-F767ZI | 768 | `0x08020000` | NuttX config storage at `0x08010000`, two 32 KiB sectors |
| nRF7002-DK | 944 | `0x000fc000` | 1 MiB LittleFS at offset `0x200000` in external SPI flash |

- The slot starts at the firmware offset in the [board table](#build-and-provisioning).
- The factory partition is 16 KiB, on the Nucleo the first 16 KiB of its sector.
- Package an image with `port/nuttx/sign-arm.sh <board> <nuttx.bin>`, with `MCUBOOT_SIGNING_KEY` set to the key trusted by the board’s MCUboot. The script writes a confirmed image for direct installation in the primary slot and a compact signed image for OTA.
- Provisioning NuttX on a previously managed Zephyr board requires a backup of its identity and application data before repurposing the storage.

### Signed firmware updates on Arm

1. Install MCUboot and an OTA-capable NuttX baseline on the board before its first fleet update.
2. Sign the application with the key trusted by that bootloader and publish it under the board's platform name. For example, for the Pico 2 W:

```sh
FW_VERSION=1.0.1 MCUBOOT_SIGNING_KEY=/path/to/mcuboot-signing.pem \
    port/nuttx/sign-arm.sh pico2w /path/to/nuttx.bin
python3 tools/fw-push.py registry.example.com/flightctl-mcu/fw:v1.0.1 \
    nuttx-pico2w=/path/to/nuttx.ota.bin
```

| Board | Platform name |
|---|---|
| Pico 2 W | `nuttx-pico2w` |
| nRF7002-DK | `nuttx-nrf7002dk` |
| NUCLEO-F767ZI, ST-LINK | `nuttx-nucleo-f767zi` |
| NUCLEO-F767ZI, Ethernet | `nuttx-nucleo-f767zi-eth` |

The Nordic image updates the nRF5340 application core. The network core is installed separately over J-Link.

On the device:

1. The agent checks the board name in the protected MCUboot metadata.
2. It writes and reads back the secondary slot, then requests a trial boot.
3. MCUboot verifies the image signature.
4. The agent confirms the trial after a successful management status update.
5. A trial that cannot reach the server reboots automatically and MCUboot restores the previous image.

| Board | Update transport | Confirmed OTA image (B) | Signed update | Delta of the update (B) | Automatic rollback | Delta of the rolled-back candidate (B) |
|---|---|---:|---|---:|---|---:|
| Nordic nRF7002-DK | J-Link USB serial | 693,175 | `+111` → `+112` | — | `+113` → `+112` | — |
| ST NUCLEO-F767ZI | ST-LINK USB serial | 658,249 | `+111` → `+112` | — | `+113` → `+112` | — |
| Raspberry Pi Pico 2 W | Wi-Fi | 969,719 | `+121` → `+122` | 10,077 | `+123` → `+122` | 14,959 |

Build numbers in the table are relative to version `0.9.6`. A dash means no delta size is given.

- Signed update: each board downloaded a signed image from a private registry and confirmed it after reporting to Flight Control.
- Automatic rollback: the next signed candidate booted but could not establish its management connection. MCUboot automatically restored the confirmed image. Each agent reconnected under the same identity and reported the previous digest and the rollback.

The Pico also confirmed three consecutive signed **full-image** updates over Wi-Fi. Each came up with a new boot ID, the expected image digest and `trend` running.

| Pico 2 W build | OTA payload (B) | Assignment to `UpToDate`, about (s) |
|---|---:|---:|
| `+132` | 970,095 | 74 |
| `+133` | 970,096 | 74 |
| `+134` | 970,095 | 74 |

| Pico 2 W network setting for these updates | Value |
|---|---:|
| Link-layer packet size (B) | 1,514 |
| Receive buffer (B) | 4,380 |
| I/O buffers reserved | 8 |

## Memory and flash

Agent v0.9.4, heap read with the neural probe running. A dash means no value is given.

| S3 profile | Image (B) | User heap free (B) | User heap total (B) | Largest free block (B) | Kernel heap free (B) |
|---|---:|---:|---:|---:|---:|
| Mini N4R2 | 1,318,972 | 1,963,824 | — | 1,886,896 | 204,760 |
| DevKitC N8R8, Simple Boot | 1,318,972 | 8,241,048 | 8,388,604 | — | — |

[S3 Mini benchmarks](BENCHMARKS.md#s3-mini-n4r2) · [Same-board comparison](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board)

### Temperature and scheduling

The released S3 images use a 20 ms round-robin time slice. With buffered logging and tracing off, a complete `trend` sample takes a median of 497.4 µs across 28 batches. [Matched-input results on the three ports](BENCHMARKS.md#temperature-with-matched-input)

| Time slice (ms) | Median pause during a TLS handshake (ms) | Longest pause (ms) | Traced batches above 1 ms/sample |
|---:|---:|---:|---:|
| 200 | 202.0 | 208.1 | 10 of 28 |
| 20 | 21.0 | 45.0 | 3 of 28 |

ESP32-S3 N8R8, one core at 240 MHz, agent v0.9.4 with Wi-Fi active. [Time-slice runs](benchmarks/esp32s3-nuttx-trend-round-robin.json) · [Untraced 20 ms runs](benchmarks/esp32s3-nuttx-trend-20ms-untraced.json) · [Scheduler traces with the 200 ms slice](benchmarks/esp32s3-nuttx-trend-jitter.json) · [Runs that kept the temperature descriptor open](benchmarks/esp32s3-nuttx-temp-fd.json)

## Firmware OTA with MCUboot

1. Install imgtool 2.2.0 in the build environment. [Versions](DEPENDENCIES.md#nuttx-port)
2. Build with `OTA=1`. Select the module and set the MCUboot image version for each release. The agent reports this version as `systemInfo.customInfo.firmwareVersion`.

```sh
BOARD=esp32s3-n16r8 OTA=1 FW_VERSION=0.9.7 port/nuttx/build.sh
```

| Module | Primary slot | Secondary slot | Bytes per slot | Scratch (128 KiB) |
| --- | --- | --- | --- | --- |
| N4R2 | `0x10000` | `0x170000` | `0x160000` | `0x2d0000` |
| N8R8, N16R8 | `0x20000` | `0x400000` | `0x200000` | `0x600000` |

3. To migrate a Simple Boot device, install `mcuboot-esp32s3.bin` at zero and the padded, confirmed `nuttx.bin` at the primary offset over USB. Start with erased secondary and scratch trailers (the last 4 KiB of each region). Leave the factory partition and LittleFS intact to keep the enrollment and apps. The default provisioning example above targets Simple Boot.
4. When publishing a new shared tag, pass all ESP-IDF, Zephyr and NuttX images in one invocation of `tools/fw-push.py`, using the configured deployment signing key. Each NuttX argument takes the form `nuttx-esp32s3-n16r8=path/to/nuttx-ota.bin`, with the platform name matching the build. [NuttX fleet manifest](../tools/fleet/nuttx-lab.yaml)

The signed `flightctl-mcu/fw:v0.9.7` index includes the three S3 images and the four [Arm images](#signed-firmware-updates-on-arm). Each module selects its own compact `nuttx-ota.bin`. [Firmware image digests](../tools/kind/releases/v0.9.7.json)

On the device:

1. The agent checks the signed OCI index and streams the image into the secondary slot.
2. It checks the layer digest and board identifier, then requests a trial boot.
3. MCUboot swaps the slots and checks image integrity.
4. The agent confirms after contacting Flight Control. An unconfirmed reset lets MCUboot revert.

A three-minute startup deadline covers Wi-Fi and time synchronization during the trial.

### Verified OTA on the N16R8

On the N16R8, the agent reported firmware `0.9.7+0` on Apache NuttX 13.1.0. The board confirmed `fw:v0.9.7` after reboot. The agent log then recorded the change from `alarm:v0.9.5` to `alarm:v0.9.7` and the new app running. [Release image digests](../tools/kind/releases/v0.9.7.json)

<details>
<summary>Firmware 0.9.4+2 transfer and confirmation timings</summary>

| Firmware | Signed image (B) | Download over Wi-Fi (s) | Download to confirmation (s) |
|---|---:|---:|---:|
| `0.9.4+2` on NuttX 13.1.0 | 1,320,215 | 33 | 79 |

`alarm` was running. A second reset retained the image digest, enrollment and running app.

</details>

## Agent and apps

- The ESP32-S3 ROM prints a SHA-256 comparison warning when booting the NuttX Simple Boot image. The ROM continues into NuttX.
- The NuttX init service starts the agent on boot. The agent joins Wi-Fi, obtains IPv4 configuration over DHCP and waits for UTC before opening TLS. DHCP is requested at startup.
- The configured NTP service is `pool.ntp.org` over UDP port 123.
- App reads, subscriptions and telemetry serialize conversions through `/dev/temp`. Each read opens and closes its descriptor in the calling task group. The driver powers down the sensor after each reading. [Sampling time and time slice](#temperature-and-scheduling)
- The agent uses an ECDSA identity in LittleFS. The ESP32-S3 Digital Signature peripheral supports RSA, which the Flight Control 1.4 enrollment service cannot approve. [Device identity](SECURITY.md#other-chips-the-ds-peripheral-off-for-now)

### App hardware on ESP32-S3

The three S3 module profiles use the same NuttX peripheral drivers. Define the wiring in `/etc/fc/board.json` and grant the resource names to the app through `FC_ALLOW`. Select the RGB LED pin and byte order for the actual board revision. [Board map and grants](APPS.md)

| Resource | Wiring | Behavior |
| --- | --- | --- |
| GPIO | From the board map | Digital input, pull-up, pull-down, push-pull and open-drain output |
| GPIO edge interrupts | From the board map | Notify the app from a worker |
| Plain LED | From the board map | Supports active-low wiring |
| WS2812 pixel | From the board map | Driven by the RMT peripheral |
| I²C0 | SDA on GPIO8, SCL on GPIO9 | Write/read transactions use a repeated start and a shared deadline |
| SPI2 | SCK on GPIO12, MOSI on GPIO11, MISO on GPIO13 | A transfer can retain chip select across calls |
| SPI chip select | One per mapped device, active low | — |
| App UARTs, two | TX and RX from the board map | 8N1, 8E1 and 8N2 |
| RS-485 on UART2 | Direction pin on any GPIO other than GPIO0 | Active high |
| ADC1 | GPIO1 through GPIO10 | Reads in millivolts |
| PWM, LEDC | A mapped output pin | One PWM timer, including constant low and high duty |
| `die_temp` | The chip sensor | Reads in millidegrees Celsius |
| Temperature subscriptions | The chip sensor | Sample once per second for ready events or crossings of a threshold window |
| Battery sources | A wired ADC divider or an I²C Smart Battery | — |

- Metrics include temperature, Wi-Fi RSSI, scheduler CPU usage, heap watermarks and thread stack headroom.
- After a firmware assertion, the Flight Control console can retrieve the Xtensa ELF core through the agent's `coredump read` command. It contains the saved register state and up to 1 KiB of the faulting task's stack. RTC memory retains the dump across a software reset and loses it on power removal.

[N8R8 comparison on one board](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board) · [S3 Mini measurements](BENCHMARKS.md#s3-mini-n4r2)

## Not verified

- Hardware Secure Boot and flash encryption. Both were disabled on the tested N16R8 and when the Simple Boot image was tested.
- DHCP lease renewal, and Wi-Fi recovery after an access point outage.
- The ESP32, C3, C5 and C6 profiles use Simple Boot. Verified MCUboot updates cover the [S3 N16R8](#verified-ota-on-the-n16r8) and the [Arm boards](#signed-firmware-updates-on-arm).
- The app peripheral map on ESP32, C3 and C6. The S3 peripheral map applies to the S3 only. [App hardware on ESP32-S3](#app-hardware-on-esp32-s3)
