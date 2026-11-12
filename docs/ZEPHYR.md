# The agent on Zephyr

The second port of the agent, after ESP-IDF: `port/zephyr` and a small
application in `zephyr/`. The core is unchanged between the two.

**Where it stands.** On `native_sim` (Zephyr built as a Linux process) the
agent passes the end-to-end test of the simulated device, against an
unmodified Flight Control. On a real Raspberry Pi Pico W it enrolls over WiFi,
stays online, runs a signed WASM app and updates its firmware through
MCUboot, whole or as a delta, going back when the new one does not work
(below).

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
zephyr/build.sh
```

Then a flash image with the device's credentials (`deploy/enrollment.sh`
makes them, as for the other devices), and the device itself:

```bash
python3 -c "open('build/zephyr-flash.bin','wb').write(b'\xff'*(2*1024*1024))"
```

```bash
deploy/provision.py --format zephyr --image build/zephyr-flash.bin
```

```bash
zephyr/run.sh build/zephyr-flash.bin
```

It asks to be approved like any device (`flightctl approve
enrollmentrequest/<name>`), and keeps its key, certificate and apps in the
flash file across restarts.

## What is Zephyr's and what is ours

| | |
|---|---|
| Kernel, threads, clock | Zephyr, through its POSIX layer: the core uses pthreads and `clock_gettime` as elsewhere. |
| TLS, X.509 | Zephyr's mbedTLS module, 4.1, the version ESP-IDF 6.0 has; its cryptography is PSA's (TF-PSA-Crypto). |
| Network | Zephyr's sockets; `port/zephyr/fc_net_zephyr.c` gives mbedTLS its `mbedtls_net_*` functions on them. On `native_sim` the sockets are the host's (no TAP interface to set up). |
| Storage | LittleFS on the `fc_storage` flash partition: one file per key, written through a rename. |
| Credentials | The `fc_creds` partition: `"FCC1"`, a length and a JSON object with the fields of the ESP-IDF firmware's partition (`deploy/provision.py --format zephyr`). |
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

`DEVICE_KIND=zephyr ci/e2e.sh` is the end-to-end test of the simulated device
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
BOARD=rpi_pico/rp2040/w BUILD_DIR=build-pico-usb zephyr/build.sh -S cdc-acm-console
```

```bash
zephyr/flash-pico.sh .secrets
```

The first fetches the ARM toolchain (1.9 GB) and the WiFi chip's firmware;
the last writes one UF2 with the application and the credentials (the WiFi
network is the one in `firmware/sdkconfig.wifi`, or `--wifi-ssid`) and prints
the console. The board must be in its bootloader: plugged in with BOOTSEL
held down, or, once it runs this build, `PORT=/dev/cu.usbmodemXXXX`.

### Firmware updates

```bash
BOARD=rpi_pico/rp2040/w/mcuboot BUILD_DIR=build-pico-mcuboot zephyr/build.sh -- -Dzephyr_SNIPPET=cdc-acm-console
```

```bash
BUILD_DIR=build-pico-mcuboot zephyr/flash-pico.sh .secrets
```

```bash
firmware/fw-push.py <registry>/flightctl-mcu/fw:v2 rpi_pico_w=zephyr/build-pico-mcuboot/zephyr/zephyr/zephyr.signed.bin
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
| A delta (8.6 KB for the 734 KB image, 1.2%) | downloaded and the image rebuilt from the running one in 10 s; then as a whole image. `firmwareDownload: delta 8 of 717 KB` |
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
stacks, and 14 KB the WiFi driver and USB take), 110 KB of C heap. With both
TLS connections open and the app running, 34 KB of that heap were free, and
24 KB at the least; with Zephyr 4.5 and mbedTLS 4, 31 KB and 22.5 KB. (Zephyr
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

## Things to know about native_sim

- **Always `--seed-random`** (`zephyr/run.sh` passes it). Without it the
  random generator starts from a fixed seed, and every instance makes the
  same device key, hence the same device name.
- **A reboot starts the process again** (`CONFIG_NATIVE_SIM_REBOOT`), with
  the same flash file, as a board would start again.
- **The time is the host's**: the simulated clock starts there. A board will
  take it from SNTP (`CONFIG_FC_SNTP_SERVER`); that path is written and not
  yet run.
- **It is not a microcontroller.** Memory figures from it (a peak of about
  120 KB of heap with one app, on a 64-bit build) say little about a board.
