# Roadmap

flightctl-mcu lets microcontrollers be managed by an unmodified [Flight Control](https://github.com/flightctl/flightctl): enrollment, monitoring, configuration, firmware OTA and WebAssembly apps, in the same fleets as Linux devices.

Four milestones are done: it works on real hardware, it can be left running, it can be deployed in the field, and it is not tied to one chip vendor. What is left is a list, [below](#to-do), not a milestone.

## Principles

- **No server changes.** Everything goes through the public agent API. Gaps in Flight Control are raised upstream, not patched locally.
- **Portable core.** `core/` is POSIX C with mbedTLS, cJSON and WAMR. Chip and RTOS specifics live behind `port/`.
- **Verifiable without hardware.** Every feature also runs on the simulated device, so CI can check it against a real Flight Control on kind. Hardware tests add to that; they don't replace it.

## To do

Checks that were never made:

- [ ] ESP-IDF 6.0 on the other five boards (C3, C5, C6, S2, classic ESP32). On hardware only the S3 ran it; the others ran ESP-IDF 5.5, and 6.0 in the emulator (C3, C5).
- [ ] The way back from a firmware that does not work, on the ESP32-S3 (both ports) and on the C6. It is tested in the emulator and, for Zephyr, on the Pico W.
- [ ] A soak test: the lab fleet left running for weeks. The collector is ready (`deploy/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); there has been no fleet to watch for weeks.
- [ ] The Zephyr builds for boards in CI. They need the ARM and Xtensa toolchains; CI builds and tests `native_sim` only.
- [ ] A wired board. Nothing in the core is about WiFi, and Zephyr on `native_sim` already runs without it, but WiFi is the only link tried on hardware: a Zephyr board with Ethernet is two files ([DEVICES.md](DEVICES.md#another-zephyr-board)).

What the Zephyr port does not have, and the ESP-IDF firmware has:

- [ ] Protection of the device: MCUboot checks the firmware's signature in software, and the device key is in the storage partition in clear. No Secure Boot, no encryption.
- [ ] The WiFi network given in the field: a Zephyr build gets it at the factory, with the credentials.

What Flight Control offers and the agent does not answer yet:

- [ ] The remote terminal: `flightctl console device/<name>` and the Terminal tab of the UI. Flight Control puts the session in the rendered spec (`consoles`), and the agent is to open a gRPC stream for it on the endpoint it already uses. That takes an HTTP/2 and gRPC client in the core, which speaks HTTP/1.1 only; a closed set of commands in place of a shell, which a microcontroller does not have (status, heap, apps, log, reboot); and a third TLS connection for as long as the session lasts, which the boards shortest of RAM may not have room for.
- [ ] The device's log from the UI. Flight Control has no API for logs: the Log tab runs `journalctl` through the terminal's channel. So it comes after the terminal: a log kept in RAM (today it goes to the serial port only), shown by a command, and then an answer to the commands the UI sends, which holds only as long as the UI sends those.

Towards a deployment:

- [ ] A registry with TLS and authentication. Today it is plain HTTP: signatures protect the images from tampering, not from being read.
- [ ] The counters out of `customInfo` (free heap, traffic, uptime), or fewer of them: since Flight Control 1.4 each change there logs an event (item 8 below).
- [ ] The outcome of a delta in `status.os.lastDelta`, which Flight Control 1.4 has, instead of `customInfo`.

Waiting for Flight Control (the items are in the [upstream track](#upstream-track-flight-control)):

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll. Needs item 3.
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device. Needs the server to accept a re-enrollment for a known name.
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one (RSA-3072, sealed under a key in eFuse). Written and switched off: needs item 7.

Not planned here: burning eFuses on a real board. Secure Boot, flash encryption and what goes with them are verified on emulated chips, which run the chip's real ROM; whoever ships a batch burns one board per chip first ([SECURITY.md](SECURITY.md)).

## Done

Each milestone has a tag; what was measured and what went wrong is in [RESULTS.md](RESULTS.md).

**Milestone 0: proof of concept** (5–6 November 2026). Six Espressif chips (ESP32, S2, S3, C3, C5, C6) in one fleet under Flight Control 1.3.1: enrollment with a key made on the device and mutual TLS 1.3, full status, inline config applied without a reboot, firmware from one multi-chip OCI index into A/B slots with a canary rollout and automatic rollback, WASM apps updated without a reboot, and CI with an end-to-end test against Flight Control on kind.

**Milestone 1: production foundations** (tag `m1`). What would stop anyone from leaving a device running: renewal of the management certificate; TLS connections kept open (220 KB per hour idle, from 560); firmware and apps signed in cosign's format; fuzzing of what reads the network, and tests with the registry down and the server restarting; enrollment approved from an allow list; an SDK for WASM apps. On all six boards, and on emulated C3, C5 and S3.

**Milestone 2: field deployment** (tag `m2`), on emulated chips. Credentials written over USB, the same firmware everywhere, one enrollment certificate per batch; the WiFi network given in the field over SoftAP or BLE ([PROVISIONING.md](PROVISIONING.md)); a device whose certificate expired enrolls again; Secure Boot v2 with key rotation and revocation, flash and NVS encryption, anti-rollback, and on the ESP32-C5 the device key in the ECDSA peripheral ([SECURITY.md](SECURITY.md)).

**Milestone 3: beyond Espressif** (tag `m3`, November 2026).

- A second port, Zephyr, with the core unchanged ([ZEPHYR.md](ZEPHYR.md)). On `native_sim` it passes the simulated device's end-to-end test in CI. On a Raspberry Pi Pico W (RP2040, 264 KB of RAM) it enrolls over WiFi, runs a signed WASM app and updates its firmware through MCUboot: whole, as a delta, and back from an image that does not reach Flight Control. On an ESP32-S3 it does the same bar the way back: one chip, both ports.
- Firmware updates as deltas, in the core, so every port has them: a device on a known release downloads 1 to 12% of the image and rebuilds the rest from its own, checked against the signed digest ([DELTA.md](DELTA.md)). On the Pico W and on the ESP32-S3 with both ports; the other ESP32 chips in the emulator.
- Current foundations: the core on mbedTLS 4, whose cryptography is PSA only, with ESP-IDF 6.0 and Zephyr 4.5, and the tests against Flight Control 1.4.0, whose agent API is the same as 1.3.1's ([DEPENDENCIES.md](DEPENDENCIES.md)). ESP-IDF 6.0 ran on one board, the S3.

## Upstream track (Flight Control)

Raised as proposals, in parallel with the milestones. Details in [RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals).

1. `matchExpressions` in fleet selectors is accepted but ignored.
2. Rollout success is judged on `renderedVersion` only: write the agent's obligation into the API contract.
3. Fixed 5-minute disconnect timeout: make it configurable, or let the device declare its contact interval.
4. No way to declare a device class or supported app runtimes (`status.capabilities`). Since 1.4 a fleet can at least select on the class a device reports (`custominfo.flightctl.io/deviceClass`), not yet refuse an app it cannot run.
5. No free-form status per app, full status on every update (most of the remaining idle traffic), `os.image` compared as a string.
6. Enrollment certificates cannot be revoked: the server checks only their signer and expiry.
7. Devices with RSA keys cannot enroll: the internal CA copies the request's signature algorithm, which fails with its ECDSA key.
8. Since 1.4, `customInfo` is copied into the device's labels by default, and any change in it logs an event: a device that reports counters there writes one at every status.

Items 1, 3, 6 and 7 are as they were in the source of 1.4.0.

## Open questions

- Where the agent lives long term: in the Flight Control organization, as an ESP-IDF component in the Espressif registry, or both.
- Support policy: which Flight Control and ESP-IDF versions are supported, and for how long.
- Compliance work for products that ship it (for example the EU Cyber Resilience Act, ETSI EN 303 645).

## Reference: agent API as used

Checked against Flight Control 1.3.1 and 1.4.0:

- `POST/GET enrollmentrequests`, `POST/GET certificatesigningrequests`;
- `GET devices/{name}/rendered?knownRenderedVersion=`, a long-poll of about 4 minutes that returns `204` when nothing changed;
- `PUT/PATCH devices/{name}/status`.
- mTLS with TLS 1.3 and ECDSA P-256. The device name is derived from the public key: `lower(base32hex_nopad(sha256(X || Y)))`.
- A device is up to date when `status.config.renderedVersion` equals the rendered version served; the OS is out of date when `status.os.image` differs from `spec.os.image` as a string.
- A device becomes `Unknown` after 5 minutes without contact.
- `consoles` in the rendered spec (someone asked for a terminal on the device) is ignored: see the list above.
