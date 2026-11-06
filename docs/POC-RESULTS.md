# flightctl-mcu PoC: results

Espressif microcontrollers managed by **Flight Control 1.3.1** (upstream of Red Hat Edge Manager), with no Linux on the device and **no changes to the server**. The server ran on kind on a Mac. The PoC was carried out on 5–6 November 2026.

## What was demonstrated

| Capability | How | Result |
|---|---|---|
| Enrollment and identity | P-256 key generated on the device, CSR, approval with `flightctl approve`, mTLS with TLS 1.3 | 5 MCU devices enrolled, plus a fake device on the host |
| Monitoring | Full agent API status: resources, `customInfo` (heap, RSSI, temperature, traffic), and `Unknown` after 5 minutes offline | Works |
| Config without reboot | Inline `spec.config`, validated by the application before it is stored | Applied in 1–6 s. An invalid config is rejected and reported as OutOfDate with the error message |
| Firmware OTA | `spec.os.image` points to an OCI index with one firmware per chip. The download goes straight into the A/B slot and is checked with sha256 | Staged rollout (canary first, then the rest) across 3 devices in 50–80 s |
| Automatic rollback | The new firmware is confirmed only after the server accepts a status from it; otherwise the device goes back to the previous one | A broken firmware rolls itself back in about 3.5 minutes. The rollout stops at the canary and the other devices never receive it |
| WASM apps without reboot | `spec.applications` of type `container` with a WASM OCI image, run by WAMR | Update from v1 to v2 on Xtensa and RISC-V with the same `.wasm` file, no reboot |
| Heterogeneous fleet | One fleet and one firmware reference, with per-device settings through labels and the template | S3, C3, C5 and classic ESP32 in the same fleet |

## Fleet tested

| Chip | Arch | Board | Temperature sensor | LED | Phases tested |
|---|---|---|---|---|---|
| ESP32-S3 (×2) | Xtensa LX7 | DevKitC N8R8 / N4R2 | internal | WS2812 on GPIO 38, RGB order (label) | F2–F5 |
| ESP32-C3 | RISC-V | SuperMini | internal | blue, active low, GPIO 8 (label) | F2–F5 |
| ESP32-C5 | RISC-V | DevKitC | internal (about 15 °C below the others) | WS2812 on GPIO 27 | F2–F5 |
| Classic ESP32 | Xtensa LX6 | DevKit | **simulated** (no supported sensor) | GPIO 2 | F2–F5 |
| ESP32-C6 | RISC-V | DevKitC | internal | WS2812 on GPIO 8 | built only: flashing this board fails (stub checksum error) |
| ESP32-S2 | Xtensa LX7 | — | — | — | not tested (board not connected) |
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

Each request costs about 10 KB, and most of that is the TLS 1.3 handshake with mTLS, because the PoC opens a new connection for every request. The status JSON is 2–3 KB.

Traffic goes down mostly with keep-alive connections or TLS session resumption, and with less frequent or differential status updates. The current level is fine on WiFi but needs optimizing for NB-IoT or LTE-M. The 25 minutes of ESP32 serial log showed no errors and no hangs.

## Problems found and fixed during the PoC

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
   - The end-to-end test sees an app update reflected in about 2 s.

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

## PoC limitations (not production ready)

- **Security**:
  - The registry is plain HTTP without authentication.
  - Firmware and apps are **not signed**: there is no cosign, no signature verified by the bootloader (Secure Boot v2), and no flash encryption.
  - The device key is stored in NVS unencrypted.
- **WiFi configured at build time.** There is no provisioning (BLE or SoftAP), and no way to transfer the enrollment config either.
- **Management certificate renewal is not implemented.** The API for it (`certificatesigningrequests`) exists.
- **One TLS connection per request**, without keep-alive. It is simple, but it increases traffic and power use.
- **App formats**: only the WASM OCI artifact. Podman "compat" images (with the `.wasm` in a tar.gz layer) are not supported.
- **Debug build** (`-Og`). With `-Os` the images would be smaller.
- **Hardware coverage**: the ESP32-S2 was not tested, and the C6 was only built.

## Suggested next steps

1. Open an upstream discussion on Flight Control limitations 1–4, the ones that block adoption for MCUs.
2. Security:
   - image signing with verification on the device;
   - Secure Boot v2 and flash encryption;
   - the device key in the HMAC/DS peripheral, where the chip supports it.
3. WiFi and enrollment provisioning, in the factory or in the field.
4. A Zephyr port of the `port/` layer: the core is POSIX C and WAMR supports Zephyr.
5. Support for battery-powered devices: optional long-poll and status only on wake-up. This depends on limitation 3.
