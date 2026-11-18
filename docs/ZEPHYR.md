# The agent on Zephyr

The second port of the agent, after ESP-IDF: `port/zephyr`, the port and a small application (`port/zephyr/src`). The core is unchanged between the two.

**Where it stands.** On `native_sim` (Zephyr built as a Linux process) the agent passes the end-to-end test of the simulated device, against an unmodified Flight Control. On a real ESP32-S3 it enrolls over WiFi, stays online, runs a signed WASM app and updates its firmware through MCUboot, whole or as a delta, going back when the new one does not work (below): the first chip to run both ports. On an Arm Cortex-M3 with Ethernet, emulated in QEMU, it passes the same end-to-end test as on `native_sim`.

## Try it

Needs Docker. The first command pulls Zephyr's build image (about 10 GB) and a Zephyr tree with the modules the port uses (about 900 MB, in `third_party/zephyr-ws`).

```bash
third_party/fetch-wamr.sh
```

```bash
third_party/fetch-zephyr.sh
```

```bash
port/zephyr/build.sh
```

Then a flash image with the device's credentials (`tools/enrollment.sh` makes them, as for the other devices), and the device itself:

```bash
python3 -c "open('build/zephyr-flash.bin','wb').write(b'\xff'*(2*1024*1024))"
```

```bash
tools/provision.py --format zephyr --image build/zephyr-flash.bin
```

```bash
port/zephyr/run.sh build/zephyr-flash.bin
```

It asks to be approved like any device (`flightctl approve enrollmentrequest/<name>`), and keeps its key, certificate and apps in the flash file across restarts.

## What is Zephyr's and what is ours

| | |
|---|---|
| Kernel, threads, clock | Zephyr, through its POSIX layer: the core uses pthreads and `clock_gettime` as elsewhere. |
| TLS, X.509 | Zephyr's mbedTLS module, 4.1, the version ESP-IDF 6.1 has; its cryptography is PSA's (TF-PSA-Crypto), with its threading layer on POSIX mutexes (`include/fc-tf-psa-user.h`), which Zephyr has no option for: the agent's threads use PSA at the same time. |
| Network | Zephyr's sockets; `port/zephyr/fc_net_zephyr.c` gives mbedTLS its `mbedtls_net_*` functions on them. On `native_sim` the sockets are the host's (no TAP interface to set up). |
| Storage | LittleFS on the `fc_storage` flash partition: one file per key, written through a rename. |
| Credentials | The `fc_creds` partition: `"FCC1"`, a length and a JSON object with the fields of the ESP-IDF firmware's partition (`tools/provision.py --format zephyr`). |
| C heap | `port/zephyr/fc_heap_zephyr.c`, a heap the port owns, so that the device can report how much is free (`memFreeBytes`, `memMinFreeBytes`); Zephyr's malloc does not say. |
| WASM | WAMR's Zephyr platform, fast interpreter, as in the other builds. |
| Firmware updates | MCUboot's two slots (boards built with it): the image is streamed into the second, read back, and asked for as a test; MCUboot checks its signature, swaps the slots, and swaps them back unless the agent confirms the image once it has reached Flight Control. `native_sim` has no MCUboot and no updates. |
| Firmware deltas | the core's decoder, as elsewhere; the port reads the running image from the first slot. |

## Hardware for apps

The names apps open are the devicetree's: the children of a node with compatible `flightctl,app-hw` ([its binding](../port/zephyr/dts/bindings/flightctl,app-hw.yaml)), each a GPIO, an ADC channel, a PWM, an I2C bus, an SPI device or a UART by what it has. A board's overlay adds them and its `.conf` enables the drivers they need:

