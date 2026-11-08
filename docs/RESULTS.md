# Results

What has been built and measured so far: Espressif microcontrollers managed by **Flight Control 1.3.1** (upstream of Red Hat Edge Manager), with no Linux on the device and **no changes to the server**. The server ran on kind on a Mac.

The work is split in milestones (see the [roadmap](ROADMAP.md)): the proof of concept (milestone 0, 5–6 November 2026) and the production foundations (milestone 1, 8 November 2026). Figures are from milestone 0 unless a section says otherwise.

## What works

| Capability | How | Result |
|---|---|---|
| Enrollment and identity | P-256 key generated on the device, CSR, approval with `flightctl approve`, mTLS with TLS 1.3 | 7 MCU devices enrolled, plus a fake device on the host |
| Monitoring | Full agent API status: resources, `customInfo` (heap, RSSI, temperature, traffic), and `Unknown` after 5 minutes offline | Works |
| Config without reboot | Inline `spec.config`, validated by the application before it is stored | Applied in 1–6 s. An invalid config is rejected and reported as OutOfDate with the error message |
| Firmware OTA | `spec.os.image` points to an OCI index with one firmware per chip. The download goes straight into the A/B slot and is checked with sha256 | Staged rollout (canary first, then the rest) across 3 devices in 50–80 s |
| Automatic rollback | The new firmware is confirmed only after the server accepts a status from it; otherwise the device goes back to the previous one | A broken firmware rolls itself back in about 3.5 minutes. The rollout stops at the canary and the other devices never receive it |
| WASM apps without reboot | `spec.applications` of type `container` with a WASM OCI image, run by WAMR | Update from v1 to v2 on Xtensa and RISC-V with the same `.wasm` file, no reboot |
| Heterogeneous fleet | One fleet and one firmware reference, with per-device settings through labels and the template | S2, S3, C3, C5, C6 and classic ESP32 in the same fleet |
| Certificate renewal (milestone 1) | The device renews its management certificate for the same key, through the renewal signer the server approves on its own | Tested with 2-minute certificates: the device keeps reporting after the first one expires |
| Signed images (milestone 1) | Firmware and apps signed in cosign's format; the device checks the signature against a key in its firmware | Unsigned images and images signed with another key are refused; `cosign verify` accepts our signatures |
| Automatic approval (milestone 1) | An allow list of device names or MACs with their labels | Used by the end-to-end test |

## Fleet tested

| Chip | Arch | Board | Temperature sensor | LED | Phases tested |
|---|---|---|---|---|---|
| ESP32-S3 (×2) | Xtensa LX7 | DevKitC N8R8 / N4R2 | internal | WS2812 on GPIO 38, RGB order (label) | F2–F5 |
| ESP32-C3 | RISC-V | SuperMini | internal | blue, active low, GPIO 8 (label) | F2–F5 |
| ESP32-C5 | RISC-V | DevKitC | internal (about 15 °C below the others) | WS2812 on GPIO 27 | F2–F5 |
| Classic ESP32 | Xtensa LX6 | DevKit | **simulated** (no supported sensor) | GPIO 2 | F2–F5 |
| ESP32-C6 | RISC-V | DevKitC | internal | WS2812 on GPIO 8, RGB order (label) | F2–F5 (rollback not exercised). Flashing works only over the native USB port, with the board powered up holding BOOT; after flashing it needs a replug |
| ESP32-S2 | Xtensa LX7 | Lolin S2 Mini (S2FNR2: 4 MB flash, 2 MB PSRAM) | internal | blue, GPIO 15 | F2–F5. Needs its PSRAM. Native USB only: flash it from download mode (hold BOOT while plugging in) |
| macOS host | arm64 | fake device | simulated | printed to the terminal | F1, F3, F5 (testing without hardware) |

## Measurements

### Firmware (v2.6, ESP-IDF 5.5.6, default `-Og` build)

| Chip | Image | Free space in the OTA slot (1.6 MB) |
|---|---|---|
| ESP32 | 1,081 KB | 34% |
| ESP32-S3 | 1,077 KB | 35% |
| ESP32-C3 | 1,148 KB | 30% |
| ESP32-C5 | 1,247 KB | 25% |
| ESP32-C6 | 1,186 KB | 28% |

