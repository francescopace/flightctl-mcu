# Results

What works, where it was verified, and the measurements. The devices are microcontrollers with no Linux, managed by an unmodified **Flight Control 1.4.0** (the upstream of Red Hat Edge Manager) on [kind](https://kind.sigs.k8s.io/). The figures come from development builds before 1.0.

## What works

**Boards**: the classic ESP32, ESP32-C3, C5, C6 and S3 with ESP-IDF, and the ESP32-S3 with Zephyr. **Emulator**: the real firmware in Espressif's emulator ([below](#in-espressifs-emulator)). **Tests**: the end-to-end tests in CI with the simulated device and Zephyr's emulated builds, unit tests and host tests ([TESTING.md](TESTING.md)). ✓ verified, ◐ in part (what is missing is in [current limitations](#current-limitations) or the linked page), – not tried.

| Function | What it does | Boards | Emulator | Tests | More |
|---|---|---|---|---|---|
| Enrollment | Device key, CSR, mTLS | ✓ | ✓ | ✓ | [QUICKSTART](QUICKSTART.md#5-approve-it) |
| Automatic approval | From an allow list | – | – | ✓ | [QUICKSTART](QUICKSTART.md#5-approve-it) |
| Status | Full agent API status, `Unknown` when offline | ✓ | ✓ | ✓ | [Label sync](#label-sync) |
| HTTP/2 | Spec, status and downloads on one connection | ◐ | ✓ | ✓ | [REGISTRY](REGISTRY.md#memory) |
| Resource alerts | CPU, heap, storage against the fleet's rules | ◐ | – | ✓ | [Timings](#timings) |
| Config | Files from the spec, no reboot | ✓ | – | ✓ | [QUICKSTART](QUICKSTART.md#6-give-it-firmware-config-and-an-app) |
| Firmware OTA | A/B slots, staged rollouts | ✓ | ✓ | – | [Timings](#timings) |
| Firmware deltas | A patch from the running release | ✓ | ✓ | ✓ | [DELTA](DELTA.md) |
| Firmware and apps in one spec | The firmware first, then the apps that need it | ✓ | ✓ | ✓ | [APPS](APPS.md#size-and-limits) |
| Automatic rollback | Back to the previous firmware if the new one fails | ✓ | ✓ | – | [The way back](#the-way-back-on-the-esp32-s3-both-ports) |
| Mixed fleets | One fleet for every chip | ✓ | – | – | [DEVICES](DEVICES.md) |
| WASM apps | Installed and updated without a reboot | ✓ | ✓ | ✓ | [APPS](APPS.md) |
| App interface | Hardware by name, timers, events, storage, C library | ◐ | – | ✓ | [APPS](APPS.md#ports) |
| An app's limits | CPU budget per call, heap kept for the agent | ◐ | ✓ | ✓ | [APPS](APPS.md#size-and-limits) |
| Modules built for no device | Any WebAssembly module, run once | ✓ | – | ✓ | [APPS](APPS.md#modules-built-for-no-device) |
| Demo apps | emlearn's TinyML model, wasm3-arduino's blink as published | ◐ | – | ✓ | [APPS](APPS.md#the-blink-of-wasm3-arduino) |
| Apps that crash the device | Not started again | – | ✓ | ✓ | [APPS](APPS.md#how-an-app-runs) |
| Crash reports | `lastFatalError` in the status | ◐ | ✓ | ✓ | [TESTING](TESTING.md#a-crash-in-the-status) |
| Certificate renewal | Same key, new certificate | – | ✓ | ✓ | [TESTING](TESTING.md) |
| Signed images | Cosign signatures checked on the device | ✓ | ✓ | ✓ | [SECURITY](SECURITY.md#two-signing-keys) |
| Registry with TLS and a password | CA and `auth.json` from the fleet | ✓ | ✓ | ✓ | [REGISTRY](REGISTRY.md) |
| Registry tokens | Bearer tokens, as Quay and Harbor use | – | – | ✓ | [TESTING](TESTING.md#registry-tokens) |
| Decommissioning | Key and certificate wiped | ✓ | ✓ | ✓ | [QUICKSTART](QUICKSTART.md#7-take-it-out) |
| Remote console | `flightctl console` and the UI's terminal | ✓ | – | ✓ | [CONSOLE](CONSOLE.md) |
| Device log | The UI's Logs tab, previous boot included | ✓ | – | ✓ | [CONSOLE](CONSOLE.md#the-log) |
| Metrics | OTLP to the Telemetry Gateway | ✓ | – | ✓ | [METRICS](METRICS.md) |
| MQTT for the apps | One broker connection for all apps, TLS | ◐ | – | ✓ | [APPS](APPS.md#mqtt) |
| Known vulnerabilities | SBOM per release, CVEs through Trustify | ✓ | – | – | [SBOM](SBOM.md) |
| Field provisioning | WiFi over the device's access point | ◐ | ✓ | ✓ | [PROVISIONING](PROVISIONING.md) |
| Secure Boot, flash encryption, anti-rollback | The `secure` variant | – | ✓ | – | [SECURITY](SECURITY.md) |

A board keeps its identity across firmware updates; only a full flash erase makes it enroll again as a new device. The ESP32-S2 and the Raspberry Pi Pico W were dropped for their RAM ([support levels](DEVICES.md#support-levels)); before that, both ran enrollment, apps, the registry with TLS and decommissioning, the S2 also CoreMark, and the Pico W deltas, rollback and MCUboot's anti-rollback.

## Measurements

### Firmware size

ESP-IDF 6.0.3, built for size (`-Os`), with WAMR at `-O2`:

| Chip | Image, v3.12 | Free in the 1,664 KB OTA slot | Built for speed (`-O2`), v3.10 |
|---|---|---|---|
| ESP32 | 978 KB | 41% | 1,121 KB |
| ESP32-S3 | 977 KB | 41% | 1,118 KB |
| ESP32-C3 | 1,024 KB | 38% | 1,192 KB |
| ESP32-C5 | 1,107 KB | 33% | 1,282 KB |
| ESP32-C6 | 1,110 KB | 33% | 1,288 KB |

`-Os` saves 141 to 178 KB per image (about 13%). A delta from v3.11 to v3.12 is 49 to 61 KB (4.7–6.2% of the image).

| Component, on the C3 built for speed | Flash |
|---|---|
| WiFi, WPA and lwIP | about 430 KB |
| Cryptography and TLS (mbedTLS 4, TF-PSA-Crypto) | about 197 KB |
| WAMR (fast interpreter) | 68 KB |
| **The agent** (`core/`) | **about 62 KB**; 79 KB with cJSON, WiFi provisioning, the delta decoder and the ESP-IDF port |
| The apps' C library | 7.7 KB |
| WiFi provisioning with its access point, on the S3 | 17.4 KB, against 117.5 KB for Espressif's `network_provisioning` ([PROVISIONING.md](PROVISIONING.md#in-the-field-the-wifi-network)) |
| Floating point in `printf`, Zephyr on the S3 | 5.4 KB, no RAM |

Almost all the space goes to the network stack and to cryptography, which any securely connected device needs anyway.

### RAM

Internal heap with the `alarm` app running; on the ESP32, C5, C6 and S3 after 40 minutes of CoreMark installs and swaps:

| Board | Firmware | Free | Lowest | Largest internal block |
|---|---|---|---|---|
| Classic ESP32 | v3.12 | 154.0 KB | 2.3 KB | 76.0 KB |
| ESP32-C5 | v3.12 | 109.2 KB | 5.3 KB | 60.0 KB |
| ESP32-C6 | v3.12 | 205.2 KB | 1.8 KB | 164.0 KB |
| ESP32-S3 N4R2 | v3.12 | 161.2 KB | – (PSRAM counted) | 84.0 KB |
| ESP32-C3 | v3.13-rc6, an hour after boot | 70 to 75 KB | 2.8 KB, at start-up | 36.0 KB |
| ESP32-S3, Zephyr | before the app interface | about 50 KB | – | – |

| Board | Lowest during a metrics export, every 30 s (v3.13-rc6) | Free once MQTT is connected (v3.14-rc6) |
|---|---|---|
| Classic ESP32 | 79 to 100 KB | 91 KB |
| ESP32-S3 | 93 to 108 KB | 96 KB |
| ESP32-C5 | 26 to 49 KB | 44 KB |
| ESP32-C6 | 25 to 44 KB, WiFi buffers at ESP-IDF's defaults | 141 KB |
| ESP32-C3 | 19 to 20 KB; 13 KB right after it took a whole firmware | 39 KB |
| ESP32-S3, Zephyr (z26) | 8.3 KB of the C heap, 55 KB free, steady | – |

| Part | What it takes |
|---|---|
| Device log | 4 KB |
| Resource alerts | about 4 KB: 65.6 KB free on the C3 five minutes after boot |
| App interface and C library | about 4.5 KB of static RAM; 1.5 KB less since the function tables are const, in flash |
| A WASM app: `trend`, 1.9 KB of `.wasm`, on the C3 | 17.7 KB of heap: the loaded module about 5 KB, the instance and its WAMR stack about 3.5 KB, its memory about 1 KB, its thread's stack 8 KB |
| The MQTT connection, on the C3 | 10 KB of heap (49 KB free before, 39 KB after, v3.14-rc7), and 29 to 31 KB free at the lowest during its handshake; the client's thread used 3.2 to 4.2 KB of its 8 KB stack |
| The metrics connection, Zephyr on the S3 | its 17 KB receive buffer, from the C heap |
| The `tinyml` app's memory | 1.2 KB, with 4.1 KB of `.wasm` |
| The memory of wasm3-arduino's blink, any of its four builds | 64 KB, in one block |

The heap's lowest in the first 25 s after a restart (v3.14-rc7, three restarts each; the block held while a TLS connection opens left out):

| Board | `alarm` alone | `alarm` and `mqttdemo`, MQTT connected |
|---|---|---|
| ESP32-C3 | 51 to 55 KB | 16 to 19 KB |
| ESP32-C5 | 56 to 58 KB | 21 to 24 KB |
| Classic ESP32 | 67 to 69 KB | 33 to 34 KB |
| ESP32-C6 | 151 to 154 KB | 119 to 120 KB |

The difference is what the app and MQTT keep (`mqttdemo`'s instance 19 KB and its thread, the client's 8 KB thread and 4 KB pool), not a handshake beside another: the lowest comes during the first metrics export, before the broker's handshake. With the held block counted, the heap's own lowest is 1 to 10 KB on every chip, with or without MQTT. On the C3 with MQTT connected, a metrics export goes down to 6 to 10 KB, and the first one or two after a restart fail for want of memory (a 4.4 KB allocation); the later ones are sent. The C5 goes down to 9 to 15 KB and sends them. With Zephyr on the ESP32-S3, 4,112 bytes of the 372 KB DRAM segment are left at link time, with a 12 KB status-thread stack and a 92 KB kernel heap.

Stack used at most (`stackMinFreeBytes`), of the stack each thread has; here a KB is 1,024 bytes:

| Firmware | Spec loop, on the main task | Status thread | App thread |
|---|---|---|---|
| Classic ESP32 | 8.9 of 24 KB (installing an app) | 4.2 of 12 KB | 3.4 of 8 KB |
| Zephyr on the Pico W | 9.2 of 14 KB (downloading firmware over TLS, whole or as a delta); 4.6 polling | 3.8 of 8 KB | 2.2 of 8 KB |
| Zephyr on the ESP32-S3 | 5.4 of 16 KB (polling, no firmware download) | 4.7 KB (now given 12 KB; no certificate renewal) | 3.0 of 8 KB (`alarm`) |
| Zephyr on a Cortex-M3 (`mps2/an385` in QEMU) | | | 2.7 of 8 KB (`printf` of `%f`); 2.5 without the C library |
| ESP32-C3 in the emulator, v3.14-rc4 | | | 2.8 of 8 KB (`printf` and `snprintf` of `%f`) |

Installing an app or downloading firmware takes about twice the stack of polling. The C3 with the `alarm` app uses 1% of its CPU.

### CoreMark

Iterations per second of [CoreMark](https://github.com/eembc/coremark) 1.0's two published WebAssembly builds, unmodified, on WAMR's fast interpreter ([APPS.md](APPS.md#modules-built-for-no-device)). ESP-IDF v3.13 built for size unless marked; in brackets, v3.10 built for speed (`-O2`):

| Board | wasm3's build (64 KB of memory) | wasm-coremark's build (128 KB) | Native, one core (Espressif) |
|---|---|---|---|
| Classic ESP32 (240 MHz) | 14.19; v3.12: 14.12–14.27 (15.16–15.30) | does not fit: largest free block 64–76 KB | about 505 |
| ESP32-C3 (160 MHz) | 14.46, right after a restart only (14.16–14.22) | does not fit: largest free block 36–38 KB | about 409 |
| ESP32-C6 (160 MHz) | 11.81–11.98 (12.22–12.26) | 13.24–13.71 (14.83) | |
| ESP32-C5 (240 MHz) | 19.46; v3.12: 18.86–22.34, once 14.81 (22.04–22.39) | does not fit: largest free block 72 KB | |
| ESP32-S3, PSRAM off (240 MHz) | not tried since v3.10 (18.88–18.95) | does not fit: no internal block of 128 KB left once running (155 KB after boot) | about 614 |
| ESP32-S3 N4R2 (2 MB quad PSRAM) | 17.63–18.08 (18.89–19.02) | 21.04–21.64, in PSRAM (22.89–22.96) | about 614 |
| ESP32-S3 N8R8 (8 MB octal PSRAM), `esp32s3-r8` image | not tried since v3.10 (18.93) | not tried since v3.10 (22.89–22.95, in PSRAM) | about 614 |
| ESP32-S3 with Zephyr, no PSRAM | does not fit (z25): the C heap (123 KB) has no block for the module and 64 KB | – | |
| ESP32-S3 N8R8 with Zephyr | 17.83–17.84, in PSRAM (z26) | 21.69–21.70, in PSRAM (z26); with z24 and z25 the 12.5 KB module did not load, the C heap out of memory | about 614 |

| | |
|---|---|
| Against native code | about 30 times slower, the instruction count included; WAMR's AOT mode should be about 3 times slower, with one module per architecture instead of one for every device; not measured |
| Firmware built for size, WAMR still at `-O2` | about 5% slower on the S3, 7% on the classic ESP32; on v3.10 the C5 and C6 lost nothing (22.3 and 12.2 either way) |
| Classic ESP32 at 240 MHz | 2.2 times faster than at ESP-IDF's debug default of 160 MHz |
| Block needed for the memory | one free block of 68 KB for 64 KB, of 136 KB for 128 KB |
| ESP32-S3 with PSRAM | an app's memory goes to PSRAM when no internal block is large enough: the `esp32s3` image for quad PSRAM or none, `esp32s3-r8` for octal ([DEVICES.md](DEVICES.md)) |
| Zephyr on the S3 N8R8 | the octal PSRAM is a heap of its own for app memories of 16 KB or more; its driver takes 4 KB of internal RAM |

By release, wasm3's build:

| Release | Classic ESP32 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-S3 N4R2 |
|---|---|---|---|---|---|
| v3.12 | 14.12–14.27 | | about 19.7 or about 22.3 from one run to the next, once 14.8, not explained yet | 11.45–11.98 | |
| v3.13 | 14.19 | 14.46 | 19.46, one run | 11.81–11.98, four runs; wasm-coremark's build 13.24 and 13.71 | 17.63–18.08 |
| v3.14-rc4, one run | 14.22 | 14.27 | 19.65 | 11.75 | 17.56 |

Whether the 68 KB block is there depends on how broken up the heap is:

| Release | The 64 KB build installed, or swapped for itself without a restart |
|---|---|
| v3.12 | 3 installs from the `alarm` app and 4 swaps per board: one swap failed on the classic ESP32 (largest free block 48 KB), one install on the C5 (64 KB); the C6 and the S3 passed every one, and again with v3.13 |
| v3.13 | there once in two tries on the classic ESP32 (else 64 KB the largest of 138 KB free) and on the C5 (else 44 KB); once in five on the C3, right after a restart (else 48 to 50 KB of 50 to 87 KB free, 22 KB after a swap); the same without metrics |
| v3.14-rc4, with the 32 KB kept for the agent | taken and run (a 7.7 KB `.wasm`) on the classic ESP32, C3, C5, C6 and S3, its instance leaving 85 to 96 KB of internal heap (52 KB on the S3), no TLS handshake failed; right after a swap from another app missing on the ESP32, C3 and C5 (largest 28 to 44 KB), there after a restart |
| v3.14-rc6 | missing right after another app and after a remove then install, on the C3, C5 and classic ESP32 (largest 36 to 56 KB), each later try finding the same block; on the classic ESP32 found but refused, leaving the agent 11 to 17 KB. In all CoreMark takes about 105 KB: the loaded module about 18 KB, the thread's stack 8.7 KB, the instance about 71 KB. With the agent connected and no app, about 103 KB are free on the C3, 107 KB on the C5, 117 KB on the classic ESP32: with the 32 KB kept for the agent, 20 to 34 KB short. After a restart it fits because its instance comes before the agent connects (150 to 168 KB free then) |
| Lowest heap seen | 2.3 KB on the classic ESP32, the agent still reporting; 108 bytes on the C3 after a failed swap, where one TLS handshake failed (`PSA_ERROR_INSUFFICIENT_MEMORY`) |

### Timings

ESP-IDF boards with an early build, unless the row says otherwise:

| Operation | Time |
|---|---|
| From boot to enrollment request (WiFi, SNTP, TLS) | about 10 s |
| Config change, from apply to device confirmation | 1–6 s |
| Firmware rollout to 3 devices, canary first | 50–80 s, about 20 s per device for download and reboot |
| Rollback of a broken firmware | about 3.5 min: the 150 s confirmation window and 2 reboots |
| WASM app update across the fleet, canary first | about 50 s, nearly all of it the rollout; the swap on the device is immediate |
| App state change, to Flight Control | about 2 s |
| Memory rule added to the fleet, to `Warning` and `DeviceMemoryWarning` (end-to-end test) | 21 s; `DeviceMemoryNormal` once the rule is removed |
| An app call's budget of 5 million instructions, in a counting loop (v3.14-rc1) | 0.66 s on the C5, 0.9 s on the S3, 1.06 s on the C3, 1.2 s on the classic ESP32, 1.3 s on the C6 |
| MQTT connected, from the spec that gives the broker (v3.14-rc6) | within 70 s |
| Decommissioning, from the request to `Completed` | 2.4 to 3 s |

### Idle traffic

Bytes on the wire, TLS included (`netTxBytes`, `netRxBytes`), with the earlier HTTP/1.1 client and its two kept connections; not measured yet over HTTP/2:

| | Traffic |
|---|---|
| Simulated device, no app, a status every minute | about 220 KB/hour: 87 KB sent, 130 KB received |
| ESP32-C3 with the `alarm` app | about 280 KB/hour |
| A connection per request instead | 560 to 730 KB/hour, mostly the TLS 1.3 handshake with mTLS |
| Exchanges | a status at once on a change, else every 2 minutes; the long-poll lasts about 4 minutes |
| TLS handshakes | 2 after boot, none in the next 10 minutes |
| A reconnection | a full handshake, about 1.5 KB received: the server issues session tickets but does not resume them |
| After a server restart | both connections reopen; the next spec applies in 2 s |

What is left is the full status, and going lower needs differential updates (limitation 6). The apps' changes go out together, one status every 15 s at most.

### In Espressif's emulator

[esp-emu](https://github.com/espressif/esp-emulator) 0.51 runs the real firmware for the C3, C5, S3 and C6 against the same Flight Control, here with 4-minute certificates ([TESTING.md](TESTING.md)):

| Chip | Signed OTA (reboot into the other slot) | Unsigned app | Certificate renewal | Heap free / minimum / largest block |
|---|---|---|---|---|
| ESP32-C3 | confirmed, 19 s | refused | 6 renewals, still reporting after the first certificate expired | 57.8 / 37.6 KB / – |
| ESP32-C5 | confirmed, 15 s | refused | renewed, still reporting after the first certificate expired | 61.7 / 42.2 / 40.9 KB |
| ESP32-S3 | confirmed, 35 s | – | renewed | 121.3 / 102.1 / 73.7 KB |

The C3 figures match the board's.

### ESP-IDF 6.0.3 on a board (ESP32-S3)

An ESP32-S3-DevKitC with the debug build (mbedTLS 4.1.1, cryptography through PSA), against Flight Control 1.4.0:

| Step | Result |
|---|---|
| Enrollment | key generated on the chip, TLS 1.3 with the enrollment certificate, management certificate for a year |
| Config and WASM app | config file written, the signed `alarm` app installed and running |
| Firmware, whole image | 1181 KB downloaded in 5 s into the other slot, restarted, confirmed |
| Firmware, as a delta | the release build (1000 KB) from a delta of 511 KB, rebuilt from the running image in 13 s, restarted, confirmed |
| Memory (release build, app running) | 145 KB of heap free, 124 KB at the lowest |

The delta is half of the image because it goes from the debug build to the release one; between builds of the same kind it is 1 to 12% ([DELTA.md](DELTA.md)).

### The way back on the ESP32-S3, both ports

Each port was given a firmware that reaches Flight Control on a wrong port (9443) and gives itself 30 seconds instead of 150:

| | ESP-IDF 6.0.3 | Zephyr 4.5 with MCUboot |
|---|---|---|
| Before | the release build, received as a delta of 512 KB (`lastDelta: Applied`) | a build received as a delta of 28 KB (`lastDelta: Applied`) |
| Broken image | 1181 KB, whole, downloaded and restarted in 11 s | 885 KB, whole, in 14 s |
| Given up | 30 s after the agent started, by the image itself | the same |
| Back | the bootloader takes the previous slot | MCUboot swaps the slots back: 15 s |
| Reported | 53 s after the fleet changed: "failed to start and was rolled back; not retried" | 1 min 53 s, the same message |

Neither tried the image again. The ESP32-C6 was back and reporting 50 s after it restarted into a broken firmware, and with v3.13 online again 3 to 5 s after the restart back.

### A crash in the status

What `lastFatalError` gives after each kind of crash is in [TESTING.md](TESTING.md#a-crash-in-the-status). The handler takes 799 bytes of IRAM on the ESP32, 907 on the S3 and 1,102 on the C3, C5 and C6; the record takes 140 bytes of RAM kept across restarts.

### The registry with TLS, and decommissioning, on boards

With the registry behind TLS and a password, its CA and `auth.json` from the fleet ([REGISTRY.md](REGISTRY.md)):

| | ESP32-C3, ESP-IDF 6.0.3 | Pico W, Zephyr | ESP32-S3, Zephyr |
|---|---|---|---|
| App over TLS | running 3 s after the config, no restart | about 6 s | about 6 s |
| Firmware over TLS, whole | 1234 KB in 5 s | 729 KB in 24 s | 886 KB |
| Firmware over TLS, as a delta | not tried | not tried | 17 KB (`Applied`) |
| Decommissioning, from request to Completed | 3 s | 2.5 s | 2.4 s |

After decommissioning each board wiped its key and certificate, showed as `Decommissioned` and kept running its app without enrolling again; erased, each enrolled again as a new device. The heap during a download over TLS is not measured yet.

## Label sync

Flight Control 1.4 copies every entry of `customInfo` into the device's labels, and these devices report counters there. With the default mapping, each status update logged two label events: 227 of 400 events on the test cluster while the tests ran. [QUICKSTART.md](QUICKSTART.md) applies `tools/fleet/labelsync-custom-info.yaml`, which copies only what stays the same, and the agent keeps its status still ([details](UPSTREAM.md#label-sync-in-detail)). The simulated device, with no app, over 9 minutes (a status a minute):

| | Events |
|---|---|
| Counters and the condition's time new at every status | 10, one per status |
| The condition's time kept, counters once an hour | 2: at the start, and when the counters were read again five minutes later; then one an hour |

## Flight Control limitations found (upstream proposals)

Each in short; the source references, how it was found and the workarounds are in [UPSTREAM.md](UPSTREAM.md).

1. **`matchExpressions` in the fleet selector is accepted but ignored.** The fleet stays empty with no error. Proposal: support it, or reject it at validation.
2. **Batch success is based only on `renderedVersion`.** An agent that reports the new version before its firmware is confirmed makes the rollout advance. Proposal: write this into the agent API contract, or also check `os.image` and the `Updating` condition.
3. **Fixed 5-minute disconnect timeout** (`DeviceDisconnectedTimeout`). Battery devices in deep sleep would be `Unknown` most of the time. Proposal: make it configurable, or let the device declare its contact interval.
4. **No way to declare the device class or the supported runtimes.** The server accepts `compose`, `quadlet` or `helm` apps for a microcontroller, and the error shows only on the device. Proposal: let the device report its runtimes, such as `applicationRuntimes: [wasm]`, and validate against them.
5. **No free-form status field per app.** An app's state has to go in `customInfo`. Proposal: a field such as `message` or `info` in `DeviceApplicationStatus`.
6. **Cost of each status update.** Every PUT sends the full status, 2–3 KB of JSON. Proposal: document PATCH as the path for constrained devices, or add a heartbeat-only variant.
7. **The server compares `os.image` only as a string.** Re-pushing the same tag is not seen as an update. Proposal: recommend digest references, or compare `imageDigest`.
8. **An enrollment certificate cannot be revoked.** A leaked one can enroll new devices until it expires. Proposal: a revocation list, or refuse certificates whose request was deleted or denied. Fixed on Flight Control's main branch, not in 1.4.0.
9. **A device with an RSA key cannot enroll.** The internal CA signs with the request's algorithm, which fails with its ECDSA key. Proposal: let the CA sign with its own algorithm.
10. **Since 1.4, a change in the status is a label event.** Every status change asks for a label reconciliation, even when no label changes. Proposal: reconcile only when a mapping's inputs change, and do not copy all of `customInfo` by default.
11. **A fleet's OS image cannot come from the software catalog.** `spec.os.catalogItemRef` is accepted, but the device gets no OS image and no error. Proposal: copy `os.catalogItemRef`, as is done for applications.
12. **An inline file given in base64 reaches the device percent-encoded.** `aGk=` arrives as `aGk%3D`. Proposal: unescape the data URL before serving it as base64.
13. **A device cannot enroll again under its own name.** A device whose certificate expired while offline must enroll under a new name. Proposal: let an enrollment certificate re-enroll a known device, with the same approval.
14. **A withdrawn advisory stays on the devices.** Findings that Trustify no longer returns are never removed. Proposal: replace a digest's findings with what the scan returns.
15. **A console session whose user left before the device came stays listed.** The device then asks for its spec every few seconds, for good. Proposal: remove a session when its user leaves, and give sessions a lifetime.

## Current limitations

- **Registry**: TLS only, with the CA the fleet gives; no public CA bundle, no redirects to another host ([REGISTRY.md](REGISTRY.md)).
- **Device key**: in flash in clear, except in the `secure` variant, where it is encrypted, or on the C5 in its ECDSA peripheral ([SECURITY.md](SECURITY.md)).
- **Provisioning**: credentials over USB; the WiFi network over USB or the device's own access point ([PROVISIONING.md](PROVISIONING.md)).
- **Enrollment certificates**: issued per batch, but Flight Control 1.4.0 cannot revoke one (limitation 8).
- **Offline longer than the certificate** (a year by default): the device enrolls again as a new device, with its apps, config and firmware (limitation 13).
- **Server unreachable**: without an accepted status for 10 minutes the device restarts, its apps with it, and again every 10 minutes; a write gives up after 30 s.
- **App format**: WASM OCI artifacts only (`application/vnd.wasm.content.layer.v1+wasm`), not a `.wasm` inside a container image.
- **JSON depth**: at most 32 levels; real documents use fewer than 10.
- **Not yet on a board**: the `secure` variant, certificate renewal, registry tokens, GPIO interrupts, wired peripherals; with Zephyr, `lastFatalError`.
- **Zephyr**: an app's heap floor and the `.wasm` size limit are not checked yet.
- **MQTT with Zephyr**: beside a metrics export every 30 s it used up the ESP32-S3's C heap (z28), and the device restarted itself after 10 minutes without a status; z29 makes both wait for heap, not yet tried on the board ([APPS.md](APPS.md#mqtt)).
- **Zephyr on the ESP32-S3 without PSRAM**: z28 with one app got down to 368 bytes of free C heap while trying to download an update, which did not go through, and the device then took no spec while its statuses went out ([ZEPHYR.md](ZEPHYR.md#on-an-esp32-s3)).
- **A stuck agent**: no status accepted, or no spec taken, for 30 minutes restarts the device from outside the agent's threads, on ESP-IDF and Zephyr; tried in unit tests only.
- **Memory for apps**: CoreMark's 64 KB fits on every ESP-IDF board and, in PSRAM, on the Zephyr S3 N8R8; 128 KB only on the C6 and the S3 with PSRAM. On the classic ESP32, C3 and C5 the 68 KB block can be missing right after another app ([CoreMark](#coremark)).

What comes next is in the [roadmap](ROADMAP.md).