```dts
/ {
	fc-hw {
		compatible = "flightctl,app-hw";
		relay1 { gpios = <&gpio0 5 GPIO_ACTIVE_HIGH>; safe = <0>; };
		button0 { gpios = <&gpio0 6 GPIO_ACTIVE_LOW>; };
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

The production board overlays do not yet expose app peripherals: the ESP32-S3 build, with 4 KB of its data region free, has little room for the drivers until something else gives way. [`tests/zephyr-hw`](../tests/zephyr-hw) runs it all on `native_sim`'s emulated drivers (GPIO, ADC, a fake PWM, I2C with an AT24 EEPROM, SPI with a loopback device, UART wired back to itself), in CI: `tests/zephyr-hw.sh`. The ESP32-S3 test overlay also runs a WASM app against a virtual button under both names. [ESP32-S3 results](RESULTS.md#zephyr-esp32-s3-hardware-names)

## Zephyr 4.5

`third_party/fetch-zephyr.sh` pins v4.5.0, which has mbedTLS 4.1.1, the same as ESP-IDF 6.1.1: the core is written for mbedTLS 4, whose cryptography is PSA only.

## What the tests check

CI runs the end-to-end test of the simulated device with the Zephyr build as the device, on `native_sim` and on `mps2/an385` in QEMU, and the port's hardware for apps and the apps' C library on emulated drivers: see [TESTING.md](TESTING.md) (Zephyr). One check is weaker than on the host: that a config file was stored is read from the device's log, as the file is inside the flash image.

## On an emulated Arm Cortex-M3

QEMU's `mps2-an385`: Arm's MPS2 board with a Cortex-M3 and an SMSC LAN9118 Ethernet controller, which Zephyr drives. It is how CI tries the port on Arm and on Ethernet, without a board:

```bash
third_party/fetch-zephyr-sdk.sh
```

```bash
BOARD=mps2/an385 port/zephyr/build.sh
```

```bash
python3 -c "open('build/qemu-flash.bin','wb').write(b'\xff'*(1024*1024))"
```

```bash
tools/provision.py --format zephyr --board mps2_an385 --image build/qemu-flash.bin
```

```bash
port/zephyr/run-qemu.sh build/qemu-flash.bin
```

QEMU's user network gives the device an address by DHCP, a DNS server and the way out to Flight Control. The board has no flash controller: the credentials and the storage are in Zephyr's flash simulator, in the last MB of SRAM, where `run-qemu.sh` loads the credentials partition. What the device stores survives its restarts (a certificate, the decommissioning), not the end of QEMU.

The build has the memory of a microcontroller with 384 KB of SRAM, the least a device needs ([DEVICES.md](DEVICES.md#what-a-device-needs)): 300 KB of static data, of which 96 KB of kernel heap (thread stacks) and 128 KB of C heap; 400 KB of code. It passes the end-to-end test of the simulated device, from enrollment to decommissioning.

What it is not: there is no MCUboot and no firmware update, and no random generator (Zephyr's test generator, whose numbers are not secret, stands in for one).

## On a board

- **A thread's stack** comes from the kernel heap when its size is set; if that fails, Zephyr keeps a 1 KB default. The core checks and logs it instead of overflowing.
- **A fatal error restarts the device** (Zephyr's default is to stop), and what it was is kept in RAM and logged at the next boot. The status says it too, in `customInfo`: `resetReason` is `panic`, as on ESP-IDF, and `lastFatalError` gives the reason, the thread, the uptime, the `pc` and what was calling: the `lr` on Arm, on the ESP32-S3 (Xtensa) the return addresses of Zephyr's own backtrace; on `native_sim` the `pc` is 0. It is the ESP-IDF firmware's text ([core/include/fc_fatal.h](../core/include/fc_fatal.h)), for `addr2line` against `zephyr.elf` (`tools/fw-elf.py` gets a release's back from the registry), with the firmware's version after it when a rollback put another one in its place.
- **A watchdog on the system work queue** restarts the device after 30 minutes without a status accepted or a spec taken, as on ESP-IDF; the agent itself restarts it after 10 minutes without a status. While only console sessions change, the agent parses the list of sessions, not the whole spec.
- **DNS and connections**: Zephyr resolves one query at a time and allows four connections by default; the agent's threads resolve together at start, and the registry needs a connection beside Flight Control's, DHCP's and DNS's. `prj.conf` raises both (`CONFIG_DNS_NUM_CONCUR_QUERIES`, `CONFIG_NET_MAX_CONN`).
- **POSIX mutexes come from a fixed pool** (`CONFIG_MAX_PTHREAD_MUTEX_COUNT`, 5 by default), and so do condition variables. The core takes 7, mbedTLS's threading layer 7, the HTTP/2 client up to 11 (two per connection, one per session, one for the TLS buffers). Short of them, mbedTLS's own mutexes fail and a TLS handshake ends in `PSA_ERROR_GENERIC_ERROR` when the device signs with its key. `prj.conf` gives 28 mutexes and 12 condition variables, and the core logs it when it finds none left.
- **The port reads a firmware slot back** before it asks MCUboot to try it: a flash driver may report a write it did not do.

## On an ESP32-S3

An ESP32-S3-DevKitC (Xtensa LX7, 512 KB of RAM, 8 MB of flash, WiFi in the chip): the board the ESP-IDF firmware runs on, here with Zephyr's own WiFi driver, MCUboot and TF-PSA-Crypto instead of Espressif's.

![The ESP32-S3 with Zephyr in Flight Control's UI](img/zephyr-device.png)
*The ESP32-S3 with Zephyr in Flight Control: operating system `zephyr`, architecture `xtensa`, the firmware `z26` from its fleet, and the WASM app `alarm` running.*

```bash
third_party/fetch-zephyr.sh
```

```bash
third_party/fetch-zephyr-sdk.sh
```

```bash
BOARD=esp32s3_devkitc/esp32s3/procpu BUILD_DIR=build-esp32s3 port/zephyr/build.sh
```

```bash
ERASE=1 port/zephyr/flash-esp32.sh /dev/cu.usbmodemXXXX .secrets
```

The fetch scripts bring Espressif's HAL module (164 MB), the binary WiFi libraries it needs (under Espressif's licence) and the Xtensa toolchain. The agent firmware uses MCUboot for updates; `flash-esp32.sh` writes MCUboot, the signed application and the credentials with esptool, through the board's UART port, where the console also is (115200). Nothing burns eFuses: MCUboot checks the image's signature in software, and Secure Boot and flash encryption stay off.

On the board it enrolls, runs a signed app, takes its firmware whole or as a delta through MCUboot and goes back from a broken one, downloads over HTTP/2, and answers the UI's Logs tab; what was tried, with figures, is in [RESULTS.md](RESULTS.md#zephyr-on-the-esp32-s3).

For the internal-RAM CoreMark run on the N8R8, the application and MCUboot overlays set `&psram0 { status = "disabled"; };`. In the application board configuration, set `CONFIG_ESP_SPIRAM=n`, omit the octal mode and external heap settings, and set `CONFIG_FC_HEAP_SIZE=140592` and `CONFIG_HEAP_MEM_POOL_SIZE=79872`. The WiFi buffer and thread stack settings are the same as on the ordinary N8R8 build. [CoreMark score and heap measurements](RESULTS.md#coremark)

What it took, beyond the board's two files in `port/zephyr/boards/`:

- **Memory by hand.** Zephyr gives the first core 372 KB for data. The kernel heap is the larger of what the application asks and what the drivers add: the WiFi driver's 51 KB are not added to a size that is set, and with 64 KB there were 28 KB left for thread stacks. It is 92 KiB, the C heap 120,112 B, made smaller for what the PSRAM driver (below), the device's 4 KB log and 1 KB for Zephyr's lines on their way into it take of the region, the status thread's stack 12 KB, the main thread's, where the spec loop runs, 16 KB. The ordinary N8R8 image leaves 4,992 B of the region at link time; what the stacks use is in [RESULTS.md](RESULTS.md#ram).
- **PSRAM for apps and TLS.** The N8R8 has 8 MiB of octal PSRAM. The board configuration gives 2 MiB to the shared heap. WAMR takes app linear memories of 16 KiB or more from this heap when available. On the N8R8 software-crypto build, the TLS pool also prefers this heap for its two 17 KiB receive records. Both allocators fall back to internal RAM when needed. The PSRAM driver costs about 4 KiB of internal RAM. With MQTT connected, the board needs 32 POSIX mutexes for an OTLP client-certificate handshake to complete. [Memory and CoreMark measurements](RESULTS.md#ram)
- **The WiFi driver joins after the request returns**, and chooses the access point itself (`CONFIG_ESP32_WIFI_STA_SCAN_ALL`: the strongest).
- **A kept connection the server closed** is `ENOTCONN` to Zephyr's TCP, not a reset: the port took it for an error and warned at every request, before the core opened another connection as it should.

Zephyr boards go in a fleet of their own (the example is `tools/fleet/zephyr-lab.yaml`). A release is one OCI index with an image for each Zephyr board, named by the board's `CONFIG_FC_FW_PLATFORM`:

```bash
tools/fw-push.py --delta-from <running tag> <registry>/flightctl-mcu/fw:<new tag> esp32s3_devkitc=port/zephyr/build-esp32s3/zephyr/zephyr/zephyr.signed.bin
```

A board must hold the public half of the key the release is signed with; one provisioned with another refuses the fleet's app until its credentials are written again.

Field provisioning ([PROVISIONING.md](PROVISIONING.md#on-zephyr)) needs, on this board: Zephyr's STA+AP usage mode (`CONFIG_WIFI_USAGE_MODE_STA_AP`; with `ESP32_WIFI_AP_STA_MODE` alone the network manager keeps one interface, the access point's address and DHCP server land on the station's, and installers get none), 4 KB of stack for the DHCP server's thread, and no ICMP probe before a lease (with it, the board restarted a second after a client joined).

CI builds the MCUboot agent with the Xtensa toolchain. A separate Simple Boot image runs the WASM GPIO test on the emulated ESP32-S3. Secure Boot and flash encryption remain unverified on Zephyr. [GPIO test results](RESULTS.md#zephyr-esp32-s3-hardware-names) · [Zephyr security](SECURITY.md#zephyr)

## Things to know about native_sim

- **Always `--seed-random`** (`port/zephyr/run.sh` passes it). Without it the random generator starts from a fixed seed, and every instance makes the same device key, hence the same device name.
- **A reboot starts the process again** (`CONFIG_NATIVE_SIM_REBOOT`), with the same flash file, as a board would start again.
- **The time is the host's**: the simulated clock starts there. A board takes it from SNTP (`CONFIG_FC_SNTP_SERVER`).
- **It is not a microcontroller.** Memory figures from it (a peak of about 120 KB of heap with one app, on a 64-bit build) say little about a board.
