# Results

What has been verified, where, and with which figures: microcontrollers managed by **Flight Control 1.4.0** (upstream of Red Hat Edge Manager), with no Linux on the device and **no changes to the server**. The server ran on [kind](https://kind.sigs.k8s.io/) (Kubernetes in Docker: a Kubernetes cluster whose nodes are containers on one machine), on a Mac. The figures come from development builds before 1.0; where a later change moved one, the text says so.

## What works

The ESP32-S2 and the Raspberry Pi Pico W were removed after these tests, for their RAM ([support levels](DEVICES.md#support-levels)); what was verified on them is kept in this table.

| Capability | How | Verified |
|---|---|---|
| Enrollment and identity | P-256 key generated on the device, CSR, approval with `flightctl approve`, mTLS with TLS 1.3 | Boards: the ESP32, S3, C3, C5 and C6 with ESP-IDF, the ESP32-S3 with Zephyr (and the S2 and the Pico W). Emulated: the C3, C5, S3 and C6. The simulated device |
| Monitoring | Full agent API status: resources, `customInfo` (heap, RSSI, temperature, traffic), and `Unknown` after 5 minutes offline | Works |
| Resource alerts | CPU, heap and storage sampled against the fleet's `spec.resources` rules, as the Linux agent does; the status gives `Warning` or `Critical` per resource and the summary turns `Degraded` or `Error` | End-to-end test: a memory rule from the fleet reported as `Warning` within 21 s, Flight Control logging `DeviceMemoryWarning`, then `DeviceMemoryNormal` once the rule was removed. ESP32-C3 with the `alarm` app: 1% CPU, the three resources `Healthy` |
| Config without reboot | Inline `spec.config`, validated by the application before it is stored | Applied in 1–6 s. An invalid config is rejected and reported as OutOfDate with the error message |
| Firmware OTA | `spec.os.image` points to an OCI index with one firmware per chip. The download goes straight into the A/B slot and is checked with sha256 | Staged rollout (canary first, then the rest) across 3 devices in 50–80 s |
| Firmware deltas | A device on a known release downloads a patch from it and rebuilds the image from its own ([DELTA.md](DELTA.md)) | 1 to 15% of the image between builds of the same kind. On the classic ESP32, C5, C6 and S3 (ESP-IDF, the core's decoder), on the ESP32-S3 with Zephyr and the Pico W, and on every chip in the emulator |
| Automatic rollback | The new firmware is confirmed only after the server accepts a status from it; otherwise the device goes back to the previous one | A broken firmware rolls itself back; the rollout stops at the canary and the other devices never receive it. On the ESP32-S3 with both ports ([below](#the-way-back-on-the-esp32-s3-both-ports)), the C6 (back on the previous firmware and reporting it 50 s after it restarted into the broken one), the Pico W and the emulated chips |
| WASM apps without reboot | `spec.applications` of type `container` with a WASM OCI image, run by WAMR | Update from v1 to v2 on Xtensa, RISC-V and Arm with the same `.wasm` file, no reboot |
| Modules built for no device | A module published for any WebAssembly runtime runs unmodified: the export named in `FC_ENTRY` (or `_start`) is called once with no instruction budget, `env.clock_ms` gives it the time, tail calls are built in; the result is the app's state, the app `Completed` ([APPS.md](APPS.md#modules-built-for-no-device)) | CoreMark's published builds on the classic ESP32, the S2, C3, C5, C6 and S3 with ESP-IDF, and on the Zephyr ESP32-S3 ([CoreMark](#coremark)); removing the app stops a run at once (unit tests) |
| App interface | Hardware by name from a board map, grants (`FC_ALLOW`), timers and events, a config file per app, key/value storage, a safe state when an app stops ([APPS.md](APPS.md)) | Unit tests against a fake board, and the end-to-end tests. On the C3 and on the Zephyr ESP32-S3 (temperature and LED simulated) the `alarm` app opened the board's LED as `led0`, its pin from the firmware, and a new threshold took it from alarm to ok without restarting it. It runs on the C6 (`led0` its WS2812 LED, the app reloaded from flash after a restart) the classic ESP32, the S2 and the Pico W. GPIO interrupts not yet tried on a board |
| Apps that crash the device as they start | An app whose start crashed the firmware twice in a row (while its module loaded, or in its first minute) is not started again: `Error` in the status, the spec not retried, the device on to the server; tried again when its image, env vars or the firmware change ([APPS.md](APPS.md#how-an-app-runs)) | Unit tests: a saved app crashing at every boot, a module crashing as it loads beside an app that runs (only it is stopped), single crashes and other restarts, a new firmware. In the emulator, an ESP32-C6 image whose `alarm` crashed it at every boot: two crashes, then the app not started and the firmware going on. On Zephyr the crash is told by the fatal error kept in RAM across the restart, seen on the Pico W |
| Heterogeneous fleet | One fleet and one firmware reference, with per-device settings through labels and the template | Classic ESP32, S3, C3, C5 and C6 (and the S2) in the same fleet |
| Certificate renewal | The device renews its management certificate for the same key, through the renewal signer the server approves on its own | With 2-minute certificates, in the emulator and on the simulated device: the device keeps reporting after the first one expires. Not on a board, where a certificate lasts a year |
| Signed images | Firmware and apps signed in cosign's format; the device checks the signature against a key in its credentials | Unsigned images and images signed with another key are refused, the running app kept, the device OutOfDate with the reason; `cosign verify` accepts the signatures |
| Registry with TLS and a password | The registry's CA and an `auth.json` from the fleet ([REGISTRY.md](REGISTRY.md)) | On the C3 and the S2 (ESP-IDF), the ESP32-S3 and the Pico W (Zephyr) ([below](#the-registry-with-tls-and-decommissioning-on-boards)), and in every end-to-end test |
| Decommissioning | Key and certificate wiped, no enrollment until the storage is erased | On the same boards, 2.4 to 3 s from the request to `Completed`, and in the end-to-end tests |
| Automatic approval | An allow list of device names or MACs with their labels | Used by the end-to-end tests |
| Remote console | `flightctl console device/<name>` and the UI's terminal: a gRPC stream on the agent's connection, Kubernetes' remote command protocol, and a closed set of commands (`help`, `status`, `apps`, `reboot`, `exit`), one per session or at a prompt ([CONSOLE.md](CONSOLE.md)) | On the classic ESP32, C3, C5, C6 and S3 (ESP-IDF) and the ESP32-S3 with Zephyr against Flight Control 1.4.0: one command per session (exit status 127 for an unknown one) and a prompt with `--tty` (the prompt on the ESP-IDF boards). In the end-to-end test on the simulated device, Zephyr's native_sim and the emulated Cortex-M3 |
| Metrics | The device's metrics and its apps' (`fc_metric`) to the Telemetry Gateway in OTLP over gRPC, on a TLS connection of its own beside the agent's, with the `device-svc-client` certificate the device asks for ([METRICS.md](METRICS.md)) | Against Flight Control 1.4.0 with the simulated device: the certificate approved by the server at once, the metrics in the gateway's Prometheus exporter with the device's `device_id` (`device_memory_free_bytes`, `device_network_io_bytes_total`...). A gateway that drops the packets is given up after 15 s, then tried later and later; a corrected address is tried at once. The request byte for byte as OpenTelemetry's own protobuf classes write it, and an app's `fc_metric` (unit tests). On the classic ESP32, C5, C6 and S3 (ESP-IDF, v3.13-rc6, every 30 s): the certificate asked for and approved, the device's metrics at the gateway, and the `trend` app's (`app_<name>_temp_c`) from the C6. The internal heap at its lowest during an export: 79 to 100 KB free on the ESP32, 93 to 108 KB on the S3, 26 to 49 KB on the C5, 25 to 44 KB on the C6 (its WiFi's buffers are ESP-IDF's defaults); 19 to 20 KB on the C3 (13 KB right after it took a whole firmware); none skipped for want of memory, and none lowered a device's heap below what its boot had reached. On the ESP32-S3 with Zephyr (z26), whose second connection takes its 17 KB receive buffer from the C heap: 55 KB of the C heap free, 8.3 KB at the lowest, steady over the exports |
| Device log | The agent's and the apps' lines kept in RAM (4 KB, the oldest dropped first), shown by the console's `log` and by the UI's Logs tab, which runs `journalctl ... \| tail -n` in a `bash -s` session: the device answers it, with the filters for level, unit, boot and time range, and live mode ([CONSOLE.md](CONSOLE.md#the-log)) | Against Flight Control 1.4.0 with the simulated device: the Logs tab's command, as the UI 1.4.0 builds it, sent through `flightctl console -- /bin/bash -s`, answered with the lines and the end marker the tab waits for. On the classic ESP32, C3, C5, C6 and S3 (ESP-IDF, v3.13-rc6) and on the ESP32-S3 with Zephyr (z26), the tab's commands in one session as the tab reuses it: every level and warning and above, a unit (an app's lines alone), the previous boot and a past range (empty), a file (not found), and live mode (the new lines as they come); the session's thread used 5.2 KB of its 8 KB stack at most. In Flight Control 1.4.0's UI, the Logs tab shows a board's log |
| Known vulnerabilities | An SBOM per release, in the registry and in Trustify; Flight Control finds it by the image digest the devices report ([SBOM.md](SBOM.md)) | With Flight Control 1.4.0 and Trustify 0.4.12. Trustify's own feeds reach none of these components; the CVEs NVD has for them (`tools/advisories.py`) showed on the devices within a minute: six for three releases, none of which applies to the code built, so each is listed with the reason and kept out ([SBOM.md](SBOM.md#from-nvd-toolsadvisoriespy)) |
| Provisioning in the field | The WiFi network over SoftAP ([PROVISIONING.md](PROVISIONING.md)) | On the emulated C5, S3 and C6, in CI. Zephyr: the same protocol against esp_prov in CI; on the ESP32-S3 from an iPhone with ESP SoftAP Prov |
| Secure Boot, flash encryption, anti-rollback | The `secure` variant ([SECURITY.md](SECURITY.md)) | On emulated chips only (the C5, S3 and C6 in CI): no real chip has been burned. Zephyr: anti-rollback by MCUboot, on the Pico W ([SECURITY.md](SECURITY.md#zephyr)) |

Flashing a firmware does not touch the key storage: a board keeps its identity unless its whole flash is erased, and then enrolls again as a new device.

## Measurements

### Firmware size

ESP-IDF 6.0.3, v3.12, built for size (`-Os`) with WAMR, which runs the apps, at `-O2`:

| Chip | Image | Free in the 1,664 KB OTA slot | Built for speed (`-O2`), v3.10 |
|---|---|---|---|
| ESP32 | 978 KB | 41% | 1,121 KB |
| ESP32-S3 | 977 KB | 41% | 1,118 KB |
| ESP32-C3 | 1,024 KB | 38% | 1,192 KB |
| ESP32-C5 | 1,107 KB | 33% | 1,282 KB |
| ESP32-C6 | 1,110 KB | 33% | 1,288 KB |

`-Os` saves 141 to 178 KB per image (about 13%); what it costs in speed is under [CoreMark](#coremark). A delta from v3.11 to v3.12 is 49 to 61 KB (4.7–6.2% of the image).

The largest parts, on the C3, built for speed (the agent as of v3.11, the rest measured on an earlier build):

| Component | Flash (code and constants) |
|---|---|
| WiFi, WPA and lwIP | about 430 KB |
| Cryptography and TLS (mbedTLS 4, TF-PSA-Crypto) | about 197 KB |
| WAMR (fast interpreter) | 68 KB |
| **flightctl-mcu agent** (`core/`: enrollment, spec, status with HTTP/2, resource alerts, OTA, OCI, signatures, apps, deltas) | **about 62 KB**; 79 KB with cJSON, WiFi provisioning, the delta decoder and the ESP-IDF port |

Almost all the space goes to the network stack and to cryptography, which any securely connected device needs anyway. The core's own WiFi provisioning takes 17.4 KB on the S3 (debug build) with its access point, where Espressif's `network_provisioning` took 117.5 KB ([PROVISIONING.md](PROVISIONING.md#in-the-field-the-wifi-network)).

### RAM

Internal heap with the `alarm` app running. v3.12 holds one TLS connection at a time; these figures were read after 40 minutes of CoreMark installs and swaps, the lowest during CoreMark's runs:

| Board | Firmware | Free | Lowest | Largest internal block |
|---|---|---|---|---|
| Classic ESP32 | v3.12 | 154.0 KB | 2.3 KB | 76.0 KB |
| ESP32-C5 | v3.12 | 109.2 KB | 5.3 KB | 60.0 KB |
| ESP32-C6 | v3.12 | 205.2 KB | 1.8 KB | 164.0 KB |
| ESP32-S3 N4R2 | v3.12 | 161.2 KB | – (PSRAM counted) | 84.0 KB |
| ESP32-C3 | v3.13-rc6, an hour after boot | 70 to 75 KB | 2.8 KB, at start-up | 36.0 KB |
| ESP32-S3, Zephyr | before the app interface | about 50 KB | – | – |

On the C3 the lowest is reached in the first seconds after boot, before any app or export; v3.13 also holds the device's log (4 KB) and sends metrics every 30 s, with 19 to 20 KB of internal heap left during an export ([METRICS.md](METRICS.md)). The resource alerts take about 4 KB (65.6 KB free on the C3 five minutes after boot), the app interface about 5 KB of static RAM.

Stack used at most, of the stack each has (customInfo `stackMinFreeBytes`; here a KB is 1,024 bytes):

| Firmware | Spec loop, on the main task | Status thread | App thread |
|---|---|---|---|
| Classic ESP32 | 8.9 of 24 KB (installing an app) | 4.2 of 12 KB | 3.4 of 8 KB |
| Zephyr on the Pico W | 9.2 of 14 KB (downloading firmware over TLS, whole or as a delta); 4.6 polling | 3.8 of 8 KB | 2.2 of 8 KB |

The spec loop's figures include the main task's start-up, which runs on the same stack first. Installing an app or downloading firmware takes about twice the stack of polling. A certificate renewal runs on the status thread and has not been measured on a board.

What a WASM app costs, on the C3 (debug build): 17.7 KB of heap for the `trend` app (1.9 KB of `.wasm`). Split on a host with the same module: the module loaded by WAMR's fast interpreter (about 5 KB), the instance and its WAMR stack (about 3.5 KB), the module's linear memory (about 1 KB) and the app's thread stack (8 KB). The `.wasm` itself is freed once the module is loaded.

### CoreMark

[CoreMark](https://github.com/eembc/coremark) 1.0 from its two published WebAssembly builds, unmodified ([APPS.md](APPS.md#modules-built-for-no-device)), run by WAMR's fast interpreter with its instruction count on. ESP-IDF v3.13, with metrics sent every 30 s or none (v3.12 where marked): the firmware built for size (`-Os`), WAMR at `-O2`. Iterations per second; every run valid (CRCs right, a timed part of 10 s or more), with the agent connected. In brackets, the same with the firmware built for speed (`-O2`, v3.10):

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

The interpreter is about 30 times slower than native code; the instruction count, which lets any app be stopped, is part of that. WAMR's AOT mode should come to about 3 times slower than native, but an app would then be published once per architecture (Xtensa, RISC-V) instead of as one module for every device; not measured. At 240 MHz, CoreMark runs 2.2 times faster on the classic ESP32 than with ESP-IDF's debug default at 160 MHz.

Building the firmware for size, with WAMR still at `-O2`, costs CoreMark about 5% on the S3 and 7% on the classic ESP32; on the same release (v3.10) the C5 and C6 lost nothing (22.3 and 12.2 either way). v3.12 also ran lower and less evenly on the C6 (11.45–11.98) and on the C5, which gave about 19.7 or about 22.3 from one run to the next of the same image, once 14.8: not explained yet. With v3.13 on the C6, four runs of wasm3's build (11.81 to 11.98) and two of wasm-coremark's (13.24 and 13.71); on the C5 one run, 19.46.

The linear memory needs one free block of its size, rounded up by the heap: 64 KB takes a block of 68 KB, 128 KB one of 136 KB. Over 3 installs (from the `alarm` app) and 4 swaps (CoreMark replaced by itself, without a restart) per board, one try failed on the classic ESP32 (a swap, largest free block 48 KB) and one on the C5 (an install, 64 KB); the C6 and the S3 passed every one, and again with v3.13 (installs and swaps of each build). With v3.13 the 64 KB block was there once in two tries on the classic ESP32 (else 64 KB the largest of 138 KB free) and on the C5 (else 44 KB), and once in five on the C3, right after a restart (else 48 to 50 KB the largest of 50 to 87 KB free, 22 KB after a swap). Without metrics (`metrics.json` with no endpoint, from a restart) the ESP32 and the C3 failed alike, so the exports are not what breaks the heap up. On the C3, with CoreMark's memory taken, one of the agent's TLS handshakes failed for want of memory (`PSA_ERROR_INSUFFICIENT_MEMORY`), and after a swap that failed the heap's lowest since the restart was 108 bytes. On the classic ESP32 the 64 KB build runs from internal RAM, and over the runs the heap went down to 2.3 KB at the lowest while the agent kept reporting. The S3 puts an app's memory in PSRAM when no internal block is large enough, with two images: `esp32s3` for quad PSRAM (and boards with none) and `esp32s3-r8` for octal ([DEVICES.md](DEVICES.md)). With Zephyr the N8R8's octal PSRAM is a heap of its own, from which WASM apps' memories of 16 KB or more come (`port/zephyr/fc_heap_zephyr.c`); its driver takes 4 KB of internal RAM.

### Timings

ESP-IDF boards, an early build:

| Operation | Time |
|---|---|
| From boot to enrollment request (WiFi, SNTP, TLS) | about 10 s |
| Config change, from apply to device confirmation | 1–6 s |
| Firmware rollout to 3 devices (canary, then the rest) | 50–80 s, of which about 20 s per device for download and reboot |
| Rollback of a broken firmware | about 3.5 min (150 s confirmation window plus 2 reboots) |
| WASM app update across the fleet (canary, then the rest) | about 50 s, almost all of it waiting for the rollout; on the device the swap is immediate |
| App state change, to Flight Control | about 2 s |

### Idle traffic

Measured on the device with the `netTxBytes` and `netRxBytes` counters (bytes on the wire, TLS included), with the agent's earlier HTTP/1.1 client: two connections kept open, a status at once when something changes and otherwise every 2 minutes, the server's long-poll lasting about 4 minutes. Not measured since HTTP/2 put both on one connection.

| Device | Total |
|---|---|
| Simulated device, no app (status every minute) | about 220 KB/hour: 87 KB sent, 130 KB received |
| ESP32-C3 with the `alarm` app | about 280 KB/hour |

The kept connections cost 2 TLS handshakes after boot and none in the next 10 minutes; after a server restart both reopen and the next spec applies in 2 s. A connection per request cost 560 to 730 KB/hour, most of it the TLS 1.3 handshake with mTLS. What is left is the full status and the server's answer to it; going lower needs differential status updates (limitation 6 below). The server issued TLS session tickets but did not resume a session with them, so a reconnection costs a full handshake (about 1.5 KB received). Changes reported by apps are grouped, one status every 15 s at most, so an app whose state flaps does not flood the network.

### In Espressif's emulator

[esp-emu](https://github.com/espressif/esp-emulator) 0.51 runs the real firmware for the C3, C5, S3 and C6 (not the classic ESP32). Its WiFi access point NATs to the host, so the emulated chip enrolls in the same Flight Control as the boards. With the server issuing 4-minute certificates:

| Chip | Signed OTA (reboot into the other slot) | Unsigned app | Certificate renewal | Heap free / minimum / largest block |
|---|---|---|---|---|
| ESP32-C3 | confirmed, 19 s | refused | 6 renewals, still reporting after the first certificate expired | 57.8 / 37.6 KB / – |
| ESP32-C5 | confirmed, 15 s | refused | renewed, still reporting after the first certificate expired | 61.7 / 42.2 / 40.9 KB |
| ESP32-S3 | confirmed, 35 s | – | renewed | 121.3 / 102.1 / 73.7 KB |

The C3 figures match the board's. `tests/e2e/e2e-emu.sh` runs the whole sequence on the images CI builds for the boards; CI runs it for the C5, the S3 and the C6 ([TESTING.md](TESTING.md)). It updates the default build over the air to the release build, and tries a broken build that cannot reach the server, which is rolled back once its confirmation window ends.

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

Neither tried the image again.

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

Flight Control 1.4 installs `LabelSyncMapping` resources, and one of them copies every entry of `customInfo` into the device's labels (`custominfo.flightctl.io/<key>`). These devices report counters there: free heap, traffic, uptime. With the default, each status update rewrote the device's labels and logged two events ("label reconciliation was requested", "updated successfully (labels)"): 227 of 400 events on the test cluster, while the tests ran.

`tools/fleet/labelsync-custom-info.yaml` replaces that mapping with one that copies only what stays the same between firmware updates (device class, board, firmware and OS version, security state); [QUICKSTART.md](QUICKSTART.md) applies it. The labels then stop changing, and so does the second event. The first is logged whenever anything in the status changes (the service compares the whole status, `lastSeen` aside), before it knows nothing will come of it (limitation 10 below). The agent therefore keeps its status still: the `Updating` condition's time changes only with the condition, as in Kubernetes, and the counters are read at the start, five minutes later, then once an hour. The simulated device, with no app, over 9 minutes (a status a minute):

| | Events |
|---|---|
| Counters and the condition's time new at every status | 10, one per status |
| The condition's time kept, counters once an hour | 2: at the start, and when the counters were read again five minutes later; then one an hour |

What an app reports is not held back: an app whose state changes about twice a minute makes an event at each change.

The labels a mapping makes are its own: editing a device without them, to change another label, is refused (409, "cannot edit a managed label owned by a mapping"). An edit must carry them as they are.

The new labels also let a fleet select on what a device says it is, `custominfo.flightctl.io/deviceClass=zephyr` for instance, which is part of what limitation 4 asks for.

## Flight Control limitations found (upstream proposals)

1. **`matchExpressions` in the fleet selector is accepted but ignored.**
   - The fleet stays empty with no error. The controller only uses `matchLabels` (`internal/tasks/fleet_selector.go`).
   - Proposal: support it, or reject it at validation.
2. **Batch success is based only on `renderedVersion`.**
   - An agent that reports the version before confirming the new OS makes the rollout advance. The Linux agent avoids this implicitly; this agent keeps reporting the previous version until the new firmware is confirmed.
   - Proposal: write this requirement into the agent API contract, or base success also on `os.image` and the `Updating` condition.
3. **Fixed 5-minute disconnect timeout** (`DeviceDisconnectedTimeout`).
   - Battery devices in deep sleep would be `Unknown` most of the time.
   - Proposal: make it configurable per fleet or per device, or let the device declare its expected contact interval.
4. **No way to declare the device class or the supported runtimes.**
   - For an MCU the server also accepts `compose`, `quadlet` or `helm` apps, and Podman fields (ports, volumes, `runAs`) that mean nothing there. The error only surfaces on the device.
   - Proposal: let the device report something like `applicationRuntimes: [wasm]` (in `status.systemInfo`, where 1.4 moved `osMode` from `status.capabilities`), and validate rendering against it.
   - Possibly also add an explicit `appType: wasm`, or recognize images with platform `wasip1/wasm` as WASM.
5. **No free-form status field per app.**
   - `DeviceApplicationStatus` has no field such as `message` or `info`.
   - The app state ended up in `customInfo` (`app.<name>`).
6. **Cost of each status update.**
   - Every PUT sends the full status, 2–3 KB of JSON. The agent API also takes a JSON Patch of the status (`patchDeviceStatus`), which the Linux agent uses for urgent changes, but nothing documents it as the way for a constrained device, and there is no heartbeat-only variant.
   - Proposal: document PATCH as the recommended path for constrained devices, or add a heartbeat-only variant.
7. **The server compares `os.image` only as a string.**
   - Re-pushing the same tag is not seen as an update.
   - Proposal: recommend digest references (`@sha256:`) for firmware and apps, or compare `imageDigest`.
8. **An enrollment certificate cannot be revoked.**
   - The enrollment endpoint checks only the certificate's signer and expiry (`internal/api_server/middleware/enrollment_auth.go`). One leaked from a batch of devices can enroll new devices until it expires; deleting its CertificateSigningRequest changes nothing.
   - Workaround: one certificate per batch with a short lifetime (`BATCH=… EXPIRATION=… tools/enrollment.sh`), and approval from an allow list.
   - Proposal: a revocation list for enrollment certificates, or refuse those whose CertificateSigningRequest was deleted or denied.
   - Fixed on Flight Control's main branch, not in 1.4.0: enrollment is refused when the certificate's CertificateSigningRequest is revoked or not approved (`CSRStatusAllowsEnrollment`, with an API to revoke it). It waits for a release.
9. **A device with an RSA key cannot enroll.**
   - The internal CA builds the certificate with the request's signature algorithm (`internal/crypto/internal_ca.go`: `SignatureAlgorithm: csr.SignatureAlgorithm`). An RSA request asks for SHA256WithRSA, the CA's key is ECDSA, and Go's `x509.CreateCertificate` refuses: "requested SignatureAlgorithm does not match private key type". Approving the request fails with a 400.
   - It matters for keys in hardware: the ESP32 DS peripheral signs only with RSA.
   - Proposal: leave `SignatureAlgorithm` unset in the template, so the CA signs with its own algorithm whatever the device key.
10. **Since 1.4, a change in the status is a label event.**
   - The default `custom-info` mapping copies all of `customInfo` into labels, and the service logs "label reconciliation was requested" at every change in the status (`internal/service/common/device.go`: `deviceStatusesEqual`), even when no label changes ([above](#label-sync)).
   - `customInfo` is the only free-form place in the status, so a constrained device's counters end up there (limitation 5).
   - Proposal: ask for a reconciliation only when a mapping's inputs changed, and do not copy all of `customInfo` by default; or give counters a place of their own.
11. **A fleet's OS image cannot come from the software catalog.**
   - The fleet accepts `spec.os.catalogItemRef` and keeps it in its template version, but the device gets `os: {}`: building the device's spec from the template copies `os.image` only (`internal/tasks/desired_spec.go`: `assembleDesiredSpec`). The device is reported `UpToDate` and its firmware never changes, with no error.
   - Tried with a catalog for the Zephyr firmware and the `alarm` app. The app's `catalogItemRef` works: the server turned the catalog's version into the app's image reference, and the agent got the image reference as before.
   - Proposal: copy `os.catalogItemRef` too, as is done for the applications.
12. **An inline file given in base64 reaches the device percent-encoded.**
   - The server keeps inline files as data URLs (`pkg/ignition/ignition_wrapper.go`: `SetFile`). For content already in base64 it sets the URL's encoding to ASCII, which percent-encodes the text. Building the rendered spec (`internal/tasks/device_render.go`: `ignitionConfigToRenderedConfig`) then cuts off the `data:...,` prefix and serves what is left as base64, escapes included. `aGk=` arrives as `aGk%3D`.
   - The Linux agent decodes with Go's `base64.StdEncoding` (`api/core/v1beta1/types.go`: `decodeContents`), which refuses `%`. Base64 content whose length is not a multiple of three, which is most of it, should then fail to be written (read in the source, not tried with the Linux agent).
   - Found with an `auth.json` for the registry ([REGISTRY.md](REGISTRY.md)). Content given as text is turned into proper base64 by the same code and arrives intact. This agent undoes the escapes before decoding, and the tools give text.
   - Proposal: unescape the data URL's text (or use the URL's own decoding) before serving it as base64.
13. **A device cannot enroll again under its own name.**
   - An enrollment request for a name that already has a device is refused (`internal/service/enrollmentrequest/handler.go`: `ErrDuplicateName`). A device whose management certificate expired while it was offline can only enroll again under a new key, hence a new name; the old record stays until an operator deletes it.
   - Proposal: let an enrollment certificate re-enroll a known device, with the same approval as a new one.
14. **A withdrawn advisory stays on the devices.**
   - The vulnerability sync only inserts and updates findings (`internal/store/vulnerabilityfinding/store.go`: `UpsertFindings`); nothing removes one that Trustify no longer returns.
   - Found with a test advisory deleted from Trustify ([SBOM.md](SBOM.md#what-was-verified-with-a-test-advisory)): two syncs later Flight Control still counted it.
   - Proposal: replace a digest's findings with what the scan returns for it.
15. **A console session whose user left before the device came stays listed.**
   - A session is listed in the device's annotation (`device-controller/console`) when it starts, and taken out only when the user's websocket closes after the device joined (`internal/console/console.go`: `CloseSession`). If the user leaves first (`flightctl console` gives up after 60 s, the UI's Logs tab after 50 s), nothing takes it out. While any session is listed the rendered spec is served at once, with no long-poll (`internal/store/model/device.go`), so the device asks for its spec every few seconds, for good.
   - Found on the lab boards: sessions asked for while a board was restarting stayed listed. This agent remembers the last 16 sessions it saw (it never opens one twice), ends a session nobody joins within a minute, and polls every 15 s instead of 5 once only sessions it has seen are listed.
   - Proposal: take a session out when its user leaves, joined or not, and give listed sessions a lifetime.

All of these were read again in the source of 1.4.0 and of Flight Control's main branch of 12 November 2026: they hold in both, except limitation 8, fixed on main. Limitation 15 was read in main's source and seen with 1.4.0.

## Current limitations

- **Security**:
  - The registry is reached over TLS, with a password when it asks ([REGISTRY.md](REGISTRY.md)), and only with a CA the fleet gives the device: there is no public CA bundle, and blobs redirected to another host are not followed.
  - The device key is in flash in clear, except in the `secure` variant (see [SECURITY.md](SECURITY.md)), tested in the emulator only, where it is encrypted. There the CPU can still read it, except on the ESP32-C5, where it is in the ECDSA peripheral. On the other chips the Digital Signature peripheral could hold it, but Flight Control cannot enroll the RSA key that needs.
- **Provisioning**: credentials are written over USB (`tools/provision.py`), the WiFi network over USB or in the field, over the device's own access point (see [PROVISIONING.md](PROVISIONING.md)). An enrollment certificate can be issued per batch, but Flight Control 1.4.0 cannot revoke it (its main branch can).
- **Offline longer than the certificate**: a device whose management certificate expired while it was offline (a year by default) enrolls again with its enrollment certificate as a new device, keeping its apps, config and firmware; the old record is left for the operator to delete (limitation 13).
- **Server unreachable**: a device that has had no status accepted for 10 minutes restarts, and so again every 10 minutes while the server stays out of reach; its apps restart with it. A write on a connection gives up after 30 s.
- **App formats**: apps must be WASM OCI artifacts (`application/vnd.wasm.content.layer.v1+wasm`). Podman-style images with a `.wasm` inside a tarball are not supported.
- **JSON depth**: the device refuses JSON nested more than 32 levels (spec, manifests, config files), and reports the error; real documents use fewer than 10.
- **Hardware coverage**: automatic rollback was not exercised on the C6 ([DEVICES.md](DEVICES.md#boards-tried)). The latest builds were tried on the classic ESP32, the C3, the C5, the C6 and the S3 (ESP-IDF and Zephyr).
- **Memory for apps**: CoreMark's 64 KB fits on the classic ESP32, the C3, the C5, the C6 and the S3, and with Zephyr on the ESP32-S3 N8R8, in PSRAM; wasm-coremark's 128 KB on the C6 and the S3 with PSRAM. On the classic ESP32 and the C5 an install or a swap can still fail for want of one free block of 68 KB (one try in seven on each, [CoreMark](#coremark)).

What comes next is in the [roadmap](ROADMAP.md).
