# Roadmap

flightctl-mcu lets microcontrollers be managed by an unmodified [Flight Control](https://github.com/flightctl/flightctl): enrollment, monitoring, configuration, firmware OTA and WebAssembly apps, in the same fleets as Linux devices.

What it does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md). This page is what is left.

## Principles

- **No server changes.** Everything goes through the public agent API. Gaps in Flight Control are raised upstream, not patched locally.
- **Portable core.** `core/` is POSIX C with mbedTLS, cJSON and WAMR. Chip and RTOS specifics live behind `port/`.
- **Verifiable without hardware.** Every feature also runs on the simulated device, so CI can check it against a real Flight Control on kind. Hardware tests add to that; they don't replace it.

## Next

Checks not made yet:

- [ ] ESP-IDF 6.0 on the other four boards (C5, C6, S2, classic ESP32). On hardware the S3 and the C3 ran it; the others ran ESP-IDF 5.5, and 6.0 in the emulator (C5).
- [ ] The way back from a firmware that does not work, on the C6. It is tested in the emulator, on the ESP32-S3 with both ports and, for Zephyr, on the Pico W.
- [ ] A soak test: the lab fleet left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); there has been no fleet to watch for weeks.
- [ ] Two parts of decommissioning and the registry never tried: the `FactoryReset` target (the CLI and the UI send `Unenroll` only), and Bearer tokens against a real registry (Harbor, Quay); `registry:3` asks for Basic.
- [ ] The Zephyr builds for boards in CI. They need the ARM and Xtensa toolchains; CI builds and tests `native_sim` only.
- [ ] Zephyr: the report of the last fatal error is printed at boot, before the Pico W's USB console is open, and is lost. Report it in the device's status too.
- [ ] The Pico W's WiFi (CYW43439) sometimes cannot join again after the board restarts (`whd_wifi_prepare_join failed`, every 10 s) until it restarts once more.

What the Zephyr port does not have, and the ESP-IDF firmware has:

