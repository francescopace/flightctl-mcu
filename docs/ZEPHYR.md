# The agent on Zephyr

Run the Zephyr agent on an [ESP32-S3](#on-an-esp32-s3), on a [Cortex-M3 in QEMU](#on-an-emulated-arm-cortex-m3), or as a Linux process with `native_sim`. The physical S3 supports Wi-Fi and firmware updates through MCUboot. [S3 firmware size and DRAM usage](RESULTS.md#firmware-size)

## Try it

Start with a running Flight Control instance and [enrollment credentials](QUICKSTART.md#2-credentials). Allow about 10 GB for the Docker build image and 900 MB for the Zephyr checkout.

```bash
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
port/zephyr/build.sh
```

Create a blank flash image once, provision it and start the simulator. On subsequent runs, use only the last command to keep the device identity and apps.

```bash
python3 -c "open('build/zephyr-flash.bin','wb').write(b'\xff'*(2*1024*1024))"
tools/provision.py --format zephyr --image build/zephyr-flash.bin
port/zephyr/run.sh build/zephyr-flash.bin
```

It asks to be approved like any device (`flightctl approve enrollmentrequest/<name>`), and keeps its key, certificate and apps in the flash file across restarts.

## Storage and network

`fc_creds` holds factory credentials in the `FCC1` format. The agent keeps keys, configuration and apps in LittleFS on `fc_storage`. It writes each key through a file rename. The port supplies POSIX threads and sockets to the shared core. On `native_sim` sockets use the host network directly.

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

A GPIO's active level becomes the board map's `active_low`. The fleet's `/etc/fc/board.json` may give a node another name (`{"gpio": {"pump": {"dt": "relay1"}}}`); it may not name pins, and a node the devicetree lacks is refused. With this map, grant the app `pump` in `FC_ALLOW`. It replaces the implicit name `relay1` and inherits the GPIO polarity and safe output level unless the map overrides them. Two explicit names for the same node are rejected.

The port reserves the console UART. On the ESP32-S3, an application UART can supply `de-gpios` for half-duplex RS-485. The port changes direction after both the FIFO and the transmit shift register are idle. The GPIO polarity comes from the devicetree. Other controllers that specify `de-gpios` return `FC_ENOTSUP`. Zephyr's I2C and SPI calls take no timeout, so a transfer lasts as long as the driver makes it; an SPI device waiting for another app's to let go of the bus gives up after 100 ms, as on ESP-IDF.

The ESP32-S3 overlay exposes the on-chip temperature probe as `core-temperature`. Grant it by name or map the probe to a fleet-wide sensor name. Adding drivers uses internal DRAM. Check the [link-time margin](RESULTS.md#firmware-size) when changing an overlay. [Peripheral tests](TESTING.md#peripheral-tests)

## On an emulated Arm Cortex-M3

QEMU's `mps2-an385`: Arm's MPS2 board with a Cortex-M3 and an SMSC LAN9118 Ethernet controller, which Zephyr drives. It is how CI tries the port on Arm and on Ethernet, without a board:

```bash
third_party/fetch-zephyr-sdk.sh
BOARD=mps2/an385 port/zephyr/build.sh
python3 -c "open('build/qemu-flash.bin','wb').write(b'\xff'*(1024*1024))"
tools/provision.py --format zephyr --board mps2_an385 --image build/qemu-flash.bin
port/zephyr/run-qemu.sh build/qemu-flash.bin
```

QEMU supplies DHCP, DNS and outbound Ethernet. Simulated flash in the last MiB of SRAM persists through device reboots and is lost when QEMU exits. The build uses 300 KB of static data, including a 96 KB kernel heap and a 128 KB C heap. It uses a test random generator and has no MCUboot.

## On a board

- **Thread stacks** come from the kernel heap. If allocation fails, Zephyr falls back to a 1 KB stack. The core checks the size before using it.
- **Faults** restart the device. Check `customInfo.lastFatalError` for the thread, uptime, PC and return addresses. On `native_sim` the PC is zero. [Crash dump retrieval](CONSOLE.md#download-a-crash-dump)
- **Watchdogs** restart the device after 10 minutes without accepted status or 30 minutes without management progress. The spec watchdog can also fire if status continues but the spec loop stalls.
- **Connections and mutexes** come from fixed pools. Keep `CONFIG_DNS_NUM_CONCUR_QUERIES`, `CONFIG_NET_MAX_CONN` and the POSIX mutex and condition-variable limits sized for concurrent agent and TLS work. A depleted mutex pool can fail the client signature during a handshake.

## On an ESP32-S3

The Zephyr board is an ESP32-S3-DevKitC N8R8 with 8 MiB each of flash and octal PSRAM. It runs Zephyr’s WiFi driver, MCUboot and TF-PSA-Crypto. The ESP-IDF measurements use a separate S3 N4R2. [Board results](RESULTS.md#coremark)

![The ESP32-S3 with Zephyr in Flight Control's UI](img/zephyr-device.png)
*The ESP32-S3 with Zephyr in Flight Control: operating system `zephyr`, architecture `xtensa`, and the fleet’s WASM app `alarm` running.*

```bash
third_party/fetch-wamr.sh
third_party/fetch-zephyr.sh
third_party/fetch-zephyr-sdk.sh
FW_TAG=$(sed -n 's|^[[:space:]]*image: .*/flightctl-mcu/fw:||p' tools/fleet/zephyr-lab.yaml)
BOARD=esp32s3_devkitc/esp32s3/procpu BUILD_DIR=build-esp32s3 port/zephyr/build.sh -- \
  "-Dzephyr_CONFIG_FC_FW_VERSION=\"$FW_TAG\"" \
  "-Dzephyr_CONFIG_MCUBOOT_IMGTOOL_SIGN_VERSION=\"${FW_TAG#v}\""
```

Match the board by USB serial number. For a fresh device, flash MCUboot, the signed app and credentials through the UART port (115200 baud console). `ERASE=1` erases the entire flash, including any existing identity. The label selects the example Zephyr fleet.

```bash
ERASE=1 port/zephyr/flash-esp32.sh /dev/cu.usbmodemXXXX .secrets --labels app=zephyr-lab --wifi-ssid <ssid> --wifi-password <password>
```

This build uses software image verification. Credentials remain readable to anyone with physical flash access. [Zephyr security](SECURITY.md#zephyr)

For the internal-RAM CoreMark run on the N8R8, the application and MCUboot overlays set `&psram0 { status = "disabled"; };`. In the application board configuration, set `CONFIG_ESP_SPIRAM=n`, omit the octal mode and external heap settings, and set `CONFIG_FC_HEAP_SIZE=140592` and `CONFIG_HEAP_MEM_POOL_SIZE=79872`. The WiFi buffer and thread stack settings are the same as on the ordinary N8R8 build. [CoreMark score and heap measurements](RESULTS.md#coremark)

The N8R8 reserves 2 MiB of PSRAM for the shared heap. WAMR prefers it for linear memories of at least 16 KiB. Software TLS also prefers it for two 17 KiB receive records. Both can fall back to internal RAM. The board uses 32 POSIX mutexes so telemetry can authenticate while MQTT is connected. [RAM by workload](RESULTS.md#ram)

Zephyr boards go in a fleet of their own (the example is `tools/fleet/zephyr-lab.yaml`). The lab fleets share one firmware OCI index. Each Zephyr image is identified by the board’s `CONFIG_FC_FW_PLATFORM`. To publish a Zephyr-only index under a new tag:

```bash
tools/fw-push.py --delta-from <running tag> <registry>/flightctl-mcu/fw:<new tag> esp32s3_devkitc=port/zephyr/build-esp32s3/zephyr/zephyr/zephyr.signed.bin
```

A board must hold the public half of the key the release is signed with; one provisioned with another refuses the fleet's app until its credentials are written again.

For [field provisioning](PROVISIONING.md#on-zephyr), enable `CONFIG_WIFI_USAGE_MODE_STA_AP`, give the DHCP server thread a 4 KB stack and disable the ICMP probe before granting a lease.

## Things to know about native_sim

- **Always `--seed-random`** (`port/zephyr/run.sh` passes it). Without it the random generator starts from a fixed seed, and every instance makes the same device key, hence the same device name.
- **A reboot starts the process again** (`CONFIG_NATIVE_SIM_REBOOT`), with the same flash file, as a board would start again.
- **The time is the host's**: the simulated clock starts there. A board takes it from SNTP (`CONFIG_FC_SNTP_SERVER`).
- **It is not a microcontroller.** Memory figures from it (a peak of about 120 KB of heap with one app, on a 64-bit build) say little about a board.
