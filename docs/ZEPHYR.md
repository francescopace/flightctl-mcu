# The agent on Zephyr

The second port of the agent, after ESP-IDF: `port/zephyr` and a small
application in `zephyr/`. The core is unchanged between the two.

**Where it stands.** On `native_sim` (Zephyr built as a Linux process) the
agent passes the end-to-end test of the simulated device, against an
unmodified Flight Control. On a real Raspberry Pi Pico W it enrolls over WiFi,
stays online, runs a signed WASM app and updates its firmware through
MCUboot, whole or as a delta, going back when the new one does not work
(below). On a real ESP32-S3 it does the same: the first chip to run both
ports.

## Try it

Needs Docker. The first command pulls Zephyr's build image (about 10 GB) and
a Zephyr tree with the modules the port uses (about 900 MB, in
`third_party/zephyr-ws`).

```bash
third_party/fetch-wamr.sh
```

```bash
third_party/fetch-detools.sh
```

```bash
third_party/fetch-zephyr.sh
```

```bash
port/zephyr/build.sh
```

Then a flash image with the device's credentials (`tools/enrollment.sh`
makes them, as for the other devices), and the device itself:

```bash
python3 -c "open('build/zephyr-flash.bin','wb').write(b'\xff'*(2*1024*1024))"
```

```bash
tools/provision.py --format zephyr --image build/zephyr-flash.bin
```

```bash
port/zephyr/run.sh build/zephyr-flash.bin
```

It asks to be approved like any device (`flightctl approve
enrollmentrequest/<name>`), and keeps its key, certificate and apps in the
flash file across restarts.

## What is Zephyr's and what is ours

| | |
|---|---|
| Kernel, threads, clock | Zephyr, through its POSIX layer: the core uses pthreads and `clock_gettime` as elsewhere. |
| TLS, X.509 | Zephyr's mbedTLS module, 4.1, the version ESP-IDF 6.0 has; its cryptography is PSA's (TF-PSA-Crypto), with its threading layer on POSIX mutexes (`include/fc-tf-psa-user.h`), which Zephyr has no option for: the agent's threads use PSA at the same time. |
| Network | Zephyr's sockets; `port/zephyr/fc_net_zephyr.c` gives mbedTLS its `mbedtls_net_*` functions on them. On `native_sim` the sockets are the host's (no TAP interface to set up). |
| Storage | LittleFS on the `fc_storage` flash partition: one file per key, written through a rename. |
| Credentials | The `fc_creds` partition: `"FCC1"`, a length and a JSON object with the fields of the ESP-IDF firmware's partition (`tools/provision.py --format zephyr`). |
| C heap | `port/zephyr/fc_heap_zephyr.c`, a heap the port owns, so that the device can report how much is free (`memFreeBytes`, `memMinFreeBytes`); Zephyr's malloc does not say. |
| WASM | WAMR's Zephyr platform, fast interpreter, as in the other builds. |
| Firmware updates | MCUboot's two slots (boards built with it): the image is streamed into the second, read back, and asked for as a test; MCUboot checks its signature, swaps the slots, and swaps them back unless the agent confirms the image once it has reached Flight Control. `native_sim` has no MCUboot and no updates. |
| Firmware deltas | detools, in the core as elsewhere; the port reads the running image from the first slot. |

## Zephyr 4.5

`third_party/fetch-zephyr.sh` pins v4.5.0, which has mbedTLS 4.1.1, the same
as ESP-IDF 6.0.3: the core is written for mbedTLS 4, whose cryptography is
PSA only. (The port was first made on Zephyr 4.3, the last release with
mbedTLS 3.6, when the core was written for that.)

## What the port taught the core

