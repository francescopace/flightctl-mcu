# Results

What works, where it was verified, and the measurements. The devices are microcontrollers with no Linux, managed by an unmodified **Flight Control 1.4.0** (the upstream of Red Hat Edge Manager). The server ran on [kind](https://kind.sigs.k8s.io/) (Kubernetes in Docker) on a Mac. The figures come from development builds before 1.0; where a later change moved one, the text says so.

## What works

Where each function was verified:

- **Boards**: real boards, the classic ESP32, ESP32-C3, C5, C6 and S3 with ESP-IDF, and the ESP32-S3 with Zephyr.
- **Emulator**: the real firmware in Espressif's emulator, against the same Flight Control ([below](#in-espressifs-emulator)).
- **CI**: the end-to-end tests, with the simulated device and the Zephyr builds for `native_sim` and an emulated Cortex-M3 ([TESTING.md](TESTING.md)).

| Function | What it does | Verified | More |
|---|---|---|---|
| Enrollment | A P-256 key generated on the device, a CSR approved with `flightctl approve`, mTLS with TLS 1.3 | Boards, emulator, CI | [QUICKSTART.md](QUICKSTART.md#5-approve-it) |
| Automatic approval | Approves devices from an allow list of names or MACs, with their labels | CI | [QUICKSTART.md](QUICKSTART.md#5-approve-it) |
| Status | The agent API's full status: resources, `customInfo` (heap, RSSI, temperature, traffic); `Unknown` after 5 minutes offline | Boards, emulator, CI | [Label sync](#label-sync) |
| HTTP/2 | The spec's long-poll and the status share one HTTP/2 connection; firmware and apps are pulled from the registry over HTTP/2 | Boards, emulator, CI. Downloads over HTTP/2: the classic ESP32, C5, C6 and S3 with ESP-IDF. Not yet on a board: a certificate renewal over HTTP/2, Zephyr's downloads over HTTP/2 | [REGISTRY.md](REGISTRY.md#memory), [idle traffic](#idle-traffic) |
| Resource alerts | CPU, heap and storage checked against the fleet's `spec.resources` rules, as the Linux agent does: `Warning` or `Critical` per resource, `Degraded` or `Error` in the summary | ESP32-C3, CI | [Timings](#timings) |
| Config | Inline `spec.config` applied without a reboot, after the app validates it (an invalid one is refused, the device OutOfDate with the error); a file the spec drops is removed | Boards, CI | [QUICKSTART.md](QUICKSTART.md#6-give-it-firmware-config-and-an-app), [timings](#timings) |
| Firmware OTA | `spec.os.image` points to an OCI index with one firmware per chip, written to the other A/B slot and checked with sha256; staged rollouts | Boards, emulator | [QUICKSTART.md](QUICKSTART.md#6-give-it-firmware-config-and-an-app), [timings](#timings) |
| Firmware deltas | The device downloads a patch from the release it runs and rebuilds the image: 1 to 15% of the image | ESP32, C5, C6, S3, the S3 with Zephyr, emulator | [DELTA.md](DELTA.md) |
| Automatic rollback | A new firmware is kept only once the server accepts a status from it; otherwise the device goes back, and the rollout stops at the canary | ESP32-S3 with both ports, C6, emulator | [The way back](#the-way-back-on-the-esp32-s3-both-ports) |
| Mixed fleets | One fleet and one firmware reference for every chip, with per-device settings from labels | ESP-IDF boards | [DEVICES.md](DEVICES.md) |
| WASM apps | `spec.applications` with a WASM OCI image, run by WAMR, updated without a reboot; one `.wasm` for Xtensa, RISC-V and Arm | Boards, emulator, CI | [APPS.md](APPS.md) |
| App interface | Hardware by name (GPIO, I2C, SPI, UART, ADC, PWM, LED), grants, timers, events, config, storage, a small C library | Unit tests, Zephyr's emulated drivers; LED and temperature on the boards. With nothing wired, I2C, SPI, UART, ADC and PWM open and answer on the classic ESP32, C5, C6 and S3 and the emulated C3 (`hwcheck`). Not yet on a board: GPIO interrupts, wired devices | [APPS.md](APPS.md#ports) |
| An app's limits | An instruction budget and 1 s in the device's functions per call; 32 KB of heap kept for the agent; a `.wasm` sized to the heap | ESP-IDF boards, emulated C3, unit tests. Not checked on Zephyr yet | [APPS.md](APPS.md#size-and-limits) |
| Modules built for no device | A module published for any WebAssembly runtime runs unmodified, once | CoreMark on the ESP-IDF boards and the S3 with Zephyr | [APPS.md](APPS.md#modules-built-for-no-device), [CoreMark](#coremark) |
| Apps that crash the device | An app whose start crashed the firmware twice is not started again; its status says `Error` | Unit tests, emulated C6 | [APPS.md](APPS.md#how-an-app-runs) |
| Crash reports | After a crash, `lastFatalError` in the status: the fault, the task, the `pc` and the callers, decoded against the release's ELF | Emulator (C3, C5, C6, S3), CI. Zephyr: the fault's report in the log on the S3; `lastFatalError` not yet checked on a board | [A crash in the status](#a-crash-in-the-status) |
| Certificate renewal | The device renews its management certificate for the same key | Emulator, CI, with 2-minute certificates. Not on a board, where a certificate lasts a year | [TESTING.md](TESTING.md) |
| Signed images | Firmware and apps signed in cosign's format, checked on the device; an unsigned image or one signed with another key is refused, the running one kept, the device OutOfDate with the reason | Boards, emulator, CI | [SECURITY.md](SECURITY.md#two-signing-keys) |
| Registry with TLS and a password | The registry's CA and `auth.json` come from the fleet | ESP32-C3, the S3 with Zephyr, emulator, CI | [REGISTRY.md](REGISTRY.md), [on boards](#the-registry-with-tls-and-decommissioning-on-boards) |
| Registry tokens (Bearer) | A token from the registry's realm, with the `auth.json` credentials or none, renewed when it runs out | The device's client on a host, against Quay 3.17 and docker_auth. Not on a board, not with Harbor | [TESTING.md](TESTING.md#registry-tokens) |
| Decommissioning | Key and certificate wiped; no new enrollment until the flash is erased | ESP32-C3, the S3 with Zephyr, emulator, CI | [QUICKSTART.md](QUICKSTART.md#7-take-it-out) |
| Remote console | `flightctl console` and the UI's terminal, with a closed set of commands | Boards, CI | [CONSOLE.md](CONSOLE.md) |
| Device log | The agent's, the apps' and the system's lines, in RAM kept across a restart, shown by the console and the UI's Logs tab | Boards, CI | [CONSOLE.md](CONSOLE.md#the-log) |
| Metrics | The device's and the apps' metrics to Flight Control's Telemetry Gateway, in OTLP | Boards, CI | [METRICS.md](METRICS.md) |
| MQTT for the apps | One broker connection for all the apps, MQTT 3.1.1, QoS 0 and 1, over TLS | ESP-IDF boards, against mosquitto. Zephyr: built, not run on a board | [APPS.md](APPS.md#mqtt) |
| Known vulnerabilities | An SBOM per release; Flight Control shows the CVEs found by Trustify | Boards, with Trustify 0.4.12 | [SBOM.md](SBOM.md) |
| Provisioning in the field | The WiFi network set over the device's own access point | Emulated C5, S3 and C6 in CI; the S3 with Zephyr from an iPhone | [PROVISIONING.md](PROVISIONING.md) |
| Secure Boot, flash encryption, anti-rollback | The `secure` variant | Emulator only: no real chip has been burned. Zephyr: anti-rollback by MCUboot, on the Pico W | [SECURITY.md](SECURITY.md) |

Flashing a firmware does not touch the key storage: a board keeps its identity unless its whole flash is erased, and then enrolls again as a new device.

The ESP32-S2 and the Raspberry Pi Pico W were dropped for their RAM ([support levels](DEVICES.md#support-levels)). Before that, the S2 ran enrollment, apps, the registry with TLS, decommissioning and CoreMark in a mixed fleet. The Pico W ran enrollment, apps, deltas, rollback, MCUboot's anti-rollback, the registry with TLS, decommissioning, and kept the record of a crash across the restart.

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
| The MQTT connection, on the C3 | 9 KB of heap (49 KB free before, 40 KB after); the client's thread used 3.2 to 4.2 KB of its 8 KB stack |
| The metrics connection, Zephyr on the S3 | its 17 KB receive buffer, from the C heap |

On the C3 the heap's lowest comes in the first seconds after boot: 3 KB in the first 23 s with MQTT, which connects 4 s after the agent, against 9 KB without. With Zephyr on the ESP32-S3, 4,112 bytes of the 372 KB DRAM segment are left at link time, with a 12 KB status-thread stack and a 92 KB kernel heap.

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
| Lowest heap seen | 2.3 KB on the classic ESP32, the agent still reporting; 108 bytes on the C3 after a failed swap, where one TLS handshake failed (`PSA_ERROR_INSUFFICIENT_MEMORY`) |

### Timings

ESP-IDF boards with an early build, unless the row says otherwise:

| Operation | Time |
|---|---|
| From boot to enrollment request (WiFi, SNTP, TLS) | about 10 s |
| Config change, from apply to device confirmation | 1–6 s |
| Firmware rollout to 3 devices (canary, then the rest) | 50–80 s, of which about 20 s per device for download and reboot |
| Rollback of a broken firmware | about 3.5 min (150 s confirmation window plus 2 reboots) |
| WASM app update across the fleet (canary, then the rest) | about 50 s, almost all of it waiting for the rollout; on the device the swap is immediate |
| App state change, to Flight Control | about 2 s |
| Memory rule added to the fleet, to `Warning` and Flight Control's `DeviceMemoryWarning` (end-to-end test) | 21 s; `DeviceMemoryNormal` once the rule is removed |
| An app call's budget of 5 million instructions, used up by a counting loop (v3.14-rc1) | 0.66 s on the C5, 0.9 s on the S3, 1.06 s on the C3, 1.2 s on the classic ESP32, 1.3 s on the C6 ([APPS.md](APPS.md#size-and-limits)) |
| MQTT connected, from the spec that gives the broker (v3.14-rc6) | within 70 s |
| Decommissioning, from the request to `Completed` | 2.4 to 3 s ([below](#the-registry-with-tls-and-decommissioning-on-boards)) |

### Idle traffic

Measured on the device with the `netTxBytes` and `netRxBytes` counters (bytes on the wire, TLS included), with the agent's earlier HTTP/1.1 client: two connections kept open, a status at once when something changes and otherwise every 2 minutes, the server's long-poll lasting about 4 minutes. Not measured yet since HTTP/2 put both on one connection.

| Device | Total |
|---|---|
| Simulated device, no app (status every minute) | about 220 KB/hour: 87 KB sent, 130 KB received |
| ESP32-C3 with the `alarm` app | about 280 KB/hour |

- The kept connections cost 2 TLS handshakes after boot and none in the next 10 minutes. After a server restart both reopen, and the next spec applies in 2 s.
- A connection per request cost 560 to 730 KB/hour, most of it the TLS 1.3 handshake with mTLS.
- What is left is the full status and the server's answer to it. Going lower needs differential status updates (limitation 6 below).
- The server issued TLS session tickets but did not resume a session with them, so a reconnection costs a full handshake (about 1.5 KB received).
- Changes reported by apps are grouped, one status every 15 s at most, so an app whose state flaps does not flood the network.

### In Espressif's emulator

[esp-emu](https://github.com/espressif/esp-emulator) 0.51 runs the real firmware for the C3, C5, S3 and C6 (not the classic ESP32). Its WiFi access point NATs to the host, so the emulated chip enrolls in the same Flight Control as the boards. With the server issuing 4-minute certificates:

| Chip | Signed OTA (reboot into the other slot) | Unsigned app | Certificate renewal | Heap free / minimum / largest block |
|---|---|---|---|---|
| ESP32-C3 | confirmed, 19 s | refused | 6 renewals, still reporting after the first certificate expired | 57.8 / 37.6 KB / – |
| ESP32-C5 | confirmed, 15 s | refused | renewed, still reporting after the first certificate expired | 61.7 / 42.2 / 40.9 KB |
| ESP32-S3 | confirmed, 35 s | – | renewed | 121.3 / 102.1 / 73.7 KB |

The C3 figures match the board's. `tests/e2e/e2e-emu.sh` runs the whole sequence on the images CI builds for the boards, and CI runs it for the C5, the S3 and the C6 ([TESTING.md](TESTING.md)). It updates the default build over the air to the release build, then tries a broken build that cannot reach the server, which is rolled back once its confirmation window ends.

Before v3.14 the emulated C6 could stall in about one run in two: with an app running, the release build had 2.9 KB of internal heap left while it wrote the HTTP/2 preface, and the agent stayed silent. Since v3.14 the block the TLS handshake takes is given back before the preface, and 6 runs of 6 passed. The real C6 never showed it.

### ESP-IDF 6.0.3 on a board (ESP32-S3)

An ESP32-S3-DevKitC with the debug build (mbedTLS 4.1.1, cryptography through PSA), against Flight Control 1.4.0:

| Step | Result |
|---|---|
| Enrollment | key generated on the chip, TLS 1.3 with the enrollment certificate, management certificate for a year |
| Config and WASM app | config file written, the signed `alarm` app installed and running |
| Firmware, whole image | 1181 KB downloaded in 5 s into the other slot, restarted, confirmed |
| Firmware, as a delta | the release build (1000 KB) from a delta of 511 KB, rebuilt from the running image in 13 s, restarted, confirmed |
| Memory (release build, app running) | 145 KB of heap free, 124 KB at the lowest |

The delta is half of the image because it goes from the debug build to the release one, compiled differently; between two builds of the same kind it is 1 to 12% ([DELTA.md](DELTA.md)). The secure variant was not run on a board (it burns eFuses).

### The way back on the ESP32-S3, both ports

An ESP32-S3 with each port in turn, against Flight Control 1.4.0. Each was given, through its fleet, a firmware built to reach Flight Control on a wrong port (9443) and to give itself 30 seconds instead of 150:

| | ESP-IDF 6.0.3 | Zephyr 4.5 with MCUboot |
|---|---|---|
| Before | the release build, received as a delta of 512 KB (`lastDelta: Applied`) | a build received as a delta of 28 KB (`lastDelta: Applied`) |
| Broken image | 1181 KB, whole, downloaded and restarted in 11 s | 885 KB, whole, in 14 s |
| Given up | 30 s after the agent started, by the image itself | the same |
| Back | the bootloader takes the previous slot | MCUboot swaps the slots back: 15 s |
| Reported | 53 s after the fleet changed: "failed to start and was rolled back; not retried" | 1 min 53 s, the same message |

Neither tried the image again. The ESP32-C6 (ESP-IDF) was back on the previous firmware, and reporting it, 50 s after it restarted into a broken one; with v3.13, in three more runs with two different builds, it was online again 3 to 5 s after the restart back.

### A crash in the status

What `lastFatalError` gives after each kind of crash, chip by chip, is in [TESTING.md](TESTING.md#a-crash-in-the-status). The handler takes 799 bytes of IRAM on the ESP32, 907 on the S3 and 1,102 on the C3, C5 and C6; the record takes 140 bytes of RAM kept across restarts.

### The registry with TLS, and decommissioning, on boards

Against Flight Control 1.4.0, with the registry behind TLS and a password ([REGISTRY.md](REGISTRY.md)). The CA and `auth.json` came from the fleet; decommissioning was sent with `flightctl decommission`.

| | ESP32-C3, ESP-IDF 6.0.3 | Pico W, Zephyr | ESP32-S3, Zephyr |
|---|---|---|---|
| App over TLS | running 3 s after the config, no restart | about 6 s | about 6 s |
| Firmware over TLS, whole | 1234 KB in 5 s | 729 KB in 24 s | 886 KB |
| Firmware over TLS, as a delta | not tried | not tried | 17 KB (`Applied`) |
| Decommissioning, from request to Completed | 3 s | 2.5 s | 2.4 s |

After decommissioning each board wiped its key and management certificate, restarted, showed as `Decommissioned`, and kept running its app without asking to enroll again. Erased, each enrolled again as a new device. Not measured: the heap used during a download over TLS (the reboot that follows a firmware download clears the low-water mark).

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

- **Security**:
  - The registry is reached over TLS, with a password when it asks ([REGISTRY.md](REGISTRY.md)), and only with a CA the fleet gives the device: there is no public CA bundle, and blobs redirected to another host are not followed.
  - The device key is in flash in clear, except in the `secure` variant (see [SECURITY.md](SECURITY.md)), tested in the emulator only, where it is encrypted. There the CPU can still read it, except on the ESP32-C5, where it is in the ECDSA peripheral. On the other chips the Digital Signature peripheral could hold it, but Flight Control cannot enroll the RSA key that needs.
- **Provisioning**: credentials are written over USB (`tools/provision.py`), the WiFi network over USB or in the field, over the device's own access point (see [PROVISIONING.md](PROVISIONING.md)). An enrollment certificate can be issued per batch, but Flight Control 1.4.0 cannot revoke it (its main branch can).
- **Offline longer than the certificate**: a device whose management certificate expired while it was offline (a year by default) enrolls again with its enrollment certificate as a new device, keeping its apps, config and firmware; the old record is left for the operator to delete (limitation 13).
- **Server unreachable**: a device that has had no status accepted for 10 minutes restarts, and so again every 10 minutes while the server stays out of reach; its apps restart with it. A write on a connection gives up after 30 s.
- **App formats**: apps must be WASM OCI artifacts (`application/vnd.wasm.content.layer.v1+wasm`). Podman-style images with a `.wasm` inside a tarball are not supported.
- **Firmware and apps that need it, in one spec**: the device applies a spec's apps before its firmware, and an app it cannot load holds the firmware back. So a spec that brings a new firmware together with an app that needs it (a function only the new firmware has, such as the C library's `snprintf` before v3.14) is refused by the old firmware (`not a loadable WASM module: imports env.snprintf, which this firmware does not have`), and a canary rollout is suspended. For now such a release goes out in two steps: the firmware first, then the app once the fleet is up to date. The fix proposed: when the spec also changes the firmware, an app that the running firmware cannot load because of a missing import is left for after the restart, the firmware is installed, and the new one loads the app.
- **JSON depth**: the device refuses JSON nested more than 32 levels (spec, manifests, config files), and reports the error; real documents use fewer than 10.
- **Hardware coverage**: the latest builds were tried on the classic ESP32, the C3, the C5, the C6 and the S3 with ESP-IDF, and on the S3 with Zephyr. Not yet tried on a board: the `secure` variant, certificate renewal, registry tokens, GPIO interrupts and wired peripherals.
- **Zephyr**: an app's heap floor and the `.wasm` size limit are not checked yet. MQTT is built but not yet run on a board, and `lastFatalError` not yet checked on one.
- **Memory for apps**: CoreMark's 64 KB fits on the classic ESP32, the C3, the C5, the C6 and the S3, and with Zephyr on the ESP32-S3 N8R8, in PSRAM; wasm-coremark's 128 KB on the C6 and the S3 with PSRAM. On the classic ESP32, the C3 and the C5 an install or a swap can still fail for want of one free block of 68 KB, most of all right after another app ([CoreMark](#coremark)).

What comes next is in the [roadmap](ROADMAP.md).
