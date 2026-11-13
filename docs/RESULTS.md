# Results

What has been verified, where, and with which figures: microcontrollers managed by **Flight Control 1.4.0** (upstream of Red Hat Edge Manager), with no Linux on the device and **no changes to the server**. The server ran on kind on a Mac. Figures taken before 12 November 2026 are with Flight Control 1.3.1, whose agent API is the same ([below](#flight-control-140)); each section says which firmware it was measured with.

## What works

| Capability | How | Verified |
|---|---|---|
| Enrollment and identity | P-256 key generated on the device, CSR, approval with `flightctl approve`, mTLS with TLS 1.3 | Eight boards (the six Espressif chips with ESP-IDF; a Pico W and an ESP32-S3 with Zephyr), emulated C3, C5 and S3, and the simulated device |
| Monitoring | Full agent API status: resources, `customInfo` (heap, RSSI, temperature, traffic), and `Unknown` after 5 minutes offline | Works |
| Resource alerts | CPU, heap and storage sampled against the fleet's `spec.resources` rules, as the Linux agent does; the status gives `Warning` or `Critical` per resource and the summary turns `Degraded` or `Error` | In the end-to-end test: a memory rule from the fleet reported as `Warning` within 21 s, and Flight Control logged `DeviceMemoryWarning`, then `DeviceMemoryNormal` once the rule was removed. CPU from the idle time is in the firmware, not yet read on a board |
| Config without reboot | Inline `spec.config`, validated by the application before it is stored | Applied in 1–6 s. An invalid config is rejected and reported as OutOfDate with the error message |
| Firmware OTA | `spec.os.image` points to an OCI index with one firmware per chip. The download goes straight into the A/B slot and is checked with sha256 | Staged rollout (canary first, then the rest) across 3 devices in 50–80 s |
| Firmware deltas | A device on a known release downloads a patch from it and rebuilds the image from its own ([DELTA.md](DELTA.md)) | 1 to 12% of the image between builds of the same kind; on the Pico W and on the ESP32-S3 with both ports, the other ESP32 chips in the emulator |
| Automatic rollback | The new firmware is confirmed only after the server accepts a status from it; otherwise the device goes back to the previous one | A broken firmware rolls itself back; the rollout stops at the canary and the other devices never receive it. On the ESP32-S3 with both ports, the Pico W and the emulated chips |
| WASM apps without reboot | `spec.applications` of type `container` with a WASM OCI image, run by WAMR | Update from v1 to v2 on Xtensa, RISC-V and Arm with the same `.wasm` file, no reboot |
| Heterogeneous fleet | One fleet and one firmware reference, with per-device settings through labels and the template | S2, S3, C3, C5, C6 and classic ESP32 in the same fleet |
| Certificate renewal | The device renews its management certificate for the same key, through the renewal signer the server approves on its own | With 2-minute certificates, in the emulator and on the simulated device: the device keeps reporting after the first one expires. Not on a board, where a certificate lasts a year |
| Signed images | Firmware and apps signed in cosign's format; the device checks the signature against a key in its credentials | Unsigned images and images signed with another key are refused; `cosign verify` accepts the signatures |
| Registry with TLS and a password | The registry's CA and an `auth.json` from the fleet ([REGISTRY.md](REGISTRY.md)) | On the C3 (ESP-IDF), the Pico W and the ESP32-S3 (Zephyr), and in every end-to-end test |
| Decommissioning | Key and certificate wiped, no enrollment until the storage is erased | On the same three boards, 2.4 to 3 s from the request to `Completed`, and in the end-to-end tests |
| Automatic approval | An allow list of device names or MACs with their labels | Used by the end-to-end tests |
| Provisioning in the field | The WiFi network over SoftAP or BLE ([PROVISIONING.md](PROVISIONING.md)) | On the emulated C3, in CI |
| Secure Boot, flash encryption, anti-rollback | The `secure` variant ([SECURITY.md](SECURITY.md)) | On the emulated C3 and C5 only: no real chip has been burned |

## Measurements

### Firmware size

Release v3.2, ESP-IDF 6.0.3, debug build (`-Og`):

| Chip | Image | Free in the 1,664 KB OTA slot |
|---|---|---|
| ESP32 | 1,167 KB | 30% |
| ESP32-S2 | 1,140 KB | 31% |
| ESP32-S3 | 1,166 KB | 30% |
| ESP32-C3 | 1,241 KB | 25% |
| ESP32-C5 | 1,337 KB | 20% |
| ESP32-C6 | 1,337 KB | 20% |

The release profile (`VARIANT=release`: `-Os`, quieter logs) is about 15% smaller, on the C3 and the S3. BLE provisioning built in (`VARIANT=ble`) adds about 270 KB: 1,502 KB on the C3 with release v3.1, 10% of the slot free.

The largest parts, on the C3:

| Component | Flash (code and constants) |
|---|---|
| WiFi, WPA and lwIP | about 430 KB |
| Cryptography and TLS (mbedTLS 4, TF-PSA-Crypto) | about 197 KB |
| WiFi provisioning in the field (network_provisioning, protocomm, HTTP server) | about 93 KB |
| WAMR (fast interpreter) | 68 KB |
| **flightctl-mcu agent** (`core/`: enrollment, spec, status, resource alerts, OTA, OCI, signatures, apps, applying deltas) | **35 KB** |
| its libraries and ESP-IDF port (cJSON, the detools decoder with heatshrink, `fc_port_espidf.c`) | 14 KB |

The agent itself is small. Almost all the space goes to the network stack and to cryptography, which any securely connected device needs anyway.

### RAM

Internal heap with the `alarm` app running and both agent connections open, firmware v3.0 (ESP-IDF 5.5.6), flashed over USB once and then updated over the air to a signed release by its fleet:

| Chip | Free heap | Minimum | Largest internal block | TLS handshakes |
|---|---|---|---|---|
| ESP32-C3 | 56.8 KB | 35.6 KB | 36.9 KB | 2 |
| ESP32-C5 | 59.1 KB | 43.7 KB | 36.9 KB | 2 |
| ESP32-S3 | 118.3 KB | 99.2 KB | – | 2 |
| ESP32-C6 | 162.9 KB | 142.2 KB | 135.2 KB | 2 |
| Classic ESP32 | 73.4 KB | 56.1 KB | 65.5 KB | 2 |
| ESP32-S2 | 27.6 KB internal (+2 MB PSRAM) | – | 18.4 KB | 2 |

The S2 has the least room: its largest internal block is only just above the 16.7 KB a TLS handshake needs. The lowest point is when a TLS handshake and a firmware download run together.

With release v3.1 (ESP-IDF 6.0.3) the C3 had 70 KB free and 48 KB at the lowest after three hours. Release v3.2, which adds the resource alerts, leaves about 4 KB less: 65.6 KB free five minutes after boot with `alarm`, against 69.8 KB for v3.1 at the same point. With Zephyr: about 31 KB free on the Pico W, 49 KB on the ESP32-S3 ([DEVICES.md](DEVICES.md#boards-tried)).

What a WASM app costs, on the C3 (debug builds): the heap five minutes after boot with and without the `trend` app (1.9 KB of `.wasm`) differs by 21.4 KB with v3.1 and 17.7 KB with v3.2, which no longer keeps the `.wasm` in RAM once the module is loaded. Split on a host with the same module, what is left is the module loaded by WAMR's fast interpreter (about 5 KB), the instance and its WAMR stack (about 3.5 KB), the module's linear memory (about 1 KB) and the app's thread stack (8 KB).

Flashing the firmware does not touch the key storage: a board keeps its identity unless its whole flash is erased, and then enrolls again as a new device. The firmware logs a warning when it generates a new key, and an error when the key storage is unusable and has to be erased.

### Timings

On the ESP-IDF boards, with the early firmware (v2.x, ESP-IDF 5.5.6):

| Operation | Time |
|---|---|
| From boot to enrollment request (WiFi, SNTP, TLS) | about 10 s |
| Config change, from apply to device confirmation | 1–6 s |
| Firmware rollout to 3 devices (canary, then the rest) | 50–80 s, of which about 20 s per device for download and reboot |
| Rollback of a broken firmware | about 3.5 min (150 s confirmation window plus 2 reboots) |
| WASM app update across the fleet (canary, then the rest) | about 50 s, almost all of it waiting for the rollout; on the device the swap is immediate |

### Idle traffic

Measured on the device with the `netTxBytes` and `netRxBytes` counters (bytes on the wire, TLS included). The agent keeps its two connections open (HTTP keep-alive), sends a status at once when something changes and otherwise every 2 minutes, and the server's long-poll lasts about 4 minutes:

| Device | Total |
|---|---|
| Simulated device, no app (status every minute) | about 220 KB/hour: 87 KB sent, 130 KB received; no TLS handshake in 10 minutes |
| ESP32-C3 with the `alarm` app, firmware v3.0 | about 280 KB/hour, over 10 minutes that included two server restarts |

A connection per request cost 560 to 730 KB/hour: most of each request was the TLS 1.3 handshake with mTLS. What is left is the full status and the server's answer to it; going lower needs differential status updates (limitation 6 below). TLS session resumption was tried with Flight Control 1.3.1: it issued session tickets but did not resume a session with them, so a reconnection costs a full handshake (about 1.5 KB received).

An app whose state flaps sends a status at most every 15 s (problem 11 below).

### Signed updates and kept connections on a board (ESP32-C3)

The C3 SuperMini, in a fleet of its own, with the `alarm` app (firmware v2.7 to v3.0, ESP-IDF 5.5.6):

| Check | Result |
|---|---|
| Signed firmware update, v2.7 to v2.11 | Installed and confirmed in 24 s. From v2.11 on the device checks signatures: the next update (v2.12, signed) went through in 22 s |
| Unsigned app | Refused ("is not signed"), the signed app keeps running, Flight Control shows the device OutOfDate with the reason |
| Kept connections | 2 TLS handshakes after boot, none in the next 10 minutes |
| Server restart | Both connections reopened (2 more handshakes), next spec applied in 2 s |
| Internal heap with the two connections open | about 58–60 KB free, minimum since boot 38 KB (v2.12) |
| Idle traffic | 1.5 MB/hour with v2.11, then about 690 KB/hour with v2.12 (problem 11); about 280 KB/hour with v3.0 (2-minute heartbeat, trimmed status), over 10 minutes that included two server restarts and no alarm flapping |

Certificate renewal was not run on the board, whose certificate is valid for a year; it ran in the emulator, below.

### In Espressif's emulator

[esp-emu](https://github.com/espressif/esp-emulator) 0.51 runs the real firmware for the C3, C5 and S3 (not the S2 or the classic ESP32). Its WiFi access point NATs to the host, so the emulated chip enrolls in the same Flight Control as the boards. With the server issuing 4-minute certificates:

| Chip | Signed OTA (reboot into the other slot) | Unsigned app | Certificate renewal | Heap free / minimum / largest block |
|---|---|---|---|---|
| ESP32-C3 | confirmed, 19 s | refused | 6 renewals, still reporting after the first certificate expired | 57.8 / 37.6 KB / – |
| ESP32-C5 | confirmed, 15 s | refused | renewed, still reporting after the first certificate expired | 61.7 / 42.2 / 40.9 KB (after problem 12) |
| ESP32-S3 | confirmed, 35 s | – | renewed | 121.3 / 102.1 / 73.7 KB |

The C3 figures match the board (58–60 KB free). `tests/e2e/e2e-emu.sh` runs the whole sequence for the C3 and the C5, in CI too. Since the credentials moved out of the firmware (`fc_creds` partition), it tests the same images CI builds for the boards, provisioned for the test cluster: it boots the debug build, updates it over the air to the release build (72 KB free on the C3, against 58 KB for the debug build), and tries a broken build that cannot reach the server, which is rolled back once its confirmation window ends (150 s by default, 30 s in the test build).

### ESP-IDF 6.0.3 on a board (ESP32-S3)

12 November 2026, against Flight Control 1.4.0: the first run of the ESP-IDF 6 firmware (mbedTLS 4.1.1, cryptography through PSA) outside the emulator, on an ESP32-S3-DevKitC with the debug build:

| Step | Result |
|---|---|
| Enrollment | key generated on the chip, TLS 1.3 with the enrollment certificate, management certificate for a year |
| Config and WASM app | config file written, the signed `alarm` app installed and running |
| Firmware, whole image | 1181 KB downloaded in 5 s into the other slot, restarted, confirmed |
| Firmware, as a delta | the release build (v4.0, 1000 KB) from a delta of 511 KB, rebuilt from the running image in 13 s, restarted, confirmed |
| Memory (release build, app running) | 145 KB of heap free, 124 KB at the lowest |

The delta is half of the image because it goes from the debug build to the release one, compiled differently; between two builds of the same kind it is 1 to 12% ([DELTA.md](DELTA.md)).

Not run on a board: the secure variant (it burns eFuses). The ESP32-C3 ran ESP-IDF 6.0.3 later ([below](#the-registry-with-tls-and-decommissioning-on-boards)); the C5, C6, S2 and classic ESP32 have not.

### The way back on the ESP32-S3, both ports

12 November 2026, against Flight Control 1.4.0, an ESP32-S3 with each port in turn. Each was given, through its fleet, a firmware built to reach Flight Control on a wrong port (9443) and to give itself 30 seconds instead of 150:

| | ESP-IDF 6.0.3 (v5.0) | Zephyr 4.5 with MCUboot |
|---|---|---|
| Before | the release build, come as a delta of 512 KB from the debug one (`lastDelta: Applied`) | a build come as a delta of 28 KB (`lastDelta: Applied`) |
| Broken image | 1181 KB, whole (`NotUsed`), downloaded and restarted in 11 s | 885 KB, whole, in 14 s |
| Given up | 30 s after the agent started, by the image itself | the same |
| Back | the bootloader takes the previous slot | MCUboot swaps the slots back: 15 s |
| Reported | 53 s after the fleet changed: "failed to start and was rolled back; not retried" | 1 min 53 s, the same message |

Neither tried the image again.

### The registry with TLS, and decommissioning, on boards

November 2026, against Flight Control 1.4.0, with the registry's door with TLS and a password ([REGISTRY.md](REGISTRY.md)). The CA and `auth.json` came from the fleet; decommissioning was sent with `flightctl decommission`.

| | ESP32-C3, ESP-IDF 6.0.3 | Pico W, Zephyr | ESP32-S3, Zephyr |
|---|---|---|---|
| App over TLS | running 3 s after the config, no restart | about 6 s | about 6 s |
| Firmware over TLS, whole | 1234 KB in 5 s | 729 KB in 24 s | 886 KB |
| Firmware over TLS, as a delta | not tried | not tried | 17 KB (`Applied`) |
| Heap with both connections open (app running) | 70 KB free, 48 KB at the lowest, after 3 hours | not read | not read |
| Decommissioning, from request to Completed | 3 s | 2.5 s | 2.4 s |

After decommissioning each board wiped its key and management certificate, restarted, showed as `Decommissioned`, and kept running its app without asking to enroll again. Erased, each enrolled again as a new device.

Not measured: the RAM used during a download over TLS (the reboot that follows a firmware download clears the low-water mark).

Also found: Flight Control serves inline content given in base64 percent-encoded (limitation 12 below).

## Problems found and fixed

1. **The rollout moved on while the canary was rebooting.**
   - The device reported the new `renderedVersion` right after the download.
   - Flight Control therefore counted the canary as updated and moved on to the others before the new firmware was confirmed.
   - Fix: the device keeps reporting the previous version until the new firmware is confirmed. With this fix the rollout stops at the canary.
2. **WASM linear memory.**
   - Every WASM module reserves at least one 64 KB page, which does not fit on the C3/C5.
   - WAMR shrinks it to the size actually used only if the module exports both `__heap_base` and `__data_end`.
3. **Invalid config reported as up to date.**
   - The version was advanced even when the config was rejected.
   - Fix: it now stays at the previous version, and the error appears in `updated.info`.
4. **Classic ESP32: status stuck for about 6 minutes** after an OTA.
   - It did not happen again, so the cause is not proven. Later, a TLS handshake failed once with "generic error".
   - The likely cause is a socket write with no timeout. On a half-open connection, for example after a WiFi drop, `send()` waits until TCP gives up, which can take minutes on lwIP.
   - Fixes:
     - writes now time out after 30 s (`SO_SNDTIMEO`);
     - a watchdog restarts the device if no status has been accepted for 10 minutes.
5. **App state reached Flight Control up to 60 s late.**
   - Changes of app state are now sent right away, at most one status every 3 s.
   - The end-to-end test sees an app update reflected in about 2 s. On the C3 (firmware v2.7) the alarm state reached Flight Control 2 s after the new threshold was applied.

6. **ESP32-S2: not enough internal RAM, then corrupted TLS records.**
   - With internal RAM only, the TLS handshake could not allocate its 16 KB receive buffer.
   - With the TLS buffers in PSRAM, records got corrupted: the server logged "bad record MAC" and answered status updates with 400. The device kept running, but Flight Control kept showing its last accepted status, and new firmware never got confirmed, so it rolled back.
   - Fix: PSRAM for allocations above 1 KB, TLS buffers in internal RAM, with 48 KB reserved for internal-only allocations.
7. **ESP32-S2: temperature reads could hang.**
   - On the S2, ESP-IDF's sensor read sets a bit, busy-waits for the result and clears the bit, without a lock. The status thread, the alarm task and the WASM app all read it, and two overlapping reads can leave one spinning forever.
   - Fix: a lock around the read in `port/espidf`, for every chip.
8. **Chunked HTTP bodies: integer overflow** (found by fuzzing).
   - A chunk size close to the maximum of an unsigned long wrapped the bounds check, in code from the first version.
   - Fix: chunk sizes are parsed by hand and capped; the input is kept as a fuzzing seed.
9. **Layer size from a manifest: undefined conversion** (found by fuzzing).
   - A size such as `1e400` became infinity, then was cast to an integer.
   - Fix: sizes outside 1 byte to 64 MB are rejected.
10. **Certificate renewal: use after free** (found in review).
    - A long-poll could still refer to a certificate that two renewals had already replaced and freed.
    - Fix: each request works on its own copy of the certificate.
11. **A flapping app state flooded the network** (found on the C3).
    - The C3 sensor reads in steps of 1 °C, and the chip sat right at the 40 °C threshold: the app went from alarm to pre-alarm and back several times a minute, and every change sent a status, up to one every 3 s. Each status costs about 5 KB on the wire with the server's answer, so the idle C3 used 1.5 MB/hour.
    - Fix: the agent's own changes (spec applied, firmware confirmed) are still reported within a few seconds; changes reported by apps and the firmware are grouped, one status every 15 s at most. Same C3, same flapping: about 690 KB/hour.
    - What is left is mostly the size of each status and of the server's answer to it (limitation 6 below).
12. **ESP32-C5: TLS handshakes failed for lack of memory** (found in the emulator).
    - With the two kept connections, the largest free block went down to 19 KB, and a handshake needs 16.7 KB in one piece for the TLS receive buffer. When both connections opened together (at boot, and after each certificate renewal) one failed and was retried later.
    - Fix: on the C5, WiFi code stays in flash and the WiFi stack has fewer buffers, as on the S2. Free heap went from 43.6 to 61.7 KB, the largest block from 19 to 41 KB, and no handshake failed in 5 minutes with a renewal every minute.
13. **A device offline for longer than its certificate cannot come back** (found in the emulator).
    - In the rollback test the broken build kept the device away from the server for 3 minutes, longer than the 4-minute test certificate. Back on the good firmware, every request was refused: the certificate had expired, and renewal needs a valid one.
    - With the default lifetime (one year, renewed after three months) this takes nine months offline. The rollback test now goes back to the normal lifetime before it starts.
    - Fix: after a failed status update with an expired certificate, the device drops its key and certificate and enrolls again with the enrollment certificate, as a new device (the old enrollment request is approved already and cannot issue another certificate). Apps, config and firmware stay; the allow list approves it again by MAC; the old device record is left for the operator to delete. The end-to-end test stops the server until the certificate has expired and checks that the device comes back under a new name with its config.
14. **A deeply nested JSON document crashed the firmware in a loop** (found in the emulator).
    - A fleet config holding a JSON value nested 500 levels deep made the C3 overflow its 24 KB thread stack while parsing it ("Stack protection fault"), reboot, get the same config and crash again. A registry could do the same with a manifest, which is parsed before its signature is checked. The fuzzers run on the host, with 256 KB stacks, and could not see it.
    - The real cause was two copies of cJSON in the firmware: ours and ESP-IDF's (component `json`), and the firmware ran ESP-IDF's.
    - Fix: the firmware builds only the ESP-IDF components it needs (`set(COMPONENTS main)`; the build fails if `json` comes back), and cJSON's nesting limit is 32 instead of 1000 (real documents use fewer than 10). The same config is now rejected with an error in Flight Control. Side effect: a clean firmware build takes under a minute instead of three.
    - Later, WiFi provisioning in the field needed ESP-IDF's `json` component: the firmware now links only ESP-IDF's cJSON, built with the same limit of 32 (`port/espidf/CMakeLists.txt`), and no longer ours.
15. **An app with a function pointer did not load.**
    - clang 20 and later encode `call_indirect` with the reference-types layout by default, and the firmware's WAMR, built without reference types, refuses it ("zero byte expected").
    - Fix: apps are built for the WebAssembly MVP (`-mcpu=mvp` in `wasm-apps/build.sh`).
16. **Each app was held in RAM twice, and could be lost on a full flash.**
    - The agent kept the `.wasm` bytes and gave WAMR a second copy to load, for as long as the app ran. A new version was stored only after the old one was stopped, without checking the write: on a full or failing flash the app ran until the next reboot and was then gone.
    - Fix: the module is loaded once, with WAMR copying what it needs, and the bytes are freed right after. A new version is stored first; if that fails, the running version keeps running and the update is retried. A stored module that no longer loads is reported in the status and downloaded again with the next spec.

## Flight Control 1.4.0

12 November 2026. The service on kind was upgraded in place from 1.3.1 with the Helm chart, with a Raspberry Pi Pico W enrolled and online: it stayed enrolled, came back `Online` and `UpToDate` by itself, and its free heap dipped from 22 to 17 KB at the lowest while the server restarted.

- **The agent API is the same.** No path changed between the two releases, and the schemas only gained optional fields: `status.systemInfoStatus`, `status.os.lastDelta` and `deltaSize`, `spec.os.deltaImage` in the rendered spec, `systemInfo.osMode`. The agent sends none and ignores the last.
- **Every end-to-end test passes unchanged**: the simulated device, Zephyr on `native_sim`, and the firmware in the emulator (C3 with SoftAP and with BLE provisioning, C3 and C5 secure). One run of the secure C3 failed once and passed when run again; the likely cause, not proven, is an overloaded host.
- **Flight Control now has deltas of its own**, for bootc images, made by the service. They are another mechanism: these devices find theirs in the release's index ([DELTA.md](DELTA.md)). `status.os.lastDelta` would be the place to report the outcome, instead of `customInfo`.
- **cosign 3**, which GitHub's installer now gives, accepts the signatures `tools/signing/sign.py` makes, as cosign 2 does.

What changed for a deployment is the **label sync**. 1.4 installs `LabelSyncMapping` resources, and one of them copies every entry of `customInfo` into the device's labels (`custominfo.flightctl.io/<key>`). These devices report counters there: free heap, traffic, uptime. With the default, each status update rewrote the device's labels and logged two events ("label reconciliation was requested", "updated successfully (labels)"): 227 of 400 events on the test cluster, while the tests ran.

`tools/fleet/labelsync-custom-info.yaml` replaces that mapping with one that copies only what stays the same between firmware updates (device class, board, firmware and OS version, security state). The labels then stop changing, and so does the second event. The first is logged whenever anything in the status changes (the service compares the whole status, `lastSeen` aside), before it knows nothing will come of it (limitation 10 below).

Two things changed at every status, so each status was an event: the counters in `customInfo`, and the time of the `Updating` condition, which the agent set to the time of the status. Now the condition's time changes only with the condition, as in Kubernetes, and the counters are read at the start, five minutes later, then once an hour; the statuses in between repeat them. The simulated device, with no app, over 9 minutes (a status a minute):

| | Events |
|---|---|
| Before | 10, one per status |
| After | 2: at the start, and when the counters were read again five minutes later; then one an hour |

What an app reports is not held back: the `alarm` app on the Zephyr boards, whose temperature is simulated and swings through its thresholds, changes its state about twice a minute, and each change is an event (30 in 15 minutes on the ESP32-S3).

The labels a mapping makes are its own: editing a device without them, to change another label, is refused (409, "cannot edit a managed label owned by a mapping"). An edit must carry them as they are.

One thing the new labels give: a fleet can select on what a device says it is, `custominfo.flightctl.io/deviceClass=zephyr` for instance, which is part of what limitation 4 asks for.

## Flight Control limitations found (upstream proposals)

1. **`matchExpressions` in the fleet selector is accepted but ignored.**
   - The fleet stays empty with no error. The controller only uses `matchLabels` (`internal/tasks/fleet_selector.go`).
   - Proposal: support it, or reject it at validation.
2. **Batch success is based only on `renderedVersion`.**
   - An agent that reports the version before confirming the new OS makes the rollout advance. The Linux agent avoids this implicitly.
   - Proposal: write this requirement into the agent API contract, or base success also on `os.image` and the `Updating` condition.
3. **Fixed 5-minute disconnect timeout** (`DeviceDisconnectedTimeout`).
   - Battery devices in deep sleep would be `Unknown` most of the time.
   - Proposal: make it configurable per fleet or per device, or let the device declare its expected contact interval.
4. **No way to declare the device class or the supported runtimes.**
   - For an MCU the server also accepts `compose`, `quadlet` or `helm` apps, and Podman fields (ports, volumes, `runAs`) that mean nothing there. The error only surfaces on the device.
   - Proposal: extend `status.capabilities` (which today has `osMode`) with something like `applicationRuntimes: [wasm]`, and validate rendering against it.
   - Possibly also add an explicit `appType: wasm`, or recognize images with platform `wasip1/wasm` as WASM.
5. **No free-form status field per app.**
   - `DeviceApplicationStatus` has no field such as `message` or `info`.
   - The app state ended up in `customInfo` (`app.<name>`).
6. **Cost of each status update.**
   - Every PUT sends the full status, 2–3 KB of JSON.
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
   - The default `custom-info` mapping copies all of `customInfo` into labels, and the service logs "label reconciliation was requested" at every change in the status (`internal/service/common/device.go`: `deviceStatusesEqual`), even when no label changes ([above](#flight-control-140)).
   - `customInfo` is the only free-form place in the status, so a constrained device's counters end up there (limitation 5).
   - Proposal: ask for a reconciliation only when a mapping's inputs changed, and do not copy all of `customInfo` by default; or give counters a place of their own.
11. **A fleet's OS image cannot come from the software catalog.**
   - The fleet accepts `spec.os.catalogItemRef` and keeps it in its template version, but the device gets `os: {}`: building the device's spec from the template copies `os.image` only (`internal/tasks/desired_spec.go`: `assembleDesiredSpec`). The device is reported `UpToDate` and its firmware never changes, with no error.
   - Tried on the Pico W on 12 November 2026, with a catalog for the Zephyr firmware and the `alarm` app. The app's `catalogItemRef` works: the server turned version `2.0.1-pico` into `…/alarm:v2.pico`, and the agent got the image reference as before. Unchanged on `main` the same day.
   - Proposal: copy `os.catalogItemRef` too, as is done for the applications.
12. **An inline file given in base64 reaches the device percent-encoded.**
   - The server keeps inline files as data URLs (`pkg/ignition/ignition_wrapper.go`: `SetFile`). For content already in base64 it sets the URL's encoding to ASCII, which percent-encodes the text. Building the rendered spec (`internal/tasks/device_render.go`: `ignitionConfigToRenderedConfig`) then cuts off the `data:...,` prefix and serves what is left as base64, escapes included. `aGk=` arrives as `aGk%3D`.
   - The Linux agent decodes with Go's `base64.StdEncoding` (`api/core/v1beta1/types.go`: `decodeContents`), which refuses `%`. Base64 content whose length is not a multiple of three, which is most of it, should then fail to be written (read in the source, not tried with the Linux agent).
   - Found on 12 November 2026 with an `auth.json` for the registry ([REGISTRY.md](REGISTRY.md)), in the rendered spec of a device on 1.4.0. Content given as text is turned into proper base64 by the same code and arrives intact. This agent now undoes the escapes before decoding, and the tools give text.
   - Proposal: unescape the data URL's text (or use the URL's own decoding) before serving it as base64.

13. **A device cannot enroll again under its own name.**
   - An enrollment request for a name that already has a device is refused (`internal/service/enrollmentrequest/handler.go`: `ErrDuplicateName`). A device whose management certificate expired while it was offline can only enroll again under a new key, hence a new name; the old record stays until an operator deletes it (problem 13).
   - Proposal: let an enrollment certificate re-enroll a known device, with the same approval as a new one.

All of these were read again in the source of 1.4.0 and of Flight Control's main branch of 12 November 2026: they hold in both, except limitation 8, fixed on main.

## Current limitations

- **Security**:
  - The registry can be reached over TLS with a password ([REGISTRY.md](REGISTRY.md)), but only with a CA the fleet gives the device: there is no public CA bundle, and blobs redirected to another host are not followed. Over plain HTTP, signatures make the registry safe against tampering, not against reading.
  - The device key is in flash in clear, except in the `secure` variant (see [SECURITY.md](SECURITY.md)), tested in the emulator only, where it is encrypted. There the CPU can still read it, except on the ESP32-C5, where it is in the ECDSA peripheral. On the other chips the Digital Signature peripheral could hold it, but Flight Control cannot enroll the RSA key that needs.
- **Provisioning**: credentials are written over USB (`tools/provision.py`), the WiFi network over USB or in the field, over the device's own access point or BLE (see [PROVISIONING.md](PROVISIONING.md)). An enrollment certificate can be issued per batch, but Flight Control 1.4.0 cannot revoke it (its main branch can).
- **App formats**: apps must be WASM OCI artifacts (`application/vnd.wasm.content.layer.v1+wasm`). Podman-style images with a `.wasm` inside a tarball are not supported.
- **Hardware coverage**: automatic rollback was not exercised on the C6 ([DEVICES.md](DEVICES.md#boards-tried)).

What comes next is in the [roadmap](ROADMAP.md).
