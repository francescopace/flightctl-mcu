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

What the Zephyr port does not have, and the ESP-IDF firmware has:

- [ ] Protection of the device: MCUboot checks the firmware's signature in software, and the device key is in the storage partition in clear. No Secure Boot, no encryption.
- [ ] The WiFi network given in the field: a Zephyr build gets it at the factory, with the credentials.

What Flight Control offers and the agent does not answer yet:

- [ ] The remote terminal: `flightctl console device/<name>` and the Terminal tab of the UI. Flight Control puts the session in the rendered spec (`consoles`), and the agent is to open a gRPC stream for it on the endpoint it already uses. That takes an HTTP/2 and gRPC client in the core, which speaks HTTP/1.1 only; a closed set of commands in place of a shell, which a microcontroller does not have (status, heap, apps, log, reboot); and a third TLS connection for as long as the session lasts, which the boards shortest of RAM may not have room for.
- [ ] The device's log from the UI. Flight Control has no API for logs: the Log tab runs `journalctl` through the terminal's channel. So it comes after the terminal: a log kept in RAM (today it goes to the serial port only), shown by a command, and then an answer to the commands the UI sends, which holds only as long as the UI sends those.

Towards a deployment:

- [ ] Known vulnerabilities in the firmware, in the UI's security overview. Flight Control asks Trustify (or Quay with Clair) about the OS image digest each device reports, which these devices already report. It takes an SBOM per release (ESP-IDF or Zephyr, mbedTLS, WAMR, detools, the agent), uploaded to Trustify. It is also part of what the Cyber Resilience Act asks for ([open questions](#open-questions)).

Waiting for Flight Control (the numbers are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals)):

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll. Needs limitation 3 lifted.
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device. Needs limitation 13 lifted.
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one (RSA-3072, sealed under a key in eFuse). Written and switched off: needs limitation 9 lifted.
- [ ] The firmware from the software catalog, as apps already can. Needs limitation 11 lifted.
- [ ] Revoking a batch's enrollment certificate from `tools/enrollment.sh`. Flight Control's main branch has the API (limitation 8); it waits for a release.

Not planned here: burning eFuses on a real board. Secure Boot, flash encryption and what goes with them are verified on emulated chips, which run the chip's real ROM; whoever ships a batch burns one board per chip first ([SECURITY.md](SECURITY.md)).

## Upstream (Flight Control)

The gaps found in Flight Control for this class of device, each with a proposal, are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals). They were checked against 1.4.0 and against Flight Control's main branch of 12 November 2026: all hold there, except the revocation of enrollment certificates (limitation 8), fixed on main and not yet released.

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
