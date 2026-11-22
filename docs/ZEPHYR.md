# The agent on Zephyr

The Zephyr agent runs on Espressif boards, the Nordic nRF7002-DK and the Raspberry Pi Pico 2 W over Wi-Fi. The ST NUCLEO-F767ZI connects over onboard Ethernet or the ST-LINK USB bridge. N8R8 is an ESP32-S3 module with 8 MiB of flash and 8 MiB of octal PSRAM. [Verified hardware](DEVICES.md#hardware-compatibility) · [App benchmarks](BENCHMARKS.md#managed-devices)

Builds: [ESP32-S3](#on-an-esp32-s3) · [S3 Mini, ESP32, C3, C5, C6](#on-an-esp32-s3-mini-esp32-c3-c5-or-c6) · [NUCLEO-F767ZI](#on-an-st-nucleo-f767zi) · [nRF7002-DK](#on-a-nordic-nrf7002-dk) · [Pico 2 W](#on-a-raspberry-pi-pico-2-w) · [QEMU Cortex-M3](#on-an-emulated-arm-cortex-m3) · [`native_sim`](#try-it)

## Memory and OTA on hardware

Image size and memory at link time. A dash means no value is given.

| Board | Firmware or build | Signed image (B) | Application flash (B) | RAM at link (B) | RAM free at link (B) | C heap size (B) | Kernel heap size (B) |
|---|---|---:|---:|---:|---:|---:|---:|
| S3 DevKitC N8R8 | Peripheral map and WS2812 | 1,047,479 | — | 379,328 | **1,344** | 111,920 | 94,208 |
| ST NUCLEO-F767ZI | v0.9.6 | 493,623 | 493,460 | 307,152 | — | — | — |
| Nordic nRF7002-DK | 0.9.5+nrf.4 | 916,511 | 916,348 | 285,164 | — | — | — |
| Pico 2 W, MCUboot | v0.9.5 | 834,830 | — | 454,728 | — | — | — |
| Pico 2 W, single image | v0.9.5 | — | 830,964 | 405,600 | — | — | — |

The S3 links against 380,672 B of internal DRAM. The Nucleo figure is in its 384 KiB RAM region. The Nordic build also has a 38,348 B MCUboot and a 112,344 B network-core image.

Signed firmware updates.

| Board | Firmware | Link | Download (s) | Swap and boot (s) | Boot to confirmation (s) | After the update |
|---|---|---|---:|---:|---:|---|
| ST NUCLEO-F767ZI | v0.9.6 | ST-LINK USB | 35 | — | 11.1 | `alarm` resumed with the same identity. |
| Nordic nRF7002-DK | 0.9.5+nrf.4 | Wi-Fi | 48 | 100 | 9 | — |
| Pico 2 W | v0.9.5 | Wi-Fi | — | — | — | Enrollment and apps survived the reboot. |

Heap readings. Free is the reading at the start of a download or in the stated condition. Minimum free is the low-water mark, or for a download the minimum from manifest lookup through connection close. The largest free block was sampled about every 16 KiB written.

| Board | Firmware or build | Heap | Condition | Free (B) | Minimum free (B) | Lowest largest free block (B) |
|---|---|---|---|---:|---:|---:|
| S3 DevKitC N8R8 | Peripheral map and WS2812 | C heap | Full signed download, `alarm` resident, metrics off | 97,184 | 30,140 | 76,676 |
| S3 DevKitC N8R8, PSRAM disabled | Peripheral map and WS2812 | C heap | Full signed download, `alarm` resident, metrics off | 94,184 | 41,260 | — |
| S3 DevKitC N8R8, PSRAM disabled | — | C heap | `mqttdemo` connected, in a separate run | 46,184 | — | — |
| S3 DevKitC N8R8, PSRAM disabled | CoreMark build | C heap | After instantiation | 30,896 | — | — |
| Nordic nRF7002-DK | 0.9.5+nrf.4 | Kernel heap | `alarm` running | — | 34,984 | — |
| Nordic nRF7002-DK | 0.9.6+nrf.heap | C heap (common libc) | Online with the fleet app `alarm` running | 125,980 | 124,364 | — |
| Nordic nRF7002-DK | 0.9.6+nrf.heap | Kernel heap | Same boot as the row above | — | 33,776 | — |
| Pico 2 W, single image | v0.9.5 | C heap | `trend` running, minimum since boot | 107,652 | 5,116 | — |
| Pico 2 W | — | C heap | Online, before app installation | 140,892 | 138,144 | — |
| Pico 2 W | — | Kernel heap | Online, before app installation | — | 83,568 | — |

The `mqttdemo` reading is below the 61,440 B telemetry export threshold. On the Nordic, the libc allocator records its minimum free bytes after each allocation or reallocation.

<details>
<summary>S3 DevKitC N8R8, v0.9.4 (die sensor and console LED)</summary>

| Quantity | Value (B) |
|---|---:|
| Image | 1,041,735 |
| Internal DRAM free at link | 3,456 |
| C heap free after neural inference | 73,180 |
| C heap total | 119,392 |

[Same-board benchmarks](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board)

</details>

### CPU clock and cache

The v0.9.7 Nordic image boots at 128 MHz and both v0.9.7 Nucleo images enable `CONFIG_CACHE_MANAGEMENT`. The Nucleo caches-on row was measured on `0.9.6+cache.1`.

| Board | Setting | CoreMark (iterations/s) | Richards (ms/iteration) | DeltaBlue (ms/iteration) |
|---|---|---:|---:|---:|
| Nordic nRF7002-DK | 64 MHz, application flash cache enabled | 4.510 | 3,811.2 | 3,179.7 |
| Nordic nRF7002-DK | 128 MHz, application flash cache enabled | 9.114 | 1,892.6 | 1,579.8 |
| ST NUCLEO-F767ZI | I/D caches off, 216 MHz | 5.837 | 2,915.9 | 2,409.5 |
| ST NUCLEO-F767ZI | I/D caches on, 216 MHz | 24.681 | 720.2 | 606.2 |

Medians of three runs per board and setting. [The 128 MHz and caches-on results among the other boards](BENCHMARKS.md#managed-devices)

### State logging

Zephyr keeps INFO state changes in the device log buffer and off the UART. App stdout still goes to the console.

| State changes written to | Complete `trend256` sample, median (µs) |
|---|---:|
| Log buffer | 348.7 |
| Log buffer and UART | 2,163.5 |

S3 N8R8 at 240 MHz. [Three-port comparison](BENCHMARKS.md#temperature-with-matched-input) · [Runs with and without UART output](benchmarks/esp32s3-zephyr-trend-state-uart.json) · [Profile of state logging with UART output](benchmarks/esp32s3-zephyr-trend-state.json)

## Try it

Start with a running Flight Control instance and [enrollment credentials](QUICKSTART.md#2-credentials). Allow about 10 GB for the Docker build image and 900 MB for the Zephyr checkout.

1. Fetch and build.

```bash
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
port/zephyr/build.sh
```

2. Create a blank flash image once, provision it and start the simulator. On subsequent runs, use only the last command to keep the device identity and apps.

```bash
python3 -c "open('build/zephyr-flash.bin','wb').write(b'\xff'*(2*1024*1024))"
tools/provision.py --format zephyr --image build/zephyr-flash.bin
port/zephyr/run.sh build/zephyr-flash.bin
```

3. The device asks to be approved like any device (`flightctl approve enrollmentrequest/<name>`). It keeps its key, certificate and apps in the flash file across restarts.

Use `log` in the agent's remote console to see app state changes. [State logging](#state-logging)

## Storage and network

| Stored | Where |
|---|---|
| Factory credentials | `fc_creds`, in the `FCC1` format: the tag `FCC1`, a length and a JSON object, as written by `tools/provision.py --format zephyr` |
| Keys, configuration and apps | LittleFS on `fc_storage`, with atomic updates through a file rename |
| Keys, configuration and apps on the Nucleo | Zephyr Settings backed by NVS in two physical erase sectors |

On `native_sim` sockets use the host network directly.

On boards with MCUboot, the agent streams firmware into the second slot and reads it back before requesting a test boot. MCUboot checks the signature and reverts an unconfirmed image. `native_sim` and the QEMU build have no firmware updates.

## Hardware for apps

Define each app peripheral as a child of a `flightctl,app-hw` node in the board overlay. Enable its driver in the board's `.conf`. [Devicetree binding](../port/zephyr/dts/bindings/flightctl,app-hw.yaml)

```dts
/ {
	fc-hw {
		compatible = "flightctl,app-hw";
		relay1 { gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>; safe = <0>; };
		button0 { gpios = <&gpio0 6 GPIO_ACTIVE_LOW>; };
		core-temperature { temperature-sensor = <&coretemp>; die-temperature; };
		level { io-channels = <&adc0 0>; };
		fan { pwms = <&pwm0 0 1000000>; };
		sensors { i2c-bus = <&i2c0>; clock-frequency = <100000>; };
		flash { spi-bus = <&spi2>; cs-gpios = <&gpio0 10 GPIO_ACTIVE_LOW>; spi-mode = <3>; };
		modbus { uart = <&uart1>; current-speed = <9600>; };
	};
};
```

- A GPIO's active level becomes the board map's `active_low`.
- The fleet's `/etc/fc/board.json` may give a node another name: `{"gpio": {"pump": {"dt": "relay1"}}}`. With this map, grant the app `pump` in `FC_ALLOW`.
- The new name replaces the implicit name `relay1`. It inherits the GPIO polarity and safe output level unless the map overrides them.
- The map may not name pins. A node the devicetree lacks is refused. Two explicit names for the same node are rejected.

Limits:

- The port reserves the console UART.
- On the ESP32-S3, an application UART can supply `de-gpios` for half-duplex RS-485. The GPIO polarity comes from the devicetree. Other controllers that specify `de-gpios` return `FC_ENOTSUP`.
- Zephyr's I2C and SPI calls take no timeout, so a transfer lasts as long as the driver makes it.
- An SPI device waiting for another app's to let go of the bus gives up after 100 ms, as on ESP-IDF.

On ESP32-S3 DevKitC and Pico 2 W, grant the following names in `FC_ALLOW` to use the default board map.

| App name | ESP32-S3 DevKitC-1 | Raspberry Pi Pico 2 W |
|---|---|---|
| `led0` | Onboard WS2812 RGB pixel driven by SPI3 | Onboard green LED on CYW43439 WL_GPIO0, lit by any nonzero RGB component |
| `gpio6` | GPIO6, digital input/output with edge interrupts | GP6, digital input/output with edge interrupts |
| `adc0` | GPIO4, ADC1 channel 3 | GP26, ADC channel 0 |
| `pwm0` | GPIO7, LEDC channel 0 | GP14, PWM slice 7A |
| `i2c0` | SDA GPIO1, SCL GPIO2 | SDA GP4, SCL GP5 |
| `spi0` | CS GPIO10, MOSI GPIO11, SCK GPIO12, MISO GPIO13 | CS GP17, MOSI GP19, SCK GP18, MISO GP16 |
| `uart1` | TX GPIO17, RX GPIO18 | TX GP8, RX GP9 |
| `core-temperature` | ESP32-S3 die sensor | RP2350 die sensor |

- `die_temp` reads the same physical probe without a grant.
- Subscribe to `core-temperature` for data-ready or threshold events. While subscribed, the port samples these two probes once per second.
- An app can also read the probe directly at its own cadence. [Peripheral tests](TESTING.md#peripheral-tests)

The default S3 image drives the DevKitC-1 v1.0 RGB LED on GPIO48. For v1.1, add `-Dzephyr_EXTRA_DTC_OVERLAY_FILE=boards/esp32s3_devkitc_v11_rgb.overlay` to the board's sysbuild CMake arguments to select GPIO38. [Espressif board revisions](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)

## On an emulated Arm Cortex-M3

QEMU's `mps2-an385`: Arm's MPS2 board with a Cortex-M3 and an SMSC LAN9118 Ethernet controller, which Zephyr drives. It is how CI tries the port on Arm and on Ethernet, without a board:

```bash
third_party/fetch-zephyr-sdk.sh
BOARD=mps2/an385 port/zephyr/build.sh
python3 -c "open('build/qemu-flash.bin','wb').write(b'\xff'*(1024*1024))"
tools/provision.py --format zephyr --board mps2_an385 --image build/qemu-flash.bin
port/zephyr/run-qemu.sh build/qemu-flash.bin
```

| QEMU build | Value | Unit |
|---|---:|---|
| Static data | 300 | KB |
| Kernel heap, part of the static data | 96 | KiB |
| C heap, part of the static data | 128 | KiB |
| Simulated flash, at the end of SRAM | 1 | MiB |

- QEMU supplies DHCP, DNS and outbound Ethernet.
- The simulated flash persists through device reboots and is lost when QEMU exits.
- The build uses a test random generator and has no MCUboot.

## On a board

- **Thread stacks** come from the kernel heap. If allocation fails, Zephyr falls back to a 1 KiB stack. The core checks the size before using it.
- **Faults** restart the device. Check `customInfo.lastFatalError` for the thread, uptime, PC and return addresses. On `native_sim` the PC is zero. [Crash dump retrieval](CONSOLE.md#download-a-crash-dump)
- **Watchdogs** restart the device after 10 minutes of failed status delivery or lost contact while idle. The independent 30-minute watchdog checks progress in both the status and rendered-spec loops.
- **Connections and mutexes** come from fixed pools. Keep `CONFIG_DNS_NUM_CONCUR_QUERIES`, `CONFIG_NET_MAX_CONN` and the POSIX mutex and condition-variable limits sized for concurrent agent and TLS work. A depleted mutex pool can fail the client signature during a handshake.

## On an ESP32-S3 Mini, ESP32, C3, C5 or C6

The ESP32-S3 Mini N4R2 (4 MiB of flash, 2 MiB of quad PSRAM) and the ESP32-C5 Mini (4 MiB of flash) need their flash layout in both the application and MCUboot.

For the S3 Mini:

```sh
ZEPHYR_DOCKER=1 BOARD=esp32s3_devkitc/esp32s3/procpu BUILD_DIR=build-s3-mini port/zephyr/build.sh -- \
  -Dzephyr_EXTRA_DTC_OVERLAY_FILE=boards/esp32s3_mini_n4r2.overlay \
  -Dmcuboot_EXTRA_DTC_OVERLAY_FILE=/project/port/zephyr/boards/esp32s3_mini_n4r2_mcuboot.overlay \
  -Dzephyr_EXTRA_CONF_FILE=boards/esp32s3_mini_n4r2.conf
```

For the C5 Mini:

```sh
ZEPHYR_DOCKER=1 BOARD=esp32c5_devkitc/esp32c5/hpcore BUILD_DIR=build-c5-mini port/zephyr/build.sh -- \
  -Dzephyr_EXTRA_DTC_OVERLAY_FILE=boards/esp32c5_mini.overlay \
  -Dmcuboot_EXTRA_DTC_OVERLAY_FILE=/project/port/zephyr/boards/esp32c5_mini_flash.overlay \
  -Dzephyr_EXTRA_CONF_FILE=boards/esp32c5_mini.conf
```

The `fw:v0.9.7` index names these two images `esp32s3-mini-n4r2` and `esp32c5-mini`, and the ESP32, C3 and C6 images `esp32_devkitc`, `esp32c3_devkitm` and `esp32c6_devkitc`. Firmware updates over the air are not verified on these five modules.

The other Espressif targets use their board defaults. Set `BOARD` and use a separate `BUILD_DIR` for each target. For initial installation, take the MCUboot, primary slot and factory offsets from the application's generated `zephyr/zephyr/zephyr.dts`. Generate the factory partition with `tools/provision.py --format zephyr --board <provisioning profile>`.

| Chip | Zephyr board | Provisioning profile | Factory offset |
| --- | --- | --- | --- |
| ESP32-S3 Mini N4R2 | `esp32s3_devkitc/esp32s3/procpu` | `esp32s3_mini_n4r2` | `0x3b0000` |
| ESP32 | `esp32_devkitc/esp32/procpu` | `esp32_devkitc_procpu` | `0x3b0000` |
| ESP32-C3 | `esp32c3_devkitm` | `esp32c3_devkitm` | `0x3b0000` |
| ESP32-C5 Mini (4 MiB) | `esp32c5_devkitc/esp32c5/hpcore` | `esp32c5_mini` | `0x3b0000` |
| ESP32-C5 DevKitC (8 MiB) | `esp32c5_devkitc/esp32c5/hpcore` | `esp32c5_devkitc_hpcore` | `0x7b0000` |
| ESP32-C6 | `esp32c6_devkitc/esp32c6/hpcore` | `esp32c6_devkitc_hpcore` | `0x3b0000` |

On the C5 Mini:

- The console uses native USB Serial/JTAG.
- The agent allocates from the spare high-performance SRAM bank first, then the main SRAM arena. RTC RAM remains available when neither bank can satisfy the allocation.
- At 240 MHz with agent 0.9.6 and Wi-Fi active, Richards v0.9.5 took 742.0 ms/iteration, the lowest of the three ports on this board. [C5 on the three ports](BENCHMARKS.md#managed-devices) · [C5 measurements and firmware hash](benchmarks/esp32-c5-zephyr-hp-sram-240mhz.json)

All six modules enrolled over Wi-Fi. On the classic ESP32 and C5 Mini, the agent allocates small objects in additional internal SRAM banks after boot. This leaves a contiguous block for a 64 KiB WASM page. Both boards ran CoreMark and DeltaBlue over the management connection. [Benchmark results](BENCHMARKS.md#managed-devices)

Agent heap at startup with Wi-Fi connected. The kernel heap serves thread stacks and networking. The console reply is a remote command answered over Wi-Fi during DeltaBlue. A dash means no value is given.

| Board | Agent heap without the additional banks (B) | Agent heap with the additional banks (B) | Kernel heap free (B) | Console reply during DeltaBlue (s) |
|---|---:|---:|---:|---:|
| ESP32 | 86,804 | 135,788 | 40,564 | 0.675 |
| C5 Mini | 100,068 | 142,788 | — | 0.691 |

## On an ESP32-S3

The Zephyr board is an ESP32-S3-DevKitC-compatible N8R8. It runs Zephyr’s WiFi driver, MCUboot and TF-PSA-Crypto. Its pinout identifies GPIO38 as the onboard RGB LED, and the build below selects that pin. [CoreMark](BENCHMARKS.md#coremark-module-sizes) · [Same-board app benchmarks](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board)

![The ESP32-S3 with Zephyr in Flight Control's UI](img/zephyr-device.png)
*The ESP32-S3 with Zephyr in Flight Control: operating system `zephyr`, architecture `xtensa`, and the fleet’s WASM app `alarm` running.*

1. Build.

```bash
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
third_party/fetch-zephyr-sdk.sh
BOARD=esp32s3_devkitc/esp32s3/procpu BUILD_DIR=build-esp32s3 port/zephyr/build.sh -- \
  -Dzephyr_EXTRA_DTC_OVERLAY_FILE=boards/esp32s3_devkitc_v11_rgb.overlay \
  '-Dzephyr_CONFIG_FC_FW_VERSION="0.9.7"' \
  '-Dzephyr_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="0.9.7+0"'
```

The agent reports `CONFIG_FC_FW_VERSION` as `systemInfo.customInfo.firmwareVersion`.

2. Match the board by USB serial number. For a fresh device, flash MCUboot, the signed app and credentials through the UART port (115200 baud console). `ERASE=1` erases the entire flash, including any existing identity. The label selects the example Zephyr fleet.

```bash
ERASE=1 port/zephyr/flash-esp32.sh /dev/cu.usbmodemXXXX .secrets --labels app=zephyr-lab --wifi-ssid <ssid> --wifi-password <password>
```

This build uses software image verification. Credentials remain readable to anyone with physical flash access. [Zephyr security](SECURITY.md#zephyr)

For the internal-RAM CoreMark run on the N8R8:

- In the application and MCUboot overlays, set `&psram0 { status = "disabled"; };`.
- In the application board configuration, set `CONFIG_ESP_SPIRAM=n`, omit the octal mode and external heap settings, and set `CONFIG_FC_HEAP_SIZE=140592` and `CONFIG_HEAP_MEM_POOL_SIZE=79872`.
- The WiFi buffer and thread stack settings are the same as on the ordinary N8R8 build. [CoreMark score and heap measurements](BENCHMARKS.md#coremark-module-sizes)

The N8R8 reserves 2 MiB of PSRAM for the shared heap. WAMR prefers it for linear memories of at least 16 KiB. Software TLS also prefers it for two 17 KiB receive records. Both can fall back to internal RAM. The board uses 32 POSIX mutexes so telemetry can authenticate while MQTT is connected. [Measured heap and OTA](#memory-and-ota-on-hardware)

Every Zephyr board joins the same example fleet. Its firmware OCI index selects the image by each board’s `CONFIG_FC_FW_PLATFORM`. [Publish the board images and apply the fleet](QUICKSTART.md#6-give-it-firmware-config-and-an-app)

A board must hold the public half of the key the release is signed with; one provisioned with another refuses the fleet's app until its credentials are written again.

For [field provisioning](PROVISIONING.md#on-zephyr), enable `CONFIG_WIFI_USAGE_MODE_STA_AP`, give the DHCP server thread a 4 KiB stack and disable the ICMP probe before granting a lease.

## On an ST NUCLEO-F767ZI

The `fw:v0.9.7` index has two Nucleo images.

| Nucleo firmware | Network | Build |
|---|---|---|
| `nucleo_f767zi` | Onboard RJ45 port with DHCP, for both management and OTA | `BOARD=nucleo_f767zi port/zephyr/build.sh` |
| `nucleo_f767zi-stlink` | Virtual COM port on the ST-LINK connector | Step 1 below |

With ST-LINK, a Mac running the bridge provides DHCP, DNS and outbound IP connectivity through that USB cable. The board uses its own TLS identity for Flight Control and the registry. Each bridge has its own private network.

1. Build.

```bash
export BOARD=nucleo_f767zi
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
ZEPHYR_SDK_TOOLCHAINS=arm-zephyr-eabi third_party/fetch-zephyr-sdk.sh
BUILD_DIR=build-nucleo-stlink port/zephyr/build.sh -- \
    '-Dzephyr_EXTRA_CONF_FILE=boards/nucleo_f767zi_stlink.conf' \
    '-Dzephyr_EXTRA_DTC_OVERLAY_FILE=boards/nucleo_f767zi_stlink.overlay' \
    '-Dzephyr_CONFIG_FC_FW_VERSION="0.9.7"' \
    '-Dzephyr_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="0.9.7+0"'
```

2. Generate the factory partition with `tools/provision.py --format zephyr --board nucleo_f767zi --out factory.bin`.
3. For first installation, use the probe's USB serial number to select the board and write the files below. They are relative to the build directory.

| STM32 flash address | Initial contents |
|---|---|
| `0x08000000` | `mcuboot/zephyr/zephyr.hex` |
| `0x08010000` | Blank 64 KiB for identity, configuration and apps |
| `0x08020000` | 128 KiB factory partition, `factory.bin` |
| `0x08040000` | `zephyr/zephyr/zephyr.signed.confirmed.hex` |

4. Start the bridge with Python and pySerial installed. Use the ST-LINK serial number reported by `python3 -m serial.tools.list_ports -v`.

```bash
go install github.com/containers/gvisor-tap-vsock/cmd/gvproxy@v0.9.0
python3 port/zephyr/stlink-network.py --serial "${STLINK_SERIAL:?set the probe serial number}" \
    --gvproxy "$(go env GOPATH)/bin/gvproxy"
```

- Keep the bridge running while the device uses USB networking, including across OTA reboots. Restart it after reconnecting the USB cable.
- The bridge uses USART3 at 460800 baud. Use the Flight Control remote console for logs. [gvproxy](https://github.com/containers/gvisor-tap-vsock)
- Publish `zephyr/zephyr/zephyr.signed.bin` for OTA.
- NVS has two 32 KiB erase sectors, one reserved for garbage collection, so credentials, signed apps and configuration share less than 32 KiB of live storage.
- The board map exposes `led0` on PB0 and the STM32 die temperature sensor.

| Nucleo flash region | Size (KiB) |
|---|---:|
| Primary slot | 768 |
| Secondary slot | 768 |
| MCUboot scratch sector | 256 |

Over RJ45, with firmware `0.9.6+st.eth`:

- The board enrolled, reached `Online` and `UpToDate`, and ran signed `trend:v0.9.5` from the private registry.
- After a power cycle, the agent reconnected with the same identity and a new boot ID. The remote console responded and `trend` resumed, reporting 29.9 °C compared with 31.0 °C before shutdown.
- The same Ethernet profile subsequently downloaded and confirmed the 501,111 B signed image from `fw:v0.9.6` over RJ45. After the OTA reboot, the remote console reported `alarm Running ok` with the USB network bridge stopped.

## On a Nordic nRF7002-DK

- Use the nRF5340 application core as the board target.
- The build produces MCUboot, the signed agent and an HCI IPC controller for the network core. The agent starts the controller before enrollment.
- PSA Crypto obtains random bytes from the network core through HCI and returns an error if that source is unavailable.
- The nRF7002 companion provides Wi-Fi.
- The network core also samples the nRF5340 **die temperature** once per second, from the chip’s TEMP peripheral at 0.25 °C resolution. Apps read it as `die_temp`, in millidegrees Celsius. Reads fail if the most recent sample is more than three seconds old.

When upgrading a DK first installed with the standard HCI-only image, program the new `hci_ipc/zephyr/zephyr.hex` on the network core over USB. Subsequent application-core updates use the normal OTA path. [Nordic TEMP specification](https://docs.nordicsemi.com/r/bundle/ps_nrf5340/page/temp.html)

Grant these board-map names in the app’s `FC_ALLOW`. The built-in `die_temp` read is available without a grant.

| App name | Connection on the DK | Behavior or default |
|---|---|---|
| `core-temperature` | nRF5340 die sensor | Data-ready events follow new IPC samples |
| `led0` | First green LED, P1.06 (D4) | Lit by any nonzero RGB component, off at zero or when the handle closes |
| `pwm-led1` | Second green LED, P1.07 (D5) | PWM duty controls brightness |
| `gpio-d9` | D9 (P1.11) | Digital input or output with GPIO edge interrupts |
| `adc-a0` | A0 (P0.04) | 12-bit SAADC input in millivolts |
| `i2c0` | Arduino header SDA on D14 and SCL on D15 | 100 kHz by default |
| `spi0` | Arduino header CS on D10, MOSI on D11, MISO on D12 and SCK on D13 | Mode 0 at 1 MHz |
| `uart2` | UARTE2 TX on A2 (P0.06) and RX on A3 (P0.07) | 115200 baud by default |

Threshold events on `core-temperature` report transitions between the regions below, inside and above the configured bounds.

1. Build.

```bash
export BOARD=nrf7002dk/nrf5340/cpuapp
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
ZEPHYR_SDK_TOOLCHAINS=arm-zephyr-eabi third_party/fetch-zephyr-sdk.sh
BUILD_DIR=build-nrf7002-board port/zephyr/build.sh -- \
    '-Dzephyr_CONFIG_FC_FW_VERSION="0.9.7"' \
    '-Dzephyr_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="0.9.7+0"'
```

2. For the first installation, connect a data cable to **J2 (IMCU USB)** and select the DK by its probe serial number.
3. Write the images below with nRF Util's device programmer, with SEGGER J-Link installed on the programming host. The files are relative to `port/zephyr/build-nrf7002-board/`.

| Destination | Image for first installation |
|---|---|
| Network core, internal flash | `hci_ipc/zephyr/zephyr.hex` |
| Application core, internal flash at `0x00000` | `mcuboot/zephyr/zephyr.hex` |
| Application core, primary slot at `0x10000` | `zephyr/zephyr/zephyr.signed.confirmed.hex` |
| Application core, factory partition at `0xfc000` | A 16 KiB blob from `tools/provision.py --format zephyr --board nrf7002dk_nrf5340_cpuapp --out factory.bin` with your enrollment and Wi-Fi settings |

For OTA, publish the compact `zephyr/zephyr/zephyr.signed.bin` under platform `nrf7002dk_nrf5340_cpuapp`. Each application slot is 944 KiB. MCUboot stages the update in external SPI flash, which also holds a separate 1 MiB LittleFS partition. This OTA path updates the application core.

[Enrollment and fleet setup](QUICKSTART.md)

## On a Raspberry Pi Pico 2 W

The Pico 2 W uses the RP2350 Cortex-M33, the CYW43439 Wi-Fi chip and a USB serial console. For fleet OTA, select the MCUboot board variant.

| Flash offset | Contents | Size (KiB) |
|---|---|---:|
| `0x000000` | MCUboot | 128 |
| `0x020000` | Primary slot, including MCUboot metadata | 1472 |
| `0x190000` | Secondary slot, including MCUboot metadata | 1472 |
| `0x300000` | Factory credentials | 16 |
| `0x304000` | LittleFS | 1008 |

The factory credentials and LittleFS start at the same offsets as on the standalone USB build.

1. Build. Use the deployment's MCUboot signing key for the bootloader and subsequent releases. Set the image version for each build.

```bash
export BOARD=rpi_pico2/rp2350a/m33_0/w/mcuboot
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
ZEPHYR_SDK_TOOLCHAINS=arm-zephyr-eabi third_party/fetch-zephyr-sdk.sh
BUILD_DIR=build-pico2w-ota port/zephyr/build.sh -- -Dzephyr_SNIPPET=cdc-acm-console \
    '-Dzephyr_CONFIG_FC_FW_VERSION="0.9.7"' \
    '-Dzephyr_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION="0.9.7+0"'
```

2. For first installation or migration, connect the board with BOOTSEL held down and set `PICO_SERIAL` to its USB serial number.
3. **On a fresh board**, write the [enrollment credentials](QUICKSTART.md#2-credentials) and site Wi-Fi settings. On an enrolled standalone board, skip this step to keep its factory settings.

```bash
(
  set -eu
  : "${PICO_SERIAL:?set the board USB serial number}"
  work=$(mktemp -d)
  trap 'rm -rf "$work"' EXIT
  tools/provision.py --format zephyr --board rpi_pico2_rp2350a_m33_0_w \
      --labels board=pico2w,os=zephyr,app=zephyr-lab --out "$work/factory.bin"
  picotool load -v "$work/factory.bin" -t bin -o 0x10300000 --ser "$PICO_SERIAL"
)
```

4. Install the confirmed image and bootloader over USB once. The erase covers the last sector of the secondary slot. The writes preserve the factory partition and LittleFS, including the device identity and installed apps.

```bash
(
  set -eu
  : "${PICO_SERIAL:?set the board USB serial number}"
  dir=port/zephyr/build-pico2w-ota
  picotool erase -r 0x102ff000 0x10300000 --ser "$PICO_SERIAL"
  picotool load -v "$dir/zephyr/zephyr/zephyr.signed.confirmed.bin" -t bin -o 0x10020000 --ser "$PICO_SERIAL"
  picotool load -v "$dir/mcuboot/zephyr/zephyr.bin" -t bin -o 0x10000000 --ser "$PICO_SERIAL"
  picotool reboot --ser "$PICO_SERIAL"
)
```

5. Approve a fresh board in [step 5 of the quick start](QUICKSTART.md#5-approve-it).

For subsequent releases, include `rpi_pico2_rp2350a_m33_0_w=port/zephyr/build-pico2w-ota/zephyr/zephyr/zephyr.signed.bin` alongside the other platform images in the same `tools/fw-push.py` invocation. The Pico downloads the compact image over Wi-Fi and confirms its trial boot after Flight Control accepts its status.

## Things to know about native_sim

- **Always `--seed-random`** (`port/zephyr/run.sh` passes it). Without it the random generator starts from a fixed seed, and every instance makes the same device key, hence the same device name.
- **A reboot starts the process again** (`CONFIG_NATIVE_SIM_REBOOT`), with the same flash file, as a board would start again.
- **The time is the host's**: the simulated clock starts there. A board takes it from SNTP (`CONFIG_FC_SNTP_SERVER`).
- **It is not a microcontroller.** Memory figures from it (a peak of about 120 KB of heap with one app, on a 64-bit build) say little about a board.