Breakdown on the C3 (code and data in flash):

| Component | Size |
|---|---|
| WiFi and lwIP | about 280 KB |
| mbedTLS (crypto, TLS, X.509) | about 222 KB |
| WAMR (fast interpreter) | 71 KB |
| **flightctl-mcu agent** (`fc_core`: enrollment, spec, status, OTA, OCI, apps) | **21 KB** |

The agent itself is small. Almost all the space goes to the network stack and to cryptography, which any securely connected device needs anyway.

### RAM (internal heap, PSRAM off, with WAMR and the `alarm` app running)

| Chip | Free in steady state | Minimum since boot |
|---|---|---|
| ESP32-S3 | about 150 KB | 26 KB (during the OTA download) |
| ESP32-C3 | about 87 KB | 39 KB |
| ESP32-C6 | about 177 KB | 140 KB |
| ESP32-S2 | about 40 KB internal (largest block 23 KB), plus 2 MB PSRAM | — |
| ESP32-C5 | about 68 KB | 29 KB |
| Classic ESP32 | about 88 KB | 60 KB |

The peak is two TLS 1.3 handshakes in parallel (status thread and spec thread) plus the OTA download. The C5 has the smallest margin.

### Timings

| Operation | Time |
|---|---|
| From boot to enrollment request (WiFi, SNTP, TLS) | about 10 s |
| Config change, from apply to device confirmation | 1–6 s |
| Firmware rollout to 3 devices (canary, then the rest) | 50–80 s, of which about 20 s per device for download and reboot |
| Rollback of a broken firmware | about 3.5 min (150 s confirmation window plus 2 reboots) |
| WASM app update across the fleet (canary, then the rest) | about 50 s, almost all of it waiting for the rollout; on the device the swap is immediate |

### Idle traffic

Measured on the device with the `netTxBytes` and `netRxBytes` counters (bytes on the wire, TLS included) over 869 s, with a status every 60 s and the server's long-poll of about 4 minutes:

| Device | Sent | Received | Total |
|---|---|---|---|
| Classic ESP32 (with WASM app) | 236 KB/h | 493 KB/h | **about 730 KB/h, about 17 MB per day** |
| Fake device on the host (no app) | 183 KB/h | 380 KB/h | about 560 KB/h |

Each request cost about 10 KB, and most of that was the TLS 1.3 handshake with mTLS, because the agent opened a new connection for every request. The status JSON is 2–3 KB. The 25 minutes of ESP32 serial log showed no errors and no hangs.

**Milestone 1, with kept connections** (HTTP keep-alive, one connection per agent thread), measured over 10 minutes on the fake device:

| Device | Sent | Received | Total | TLS handshakes |
|---|---|---|---|---|
| Fake device on the host (no app) | 87 KB/h | 130 KB/h | **about 220 KB/h** (was 560) | 0 |

What is left is the full status sent every minute and the server's answer to it. Going lower needs less frequent or differential status updates (limitation 6 below). TLS session resumption was also tried: Flight Control 1.3.1 issues session tickets but did not resume a session with them, so a reconnection costs a full handshake (about 1.5 KB received).

### Release build (milestone 1)

The release profile (`VARIANT=release`: `-Os`, quieter logs) makes the C3 image 984 KB instead of 1,159 KB, about 15% smaller, with 41% of the OTA slot free.

### Milestone 1 on a board (ESP32-C3)

The C3 SuperMini, in a fleet of its own, with the `alarm` app:

| Check | Result |
|---|---|
| Signed firmware update, v2.7 to v2.11 | Installed and confirmed in 24 s. From v2.11 on the device checks signatures: the next update (v2.12, signed) went through in 22 s |
| Unsigned app | Refused ("is not signed"), the signed app keeps running, Flight Control shows the device OutOfDate with the reason |
| Kept connections | 2 TLS handshakes after boot, none in the next 10 minutes |
| Server restart | Both connections reopened (2 more handshakes), next spec applied in 2 s |
| Internal heap with the two connections open | about 58–60 KB free, minimum since boot 38 KB (v2.12) |
| Idle traffic | 1.5 MB/hour with v2.11, then about 690 KB/hour with v2.12: see problem 11 |