- **One `pthread_attr_t` per thread, kept until the thread is joined.** On
  Zephyr the attribute object owns the thread's stack: the core used one for
  its two threads (the second took the first one's stack, which never ran),
  and destroyed an app thread's right after creating it.
- **Thread stacks by platform.** A 64-bit host needs more than a
  microcontroller, and `native_sim` is a 64-bit host that gives a thread
  exactly what it asks for.

## What the tests check

`DEVICE_KIND=zephyr tests/e2e/e2e.sh` is the end-to-end test of the simulated device
with the Zephyr build as the device, and CI runs it (a job builds the
application in Zephyr's image, another runs it against Flight Control on
kind): enrollment and approval, status, a config file and its change, a WASM
app installed, updated in place and removed, an unsigned app refused, an
update while the registry is down, a second app, the renewal of the
management certificate, a server restart, and a device whose certificate
expired while the server was down, which enrolls again under a new key after
rebooting itself.

One check is weaker than on the host: that a config file was stored is read
from the device's log, as the file is inside the flash image.

## On a Raspberry Pi Pico W

An RP2040 (Cortex-M0+, 264 KB of RAM, 2 MB of flash) with a CYW43439 for
WiFi: less RAM than any ESP32 the agent runs on.

```bash
third_party/fetch-zephyr-sdk.sh
```

```bash
BOARD=rpi_pico/rp2040/w BUILD_DIR=build-pico-usb port/zephyr/build.sh -S cdc-acm-console
```

```bash
port/zephyr/flash-pico.sh .secrets
```

The first fetches the ARM toolchain (1.9 GB) and the WiFi chip's firmware;
the last writes one UF2 with the application and the credentials (the WiFi
network is the one in `port/espidf/sdkconfig.wifi`, or `--wifi-ssid`) and prints
the console. The board must be in its bootloader: plugged in with BOOTSEL
held down, or, once it runs this build, `PORT=/dev/cu.usbmodemXXXX`.

No provisioning in the field on this board: Infineon's driver in Zephyr 4.5
does not open an access point on the CYW43439. Two faults, found on the board:
- WHD's `CH20MHZ_CHSPEC` decides the band by comparing the whole chanspec
  Zephyr's driver passes (channel, band and bandwidth bits) with 14, the last
  2.4 GHz channel, so any channel is taken for 5 GHz, which the chip refuses
  (`whd_wifi_set_chanspec failed`). Comparing the channel alone fixes it
  (`CHSPEC_CHANNEL(chspec) <= CH_MAX_2G_CHANNEL` in `whd_chip_constants.h`).
- With that fixed, the access point starts, then the driver's next request to
  the chip (`event_msgs`, for the access point's events) gets no answer in 5 s,
  and the driver closes the access point again.

The board keeps the network given at the factory, and builds without a DHCP
server, which would take 2 KB of its RAM.

### Firmware updates

```bash
BOARD=rpi_pico/rp2040/w/mcuboot BUILD_DIR=build-pico-mcuboot port/zephyr/build.sh -- -Dzephyr_SNIPPET=cdc-acm-console
```

```bash
BUILD_DIR=build-pico-mcuboot port/zephyr/flash-pico.sh .secrets
```

```bash
tools/fw-push.py <registry>/flightctl-mcu/fw:v2 rpi_pico_w=port/zephyr/build-pico-mcuboot/zephyr/zephyr/zephyr.signed.bin
```

The board `rpi_pico/rp2040/w/mcuboot` is Zephyr's layout for MCUboot on the
Pico: the bootloader in the first 64 KB, two slots of 832 KB (the application
is 723 KB), and the last 320 KB for the credentials and the storage, where
the build without MCUboot has them too. The build makes MCUboot and the
application together (sysbuild) and signs the application with
`.secrets/mcuboot-signing.pem`, an ECDSA P-256 key made the first time:
MCUboot starts only images signed with it. The first flash writes MCUboot,
the application and the credentials; after that a release is the signed
image, pushed for the platform the board's configuration names
(`CONFIG_FC_FW_PLATFORM`), and `spec.os.image` in the fleet, as on ESP32.

Seen on the board, from one release to the next (with Zephyr 4.3; the last
two rows with 4.5):

| | |
|---|---|
| A whole image (734 KB) | downloaded and written in 15 s; the device restarts, MCUboot swaps the slots, and the new firmware is confirmed 38 s after its start, once Flight Control has taken its status |
| The same after a restart | still the new firmware: the confirmation is kept |
| A firmware that cannot reach Flight Control | given up after its 30 s (150 by default): the device restarts, MCUboot puts the previous firmware back, and that one reports the failure and does not try the image again |
| A delta (8.6 KB for the 734 KB image, 1.2%) | downloaded and the image rebuilt from the running one in 10 s; then as a whole image. Reported then as `firmwareDownload: delta 8 of 717 KB`; now as `lastDelta: Applied` |
| From the Zephyr 4.3 build to the 4.5 one (and mbedTLS 3.6 to 4.1) | over the air, by the MCUboot already on the board: a delta of 232 KB for the 723 KB image (33%: little of the code is where it was). The device kept its identity, certificate and app |
| A delta between two 4.5 builds (7.5 KB, 1.0%) | downloaded in 9 s; confirmed, and still the new firmware after a restart |

Not done: MCUboot's own protection against older images (the device takes
any image signed with the key), and MCUboot's console (it logs on the UART
pins, not on USB).

