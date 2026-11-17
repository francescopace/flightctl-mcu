# Roadmap

flightctl-mcu lets microcontrollers be managed by an unmodified [Flight Control](https://github.com/flightctl/flightctl): enrollment, monitoring, configuration, firmware OTA and WebAssembly apps, in the same fleets as Linux devices.

What it does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md). This page is what is left.

## Principles

- **No server changes.** Everything goes through the public agent API. Gaps in Flight Control are raised upstream, not patched locally.
- **Portable core.** `core/` is POSIX C with mbedTLS, cJSON and WAMR. Chip and RTOS specifics live behind `port/`.
- **Verifiable without hardware.** Every feature also runs on the simulated device, so CI can check it against a real Flight Control on kind. Hardware tests add to that; they don't replace it.

## Next

Chips ([support levels](DEVICES.md#support-levels)):

- [ ] The Zephyr port on a real Arm board with Ethernet, an ST Nucleo (H563ZI or F767ZI). Arm is tried only in QEMU so far ([ZEPHYR.md](ZEPHYR.md#on-an-emulated-arm-cortex-m3)).

Checks not made yet:

- [ ] The rest of HTTP/2 on the boards ([REGISTRY.md](REGISTRY.md)):
  - a certificate renewal over it;
  - Zephyr's downloads from the registry over HTTP/2;
  - the traffic it saves or costs ([RESULTS.md](RESULTS.md#idle-traffic) has HTTP/1.1's only).
- [ ] The core's own WiFi provisioning on an ESP-IDF board: it passes in the emulator only ([PROVISIONING.md](PROVISIONING.md)).
- [ ] `lastFatalError` after a real crash on the ESP32-S3 with Zephyr: the code is on the board but has never run on Xtensa ([RESULTS.md](RESULTS.md#a-crash-in-the-status)). That build has 4 KB of DRAM left at link time, so a larger static buffer must take its room from something else.

What an app can do and hold:

- [ ] A large app's memory on the classic ESP32, the C5 and the C3, every time: CoreMark takes about 105 KB of heap in all, and with the agent connected these chips have 103 to 117 KB free with no app, 20 to 34 KB short of it and the 32 KB kept for the agent, a few KB less with the agent's connection closed, as when an app starts ([RESULTS.md](RESULTS.md#coremark)). Left: that much less heap for the agent at rest, or a lower floor. Done on main, not yet on the boards: an app starts once the spec is freed, and one that did not fit is started again when the heap has room.
- [ ] What the agent takes of the heap, measured, so that the 32 KB it keeps from the apps is a figure and not a guess ([APPS.md](APPS.md#size-and-limits)). The lowest the device logs today also counts the block held while a TLS connection opens. With Zephyr the 32 KB are not checked yet.
- [ ] CoreMark slower and less even on the C5 and C6 since v3.12 than with the same `-Os` firmware before it. Not explained yet ([RESULTS.md](RESULTS.md#coremark)).
- [ ] CoreMark with Zephyr on an ESP32-S3 without PSRAM: it runs only on the N8R8, from its PSRAM ([RESULTS.md](RESULTS.md#coremark)).
- [ ] Apps against [Toit](https://toit.io) and against WAMR's AOT mode, on the same ESP32-C6 ([COMPARISON.md](COMPARISON.md#tied-to-one-chip-vendor)). Toit publishes no figures against native code or WebAssembly. Three benchmarks:
  - Richards (method calls and pointers between objects), in Toit and in C built to `.wasm`;
  - DeltaBlue (objects allocated and freed at every step), which also helps decide whether apps get `malloc`;
  - CoreMark in AOT, against the interpreter. AOT needs `wamrc`, a module per architecture, executable memory, and another way to stop an app than the instruction count.
- [ ] Two well-known apps as demos, from their published `.wasm` where the firmware can:
  - the blink of wasm3-arduino, [as published](APPS.md#the-blink-of-wasm3-arduino), on a board (tested on the host and on `native_sim`);
  - the TinyML model from emlearn, [`tinyml`](APPS.md#tinyml-with-emlearn), on a board (tested on the host). A neural network also needs `expf`, `tanhf` and `logf`.
- [ ] The app interface for 1.0 ([APPS.md](APPS.md)):
  - GPIO interrupts, and I2C, SPI, UART, ADC and PWM with devices wired to them, on a board; on Zephyr also a board that names its hardware in the devicetree, and RS-485;
  - the 1 s limit of a call in the device's functions, on a board (unit tests only);
  - the app's stack with the C library, measured on Xtensa;
  - whether apps get `malloc` and the math functions.
- [ ] MQTT for the apps ([APPS.md](APPS.md#mqtt)):
  - on the C3 with an app using MQTT, the first metrics exports after a restart fail for want of memory, the heap down to 6 to 10 KB ([RESULTS.md](RESULTS.md#ram));
  - on the boards: the connection kept through a download, and MQTT with Zephyr (built for the ESP32-S3, not run);
  - not planned for 1.0: the device's certificate for the broker, a QoS 1 message sent again after a lost link, QoS 2, a will message, a persistent session.

Waiting for Flight Control (the numbers are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals)):

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll. Needs limitation 3 lifted.
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device. Needs limitation 13 lifted.
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one (RSA-3072, sealed under a key in eFuse). Written and switched off: needs limitation 9 lifted.
- [ ] The firmware from the software catalog, as apps already can. Needs limitation 11 lifted.
- [ ] Revoking a batch's enrollment certificate from `tools/enrollment.sh`. Flight Control's main branch has the API (limitation 8); it waits for a release.

Not planned here: burning eFuses on a real board. Secure Boot, flash encryption and what goes with them are verified on emulated chips, which run the chip's real ROM; whoever ships a batch burns one board per chip first ([SECURITY.md](SECURITY.md)).

## After 1.0

Proposed for 2.0, from what similar products offer ([COMPARISON.md](COMPARISON.md)):

- [ ] Crash reports beyond `lastFatalError`: the full coredump sent off the device, to be decoded against the release's ELF (which `tools/fw-push.py` already keeps in the registry). Flight Control has no place for a file from a device; the coredump could go to the registry as an artifact, with write credentials of its own.
- [ ] Cellular (LTE-M, NB-IoT): a modem as the link, through ESP-IDF's PPP or Zephyr's cellular drivers. A long-poll held open over cellular costs data and power, so it comes with the battery-powered devices above.
- [ ] Devices without IP (BLE, Thread) through a Linux gateway in the same fleet: the gateway's Flight Control agent, or an app on it, holds the device's identity and passes its spec, status and firmware.
- [ ] More silicon: Nordic nRF (nRF7002 WiFi, nRF91 cellular), RP2350, STM32U5, the last two with TF-M to keep the device key protected.
- [ ] On Zephyr, the device key protected and the flash encrypted: today only images are signed and rollback refused. It comes with the TF-M chips above; on an ESP32-S3 it would take Espressif's own MCUboot ([what it takes](SECURITY.md#zephyr)).

Nice to have, last:

- [ ] A soak test: a fleet of boards left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.

## Upstream (Flight Control)

The gaps found in Flight Control for this class of device, each with a proposal, are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals).

To propose:

- Firmware and apps built by Flight Control. Its image builder (`ImageBuild`, `flightctl new-version`) makes bootc images for Linux; a build of another kind (a toolchain image, sources from Git, a list of chips) could make the signed multi-chip index that `tools/fw-push.py` makes today, with the delta from the previous release and an SBOM ([SBOM.md](SBOM.md)). It needs an agreed artifact type for firmware first; to be raised as an issue before any code.

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
- `consoles` in the rendered spec (someone asked for a terminal on the device, or the UI's Logs tab): the device opens `flightctl.v1.RouterService/Stream` on the agent's connection ([CONSOLE.md](CONSOLE.md)).
- The Telemetry Gateway: a certificate request signed by `flightctl.io/device-svc-client`, CN `otel-<device name>`, approved at once; then OTLP over gRPC (`ExportMetricsServiceRequest`) with that certificate ([METRICS.md](METRICS.md)).