- [ ] Protection of the device beyond signed images and anti-rollback: the device key is in the storage partition in clear, the flash is not encrypted. The RP2040 has nothing to do it with; on the ESP32-S3 it takes Espressif's MCUboot port; a chip with TF-M (RP2350, nRF54L, STM32U5) would keep the key protected ([SECURITY.md](SECURITY.md#zephyr)).
- [ ] The WiFi network given in the field on the Pico W. The ESP32-S3 has it ([PROVISIONING.md](PROVISIONING.md#on-zephyr)); Infineon's driver in Zephyr 4.5 cannot open an access point on the Pico W's chip ([ZEPHYR.md](ZEPHYR.md#on-a-raspberry-pi-pico-w)).

What Flight Control offers and the agent does not answer yet:

- [ ] The remote terminal: `flightctl console device/<name>` and the Terminal tab of the UI. Flight Control puts the session in the rendered spec (`consoles`), and the agent is to open a gRPC stream for it on the endpoint it already uses. That takes an HTTP/2 and gRPC client in the core, which speaks HTTP/1.1 only; a closed set of commands in place of a shell, which a microcontroller does not have (status, heap, apps, log, reboot); and a third TLS connection for as long as the session lasts, which the boards shortest of RAM may not have room for.
- [ ] The device's log from the UI. Flight Control has no API for logs: the Log tab runs `journalctl` through the terminal's channel. So it comes after the terminal: a log kept in RAM (today it goes to the serial port only), shown by a command, and then an answer to the commands the UI sends, which holds only as long as the UI sends those.

What an app can do and hold:

- [ ] The app interface for 1.0 ([APPS.md](APPS.md)). An app has its state, log, config, a key/value store, timers and events, and GPIO with interrupts and LEDs under the names of a board map that the fleet extends; a call that does not return is stopped. To come:
  - I2C, SPI, UART (with RS-485), ADC and PWM, on ESP-IDF, then on Zephyr through the devicetree; GPIO on Zephyr;
  - MQTT: one connection for all the apps. With the agent's two, a C3 at its lowest has room for one more TLS handshake at a time; the ESP32-S2 may not fit it;
  - metrics for Flight Control's Telemetry Gateway, which takes OTLP over gRPC only: an HTTP/2 and gRPC client, the one the remote terminal needs, and a protobuf encoder;
  - the instruction budget of a call, measured on a C3 so that it comes to about a second there;
  - whether apps get WAMR's small libc (`memcpy`, `snprintf`...), a few KB of firmware, since building a payload by hand is the biggest burden on a real app;
  - for MQTT payloads, a pool shared by the apps' event queues, which could otherwise hold 8 KB per app.
- [ ] A size limit for an app that the chip can hold. The device accepts a `.wasm` up to 64 KB, which no chip can run: in RAM an app costs about 3 times its `.wasm` once loaded, plus its linear memory, about 3.5 KB for the instance and 8 KB for its thread ([RESULTS.md](RESULTS.md#ram)). The limit should follow the heap, and the thread stack, the largest part of what a small app costs, can shrink.

Towards a deployment:

- [ ] Known vulnerabilities in the firmware, in the UI's security overview. Each release has its SBOM, in the registry and, with `TRUSTIFY_URL`, in Trustify ([SBOM.md](SBOM.md)); Flight Control finds it by the image digest the devices report. Tried with Trustify 0.4.12: its own feeds reach no C component, so `tools/advisories.py` brings NVD's CVEs for them; Flight Control shows them on the devices. CVEs that do not apply to what a release builds are listed with their reason (`tools/not-affected.json`) and kept out; Flight Control has no field for it. It is also part of what the Cyber Resilience Act asks for ([open questions](#open-questions)).

Waiting for Flight Control (the numbers are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals)):

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll. Needs limitation 3 lifted.
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device. Needs limitation 13 lifted.
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one (RSA-3072, sealed under a key in eFuse). Written and switched off: needs limitation 9 lifted.
- [ ] The firmware from the software catalog, as apps already can. Needs limitation 11 lifted.
- [ ] Revoking a batch's enrollment certificate from `tools/enrollment.sh`. Flight Control's main branch has the API (limitation 8); it waits for a release.

Not planned here: burning eFuses on a real board. Secure Boot, flash encryption and what goes with them are verified on emulated chips, which run the chip's real ROM; whoever ships a batch burns one board per chip first ([SECURITY.md](SECURITY.md)).

## Upstream (Flight Control)

The gaps found in Flight Control for this class of device, each with a proposal, are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals). They were checked against 1.4.0 and against Flight Control's main branch of 12 November 2026: all hold there, except the revocation of enrollment certificates (limitation 8), fixed on main and not yet released.

To propose:

- Firmware and apps built by Flight Control. Its image builder (`ImageBuild`, `flightctl new-version`) builds bootc images for Linux devices: a base image with the agent added, out as a container, qcow2 or ISO. A build of another kind would cover these devices: a toolchain image (ESP-IDF, Zephyr's SDK, clang for WASM), sources from Git and a list of chips, out as the signed multi-chip OCI index `tools/fw-push.py` makes, or a WASM artifact. `new-version` from a previous build is where the delta from that release would be made, and the build is where an SBOM per release would come from (see the vulnerabilities above). It needs an agreed artifact type for firmware first; to be raised as an issue before any code, after reading the image builder's source.

## Open questions

- Where the agent lives long term: in the Flight Control organization, as an ESP-IDF component in the Espressif registry, or both.
- Support policy: which Flight Control and ESP-IDF versions are supported, and for how long.
- Compliance work for products that ship it (for example the EU Cyber Resilience Act, ETSI EN 303 645).

## Reference: agent API as used

Checked against Flight Control 1.4.0:

- `POST/GET enrollmentrequests`, `POST/GET certificatesigningrequests`;
- `GET devices/{name}/rendered?knownRenderedVersion=`, a long-poll of about 4 minutes that returns `204` when nothing changed;
- `PUT devices/{name}/status` (the API also has `PATCH`, which the agent does not use).
- mTLS with TLS 1.3 and ECDSA P-256. The device name is derived from the public key: `lower(base32hex_nopad(sha256(X || Y)))`.
- A device is up to date when `status.config.renderedVersion` equals the rendered version served; the OS is out of date when `status.os.image` differs from `spec.os.image` as a string.
- A device becomes `Unknown` after 5 minutes without contact.
- `consoles` in the rendered spec (someone asked for a terminal on the device) is ignored: see [Next](#next).