What was seen on the board: it joins the network in 3 s, takes the time from
SNTP, makes its key in 2 s, does a TLS 1.3 handshake in about 9 s, enrolls,
keeps its identity across restarts, and is Online and UpToDate in Flight
Control with its two connections open. Given a fleet, it stores the config
file, downloads the signed WASM app from the registry and runs it.

Memory, of 264 KB: 111 KB of static data in this build (the WiFi driver's and
the network stack's buffers, the USB console), 45 KB of kernel heap (thread
stacks, and 14 KB the WiFi driver and USB take), 108 KB of C heap. With both
TLS connections open and the app running, 34 KB of that heap were free, and
24 KB at the least; with Zephyr 4.5 and mbedTLS 4, 31 KB and 22.5 KB; with
mbedTLS's threading layer (release z9, 2 KB less heap), 29.7 KB, and 15 KB at
the least. (Zephyr
4.5 gives the WiFi driver's twenty packet buffers 2400 bytes each, 16 KB more
than there is: the build keeps them at 1600, as they were.) It fits because the stacks are sized from what was
measured on the board: the agent's threads use 5.6 KB at most (installing an
app) and get 10 KB, not the 24 KB the core asks for by default; the main
thread uses 3 KB and gets 8 KB.

Open on this board:

- **The entropy source is not a certified one** (below).
- **The console on USB loses the end of long lines** with Zephyr 4.5's USB
  stack: lines were seen cut after 64 bytes. Not looked into.
- **Never run there**: certificate renewal, days of uptime.

What the board taught the port:

- **Flash writes of less than a page did nothing** on this board with Zephyr
  4.3.2: they reported success, and the data was not there. LittleFS lost
  what it wrote, and MCUboot could not mark a slot. The cause was in Zephyr's
  code for the RP2040's flash: the function that selects the chip was called
  with a value that always means "not selected". Four lines, which this
  repository patched while it was on Zephyr 4.3; Zephyr has them right from
  4.4. LittleFS still writes whole pages here
  (`CONFIG_FS_LITTLEFS_PROG_SIZE=256`), as it was tried, and the port still
  reads a firmware slot back before it asks MCUboot to try it.
- **A stack that cannot be had becomes a 1 KB stack.** Zephyr takes a
  thread's stack from the kernel heap when its size is set; if that fails the
  thread keeps the default. The core now checks, and says so instead of
  overflowing.
- **A fatal error restarts the device**, and what it was is kept in RAM and
  logged at the next boot: Zephyr's default is to stop for ever, silently
  when the console is on USB.
