# The agent on Zephyr

The second port of the agent, after ESP-IDF: `port/zephyr` and a small
application in `zephyr/`. The core is unchanged between the two.

**Where it stands.** On `native_sim` (Zephyr built as a Linux process) the
agent passes the end-to-end test of the simulated device, against an
unmodified Flight Control. On a real Raspberry Pi Pico W it enrolls over WiFi
and stays online, with almost no memory to spare (below). Not there yet:
firmware updates (they need MCUboot), and apps on the board.

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
| TLS, X.509 | Zephyr's mbedTLS module, 3.6, the version ESP-IDF 5.5 has. |
| Network | Zephyr's sockets; `port/zephyr/fc_net_zephyr.c` gives mbedTLS its `mbedtls_net_*` functions on them. On `native_sim` the sockets are the host's (no TAP interface to set up). |
| Storage | LittleFS on the `fc_storage` flash partition: one file per key, written through a rename. |
| Credentials | The `fc_creds` partition: `"FCC1"`, a length and a JSON object with the fields of the ESP-IDF firmware's partition (`deploy/provision.py --format zephyr`). |
| C heap | `port/zephyr/fc_heap_zephyr.c`, a heap the port owns, so that the device can report how much is free (`memFreeBytes`, `memMinFreeBytes`); Zephyr 4.3's malloc does not say. |
| WASM | WAMR's Zephyr platform, fast interpreter, as in the other builds. |
| Firmware deltas | detools, compiled in as elsewhere; unused until there are firmware updates. |

## Why Zephyr 4.3

`third_party/fetch-zephyr.sh` pins v4.3.2. From v4.4 Zephyr ships mbedTLS 4,
whose API is PSA only; the core is written for 3.6, which ESP-IDF 5.5 also
has. Moving both ports to mbedTLS 4 is a job of its own.

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

The first fetches the ARM toolchain (2.3 GB) and the WiFi chip's firmware;
the last writes one UF2 with the application and the credentials (the WiFi
network is the one in `firmware/sdkconfig.wifi`, or `--wifi-ssid`) and prints
the console. The board must be in its bootloader: plugged in with BOOTSEL
held down, or, once it runs this build, `PORT=/dev/cu.usbmodemXXXX`.

What was seen on the board: it joins the network in 3 s, takes the time from
SNTP, makes its key in 2 s, does a TLS 1.3 handshake in about 9 s, enrolls,
keeps its identity across restarts, and is Online and UpToDate in Flight
Control with its two connections open.

Memory is the limit. Of 264 KB: 114 KB of static data in this build (the
WiFi driver's and the network stack's buffers, the USB console), 71 KB of
kernel heap (the two agent threads' 24 KB stacks, 14 KB the WiFi driver and
USB take), 81 KB of C heap. With both TLS connections open 11 KB of that
heap were free, and it had been down to 4 KB: an app or a firmware update
does not fit yet. Where there is room to find: the stacks (how much of the
24 KB they use is not measured), the USB console (12 KB, only for
development), the WiFi driver's fixed 32 KB of buffers.

What the board taught the port:

- **Flash writes of less than a page do nothing** on this board with Zephyr
  4.3: they report success, and the data is not there. Whole pages work, so
  LittleFS is set to write whole pages (`CONFIG_FS_LITTLEFS_PROG_SIZE=256`).
- **A stack that cannot be had becomes a 1 KB stack.** Zephyr takes a
  thread's stack from the kernel heap when its size is set; if that fails the
  thread keeps the default. The core now checks, and says so instead of
  overflowing.
- **A fatal error restarts the device**, and what it was is kept in RAM and
  logged at the next boot: Zephyr's default is to stop for ever, silently
  when the console is on USB.
- **No entropy source.** The RP2040 has no random generator and Zephyr 4.3
  no driver for its ring oscillator: this build uses Zephyr's test generator,
  and its keys are predictable. To be solved before any real use.
- **One DNS query at a time** is Zephyr's default; the agent's two threads
  ask together at start.

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
