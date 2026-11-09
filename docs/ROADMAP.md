# Roadmap

flightctl-mcu lets microcontrollers be managed by an unmodified [Flight Control](https://github.com/flightctl/flightctl): enrollment, monitoring, configuration, firmware OTA and WebAssembly apps, in the same fleets as Linux devices.

The first milestone proved that this works on real hardware; the second closed the gaps that made it unsafe or costly to leave running. The next ones make it deployable in the field and not tied to one chip vendor.

## Principles

- **No server changes.** Everything goes through the public agent API. Gaps in Flight Control are raised upstream, not patched locally.
- **Portable core.** `core/` is POSIX C with mbedTLS, cJSON and WAMR. Chip and RTOS specifics live behind `port/`.
- **Verifiable without hardware.** Every feature also runs on the simulated device, so CI can check it against a real Flight Control on kind. Hardware tests add to that; they don't replace it.

## Milestone 0: proof of concept (done, November 2026)

Six Espressif chips (ESP32, S2, S3, C3, C5, C6) in one fleet, managed by Flight Control 1.3.1:

- enrollment with a key generated on the device, mTLS with TLS 1.3;
- full device status, including heap, RSSI, temperature and traffic;
- inline config applied in 1–6 s without a reboot, validated by the application;
- firmware OTA from one multi-chip OCI index, A/B slots, staged rollout with a canary and automatic rollback;
- WebAssembly apps (WAMR), updated without a reboot, same `.wasm` on Xtensa and RISC-V;
- CI: host agent, firmware for every chip, WASM apps, and an end-to-end test against Flight Control on kind.

Measurements, problems found and Flight Control limitations are in [RESULTS.md](RESULTS.md).

## Milestone 1: production foundations (done, November 2026)

The gaps that would stop anyone from leaving a device in the field. None of them needed new hardware or server changes, and each one is tested in CI. They ran on the simulated device, on all six boards (firmware v3.0, see [RESULTS.md](RESULTS.md#firmware-v30-on-all-six-boards)), and in Espressif's emulator for the C3, C5 and S3, where CI also runs the real firmware.

| # | Item | Why | Result |
|---|---|---|---|
| 1.1 | **Management certificate renewal** | The certificate issued at enrollment expires, and the device was lost when it did. | The device asks for a new certificate for the same key (signer `flightctl.io/device-management-renewal`, approved by the server on its own) once a quarter of the lifetime has passed, as the Linux agent does. The end-to-end test runs the server with 2-minute certificates and checks that the device keeps reporting after the first one expires. |
| 1.2 | **Persistent TLS connections** | One TLS handshake per request cost about 560 KB/hour per idle device, mostly handshakes. | HTTP keep-alive, one connection per agent thread, reopened when the server drops it. Idle traffic is about 220 KB/hour, with no handshakes in 10 minutes. The rest is the full status every minute: going lower needs differential status updates (upstream item 5). TLS session resumption was tried and dropped: Flight Control 1.3.1 issues session tickets but never resumed a session with them, so reconnections stay full handshakes (about 1.5 KB). |
| 1.3 | **Signed firmware and apps** | Anyone who could write to the registry could run code on every device. | Images are signed when pushed, in cosign's format, with an ECDSA P-256 key. A device with the public key in its firmware refuses unsigned images and images signed with another key. CI checks both, and that `cosign verify` accepts our signatures. |
| 1.4 | **Hardening and fault tests** | Parsers face data from the network, and the agent must survive a flaky server or registry. | libFuzzer targets for the HTTP reader, the OCI and signature handling, and the agent's handling of specs and certificates, two minutes each in CI. The first runs found an integer overflow in chunked-body parsing and an undefined conversion of a layer size from a manifest, both fixed and kept as seeds. A release profile (`-Os`) is about 15% smaller. The end-to-end test covers an update while the registry is down and a server restart. |
| 1.5 | **Automatic enrollment approval** | Approving each device by hand does not scale past a lab. | `deploy/auto-approve/auto-approve.py` approves requests from an allow list (device name or MAC) with their labels, through the Flight Control CLI. The end-to-end test enrolls its device this way. |
| 1.6 | **WASM app SDK** | Writing an app meant reading the agent source. | `wasm-apps/sdk/fc_app.h` documents the interface, with a version the device checks. A second example app (`trend`) runs in the end-to-end test. |

## Milestone 2: field deployment

Needs hardware in the loop, and some steps are irreversible on a board (eFuses).

- **Provisioning**: done over USB: credentials, endpoint, signing key and WiFi live in an `fc_creds` partition (`deploy/provision.py`), and the firmware has none built in. One enrollment certificate per batch, with its own lifetime: `BATCH=… EXPIRATION=… deploy/enrollment.sh` (the end-to-end test uses one, not yet run). It cannot be revoked: Flight Control checks only its signer and expiry (upstream item 6). WiFi provisioning in the field: done over SoftAP ([PROVISIONING.md](PROVISIONING.md)); a device provisioned with a proof of possession and no network opens an access point for the installer (Espressif's app or `esp_prov`). The plain emulator run in CI provisions its device that way. Still to do: BLE.
- **Expired certificates**: done. A device offline for longer than its certificate enrolls again as a new device (see problem 13 in RESULTS.md). Still to do: keep the device name across re-enrollment, which needs Flight Control's help.
- **Secure Boot v2, flash encryption and NVS encryption**: done in the emulator. The `secure` firmware variant, the factory procedure and what is left are in [SECURITY.md](SECURITY.md); CI runs the end-to-end test on emulated C3 and C5 with it. Anti-rollback (security version in eFuse) and Secure Boot key rotation (bootloader signed with spare keys, the old key revoked by the first release signed with the next) work in the emulator; their end-to-end steps against Flight Control have not run yet. Still to do: burn one real board per chip.
- **Device key in hardware**: done in the emulator. In the secure variant the device key is RSA-3072, sealed in the DS peripheral on every chip that has one (S2, S3, C3, C5, C6) under an HMAC key in eFuse; the ESP32 keeps it in the encrypted store. Generation, sealing and signing work in the emulator; enrollment and TLS with it against Flight Control have not run yet.
- **Battery-powered devices**: deep sleep, status only on wake-up, no long-poll. Depends on a configurable disconnect timeout upstream.
- **Soak tests**: the lab fleet running for weeks, metrics collected automatically. The collector is ready: `deploy/soak/collect.py` samples every device's status into a CSV and reports, per device, time not online, restarts and their causes, the heap's low-water mark over time and traffic per hour. Still to do: run it on the lab fleet (the boards need the new flash layout first).
- **Hardware-in-the-loop CI**: flash and test real boards on every release.

## Milestone 3: beyond Espressif

- **Zephyr port** of `port/`, starting from `native_sim`, then a Nordic board. WAMR and mbedTLS both run on Zephyr.
- **Other links**: Ethernet, cellular (NB-IoT, LTE-M).
- **Delta firmware updates**, to cut download size.

## Upstream track (Flight Control)

Raised as proposals, in parallel with the milestones. Details in [RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals).

1. `matchExpressions` in fleet selectors is accepted but ignored.
2. Rollout success is judged on `renderedVersion` only: write the agent's obligation into the API contract.
3. Fixed 5-minute disconnect timeout: make it configurable, or let the device declare its contact interval.
4. No way to declare a device class or supported app runtimes (`status.capabilities`).
5. No free-form status per app, full status on every update (most of the remaining idle traffic), `os.image` compared as a string.
6. Enrollment certificates cannot be revoked: the server checks only their signer and expiry.

## Open questions

- Where the agent lives long term: in the Flight Control organization, as an ESP-IDF component in the Espressif registry, or both.
- Support policy: which Flight Control and ESP-IDF versions are supported, and for how long.
- Compliance work for products that ship it (for example the EU Cyber Resilience Act, ETSI EN 303 645).

## Reference: agent API as used

Checked against Flight Control 1.3.1:

- `POST/GET enrollmentrequests`, `POST/GET certificatesigningrequests`;
- `GET devices/{name}/rendered?knownRenderedVersion=`, a long-poll of about 4 minutes that returns `204` when nothing changed;
- `PUT/PATCH devices/{name}/status`.
- mTLS with TLS 1.3 and ECDSA P-256. The device name is derived from the public key: `lower(base32hex_nopad(sha256(X || Y)))`.
- A device is up to date when `status.config.renderedVersion` equals the rendered version served; the OS is out of date when `status.os.image` differs from `spec.os.image` as a string.
- A device becomes `Unknown` after 5 minutes without contact.