- **Entropy from the ring oscillator.** The RP2040 has no random generator
  and Zephyr no entropy driver for it (its driver for Raspberry Pi chips is
  for the RP2350). The port has one (`port/zephyr/fc_entropy_rp2.c`, the node
  in the board's overlay): the chip's ring oscillator, sixteen samples folded
  into each bit, which gives nothing if the oscillator returns the same bit
  every time. PSA (TLS, the device key) and the rest of Zephyr (TCP sequence
  numbers, DNS query ids) seed their generators from it. It is what the Pico
  SDK does too, and it is not a certified source.
- **The chip joins any access point of the network, not the nearest.** On a
  network carried by three access points, the board joined a far one (-75
  dBm, against -42 for the near one) on 4 boots of 20, and on each of those
  name lookups and handshakes timed out. Infineon's driver takes no access
  point's address, and asking the chip to prefer the strongest changed
  nothing, so `main.c` gives up a join weaker than -60 dBm and joins again,
  up to five times. Where every access point is weak, the last join stays.
- **One DNS query at a time** is Zephyr's default; the agent's two threads
  ask together at start. **Four connections in all** is another: the two to
  Flight Control, DHCP and DNS use them up, and the one to the registry was
  refused.

## On an ESP32-S3

An ESP32-S3-DevKitC (Xtensa LX7, 512 KB of RAM, 8 MB of flash, WiFi in the
chip): the board the ESP-IDF firmware runs on, here with Zephyr's own WiFi
driver, MCUboot and TF-PSA-Crypto instead of Espressif's.

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

The fetch scripts bring Espressif's HAL module (164 MB), the binary WiFi
libraries it needs (under Espressif's licence) and the Xtensa toolchain. The
build is always with MCUboot, which is how Zephyr starts this chip when it is
to be updated; `flash-esp32.sh` writes MCUboot, the signed application and
the credentials with esptool, through the board's UART port, where the
console also is (115200). Nothing burns eFuses: MCUboot checks the image's
signature in software, and Secure Boot and flash encryption stay off.

Tried on the board, against Flight Control 1.4.0:

| | |
|---|---|
| Enrollment | key made by PSA from the chip's random generator, certificate for a year |
| WASM app | the signed `alarm` app runs (WAMR's fast interpreter on Xtensa); the Pico's copy, signed with another key, was refused |
| Firmware, whole | 885 KB in 7 s, 24 s from the restart to the agent, MCUboot's swap included; confirmed |
| Firmware, as a delta | 19 KB (2.2%) rebuilt the 885 KB image in 8 s; confirmed |
| A firmware that cannot reach Flight Control | downloaded whole in 14 s; given up after its 30 s; MCUboot put the previous firmware back (a 15 s swap), which reported the failure 1 min 53 s after the fleet changed, and did not try the image again |
| Reflashed over USB | same device: its key is in the storage partition |
| Memory | 49 KB of the 128 KB C heap free with the app and both connections up, 40 KB at the lowest |

What it took, beyond the board's two files in `port/zephyr/boards/`:

- **Memory by hand.** Zephyr gives the first core 372 KB for data. The kernel
  heap is the larger of what the application asks and what the drivers add:
  the WiFi driver's 51 KB are not added to a size that is set, and with
  64 KB there were 28 KB left for thread stacks. It is 96 KB now, the C heap
  128 KB, the agent's stacks 16 KB each: 98% of the region.
- **The WiFi driver joins after the request returns**, and chooses the access
  point itself (`CONFIG_ESP32_WIFI_STA_SCAN_ALL`: the strongest). The Pico's
  way of giving up a weak join is for Infineon's driver only.
- **A kept connection the server closed** is `ENOTCONN` to Zephyr's TCP, not
  a reset: the port took it for an error and warned at every request, before
  the core opened another connection as it should.

The two boards are in one fleet (`tools/fleet/zephyr-lab.yaml`), served by
one release with an image for each:

```bash
tools/fw-push.py --delta-from z2 <registry>/flightctl-mcu/fw:z3 rpi_pico_w=port/zephyr/build-pico-mcuboot/zephyr/zephyr/zephyr.signed.bin esp32s3_devkitc=port/zephyr/build-esp32s3/zephyr/zephyr/zephyr.signed.bin
```

Each took its own image from it, as a delta: 20 KB on the Pico W (2.9%), 9 KB
on the ESP32-S3 (1.0%). Both must hold the public half of the key the
release is signed with: the Pico W had been provisioned with another, and
refused the fleet's app until its credentials were written again.

Field provisioning ([PROVISIONING.md](PROVISIONING.md#on-zephyr)): with a
proof of possession and no network in its credentials, the board opened
`FC-E8EC00` on channel 6 three seconds after boot, with the provisioning
server listening, and the access point went away once the network was
written back. Release z10, which has it, came to both boards as a delta
(72 KB here, 56 KB on the Pico W) and changed nothing for a board with its
network: both were Online with the app running about 90 s after the fleet
changed. Not tried on the board: an installer joining the access point.

Not tried: Secure Boot or flash encryption under Zephyr. Not in CI: the
build needs the Xtensa toolchain.

## Things to know about native_sim

- **Always `--seed-random`** (`port/zephyr/run.sh` passes it). Without it the
  random generator starts from a fixed seed, and every instance makes the
  same device key, hence the same device name.
- **A reboot starts the process again** (`CONFIG_NATIVE_SIM_REBOOT`), with
  the same flash file, as a board would start again.
- **The time is the host's**: the simulated clock starts there. A board
  takes it from SNTP (`CONFIG_FC_SNTP_SERVER`).
- **It is not a microcontroller.** Memory figures from it (a peak of about
  120 KB of heap with one app, on a 64-bit build) say little about a board.