Certificate renewal was not run on the board, whose certificate is valid for a year; it ran in the emulator, below.

### Milestone 1 in Espressif's emulator

[esp-emu](https://github.com/espressif/esp-emulator) 0.51 runs the real firmware for the C3, C5 and S3 (not the S2 or the classic ESP32). Its WiFi access point NATs to the host, so the emulated chip enrolls in the same Flight Control as the boards. With the server issuing 4-minute certificates:

| Chip | Signed OTA (reboot into the other slot) | Unsigned app | Certificate renewal | Heap free / minimum / largest block |
|---|---|---|---|---|
| ESP32-C3 | confirmed, 19 s | refused | 6 renewals, still reporting after the first certificate expired | 57.8 / 37.6 KB / – |
| ESP32-C5 | confirmed, 15 s | refused | renewed, still reporting after the first certificate expired | 61.7 / 42.2 / 40.9 KB (after problem 12) |
| ESP32-S3 | confirmed, 35 s | – | renewed | 121.3 / 102.1 / 73.7 KB |

The C3 figures match the board (58–60 KB free). `ci/e2e-emu.sh` runs the whole sequence for the C3 and the C5, in CI too, including a broken firmware (it cannot reach the server) that is rolled back in about 3 minutes.

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
8. **Chunked HTTP bodies: integer overflow** (milestone 1, found by fuzzing).
   - A chunk size close to the maximum of an unsigned long wrapped the bounds check, in code that dated from the proof of concept.
   - Fix: chunk sizes are parsed by hand and capped; the input is kept as a fuzzing seed.
9. **Layer size from a manifest: undefined conversion** (milestone 1, found by fuzzing).
   - A size such as `1e400` became infinity, then was cast to an integer.
   - Fix: sizes outside 1 byte to 64 MB are rejected.
10. **Certificate renewal: use after free** (milestone 1, found in review).
    - A long-poll could still refer to a certificate that two renewals had already replaced and freed.
    - Fix: each request works on its own copy of the certificate.
11. **A flapping app state flooded the network** (milestone 1, found on the C3).
    - The C3 sensor reads in steps of 1 °C, and the chip sat right at the 40 °C threshold: the app went from alarm to pre-alarm and back several times a minute, and every change sent a status, up to one every 3 s. Each status costs about 5 KB on the wire with the server's answer, so the idle C3 used 1.5 MB/hour.
    - Fix: the agent's own changes (spec applied, firmware confirmed) are still reported within a few seconds; changes reported by apps and the firmware are grouped, one status every 15 s at most. Same C3, same flapping: about 690 KB/hour.
    - What is left is mostly the size of each status and of the server's answer to it (limitation 6 below).
12. **ESP32-C5: TLS handshakes failed for lack of memory** (milestone 1, found in the emulator).
    - With the two kept connections, the largest free block went down to 19 KB, and a handshake needs 16.7 KB in one piece for the TLS receive buffer. When both connections opened together (at boot, and after each certificate renewal) one failed and was retried later.
    - Fix: on the C5, WiFi code stays in flash and the WiFi stack has fewer buffers, as on the S2. Free heap went from 43.6 to 61.7 KB, the largest block from 19 to 41 KB, and no handshake failed in 5 minutes with a renewal every minute.

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

## Current limitations

- **Security**:
  - The registry is plain HTTP without authentication. Signatures protect the images from tampering, not from being read.
  - No Secure Boot v2 and no flash encryption: the device key is stored in NVS unencrypted.
- **WiFi configured at build time.** There is no provisioning (BLE or SoftAP), and no way to transfer the enrollment config either.
- **App formats**: only the WASM OCI artifact. Podman "compat" images (with the `.wasm` in a tar.gz layer) are not supported.
- **Hardware coverage**: automatic rollback was not exercised on the C6. Milestone 1 ran on one board, the C3, and in the emulator for the C3, C5 and S3; S2 and classic ESP32 have neither.

What comes next is in the [roadmap](ROADMAP.md).
