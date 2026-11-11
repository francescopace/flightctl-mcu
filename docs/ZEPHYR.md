# The agent on Zephyr

The second port of the agent, after ESP-IDF: `port/zephyr` and a small
application in `zephyr/`. The core is unchanged between the two.

**Where it stands.** On `native_sim` (Zephyr built as a Linux process) the
agent enrolls, reports its status, takes configuration files and runs signed
WASM apps, managed by an unmodified Flight Control. Not there yet: firmware
updates (they need MCUboot), WiFi, a real board, and a CI job.

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

## Things to know about native_sim

- **Always `--seed-random`** (`zephyr/run.sh` passes it). Without it the
  random generator starts from a fixed seed, and every instance makes the
  same device key, hence the same device name.
- **The time is the host's**: the simulated clock starts there. A board will
  take it from SNTP (`CONFIG_FC_SNTP_SERVER`); that path is written and not
  yet run.
- **It is not a microcontroller.** Memory figures from it (a peak of about
  120 KB of heap with one app, on a 64-bit build) say little about a board.
