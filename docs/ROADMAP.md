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

- [ ] A large app's memory on the classic ESP32, the C5 and the C3, every time. CoreMark's 64 KB takes one free block of 68 KB, and in v3.12 one install or swap in seven still failed on each for want of it (largest free block 48 and 64 KB, [RESULTS.md](RESULTS.md#coremark)); after an hour of app changes the C5's largest block was 60 KB with the `alarm` app. To find: what stays in the middle of the heap from one app to the next, and whether an app that failed for want of memory is tried again on its own. With v3.13 it fitted once in two tries on the ESP32 and the C5 and once in five on the C3 (only right after a restart; a swap found 22 KB the largest of 66 KB free), with the metrics on or off. With v3.14-rc4 it started on all three after a restart, and right after a swap from another app it found no such block (largest 28 to 44 KB).
- [ ] A floor of heap for the agent. While CoreMark ran, the heap went down to 1.8 KB on the C6, 2.3 KB on the classic ESP32 and 108 bytes on the C3, where one of the agent's TLS handshakes failed for want of memory while CoreMark's memory was taken. An app whose memory leaves the agent less than 32 KB of internal heap, once its instance is made, is now refused ([APPS.md](APPS.md#size-and-limits)); passes the unit tests. With v3.14-rc4 CoreMark (64 KB of memory) runs on all five chips with the floor: its instance left 85 to 96 KB of internal heap, 52 KB on the S3, and no handshake failed. Left: what the agent takes, measured. The device logs the heap left after an app's instance and the lowest while the agent went on, but that lowest counts the block the agent holds while a TLS connection opens; only on the S3, where that block is in PSRAM, does it read clean: 18 KB. With Zephyr the device does not check yet.
- [ ] CoreMark slower and less even on the C5 and C6 since v3.12 than with the same `-Os` firmware before it. Not explained yet ([RESULTS.md](RESULTS.md#coremark)).
- [ ] CoreMark with Zephyr on an ESP32-S3 without PSRAM: it runs only on the N8R8, from its PSRAM ([RESULTS.md](RESULTS.md#coremark)).
- [ ] Apps against [Toit](https://toit.io) and against WAMR's AOT mode, on the same ESP32-C6 ([COMPARISON.md](COMPARISON.md#tied-to-one-chip-vendor)). Toit publishes no figures against native code or WebAssembly. Three benchmarks:
  - Richards (method calls and pointers between objects), in Toit and in C built to `.wasm`;
  - DeltaBlue (objects allocated and freed at every step), which also helps decide whether apps get `malloc`;
  - CoreMark in AOT, against the interpreter. AOT needs `wamrc`, a module per architecture, executable memory, and another way to stop an app than the instruction count.
- [ ] Two well-known apps as demos, from their published `.wasm` where the firmware can:
  - the blink of [wasm3-arduino](https://github.com/wasm3/wasm3-arduino/tree/main/wasm_apps): an `arduino` import module (`pinMode`, `digitalWrite`, `delay`...) mapped to board-map names, and a `delay` longer than one wait may last (100 ms);
  - a TinyML model from [emlearn](https://emlearn.readthedocs.io): a random forest (2.5 KB) already builds with `fc_app.h` and runs as it is; an MLP needs `expf`, `tanhf` and `logf`.
- [ ] The app interface for 1.0 ([APPS.md](APPS.md)):
  - GPIO interrupts, and I2C, SPI, UART, ADC and PWM with devices wired to them, on a board; on Zephyr also a board that names its hardware in the devicetree, and RS-485;
  - the 1 s limit of a call in the device's functions, on a board (unit tests only);
  - the C library on a board, with the app's stack measured on Xtensa;
  - whether apps get `malloc` and the math functions.
- [ ] MQTT for the apps ([APPS.md](APPS.md#mqtt)), granted by `"mqtt"`: `fc_mqtt_publish`, `fc_mqtt_subscribe`, `FC_EV_MQTT` with `fc_event_data` and `fc_mqtt_topic`, `FC_EV_NET`. The client is in the core (`core/src/fc_mqtt.c`, MQTT 3.1.1 written there, QoS 0 and 1): one connection for all the apps, with the broker from `/etc/fc/mqtt.json` and its CA from `/etc/fc/mqtt-ca.crt`, reconnected with its subscriptions; started by the agent after enrollment, before the apps, and again when the fleet changes the files; connecting only while the agent has nothing else on the network and the heap has room for a handshake above the agent's 32 KB. The payloads received wait in one pool of 4 KB shared by the apps, half of it at most for one app, in place of 8 KB per app; where mbedTLS's receive buffer is the full record (Zephyr), a second block in the TLS pool, reserved only while a TLS broker is configured, as the connection stays open during downloads. It passes the unit tests (an app against a fake broker: its subscriptions, messages both ways, `FC_EV_NET`, the link lost and back, the app removed) and a test against mosquitto 2.0.22 in Docker without and with TLS (`tests/mqtt-broker.sh`: a wrong password and a wrong CA refused, messages both ways on the device's and the fleet's prefixes, one too large dropped, the keep-alive, the broker restarted). The lab's broker is `tools/mqtt/up.sh`, the example app `wasm-apps/mqttdemo`. On the boards with v3.14-rc6 it works on the five ESP-IDF chips, both ways, the client's thread on 8 KB of stack, of which it used 4.2 KB at most ([RESULTS.md](RESULTS.md#what-works)). To come:
  - the heap after a restart: on the C3 its lowest in the first 23 s is 3 KB with MQTT, 9 KB without. The broker's handshake no longer runs beside the agent's own, but likely beside the first spec being applied: that could wait for the handshake, as it waits for a metrics export;
  - on the boards: the connection kept through a download, and MQTT with Zephyr (built and linked for the ESP32-S3, not yet run);
  - not planned for 1.0: the device's certificate as the client's for the broker (today a user name and password), a QoS 1 message sent again after a lost link (it is sent once), QoS 2, a will message, a persistent session.
- [ ] A new firmware and an app that needs it, in one spec. The device checks a spec's apps before it installs the firmware, so the running firmware refuses an app that imports a function only the new one has (`env.snprintf`, before v3.14), and the firmware is held back with it ([RESULTS.md](RESULTS.md#current-limitations)); today such a release takes two steps. The fix is in the device: when the spec also changes the firmware, install the firmware first and check the apps with the new one, or leave an app that fails on a missing import for after the restart.

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
- [ ] On Zephyr, the device protected beyond signed images and anti-rollback: today its key is in the storage partition in clear and the flash is not encrypted. On an Espressif chip a protected device runs the ESP-IDF firmware; on Zephyr the protection comes with the TF-M chips above. For the ESP32-S3 under Zephyr, it would take Espressif's port of MCUboot in place of sysbuild's ([what it takes](SECURITY.md#zephyr)), and with encryption on each write to the storage erases its 4 KB sector and writes it back whole, which a power cut in between breaks.

Nice to have, last:

- [ ] A soak test: a fleet of boards left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.

## Upstream (Flight Control)

The gaps found in Flight Control for this class of device, each with a proposal, are the [limitations in RESULTS.md](RESULTS.md#flight-control-limitations-found-upstream-proposals). They were checked against 1.4.0 and against Flight Control's main branch of 12 November 2026: all hold there, except the revocation of enrollment certificates (limitation 8), fixed on main and not yet released.

To propose:

- Firmware and apps built by Flight Control. Its image builder (`ImageBuild`, `flightctl new-version`) builds bootc images for Linux devices: a base image with the agent added, out as a container, qcow2 or ISO. A build of another kind would cover these devices: a toolchain image (ESP-IDF, Zephyr's SDK, clang for WASM), sources from Git and a list of chips, out as the signed multi-chip OCI index `tools/fw-push.py` makes, or a WASM artifact. `new-version` from a previous build is where the delta from that release would be made, and the build is where an SBOM per release would come from (today `tools/fw-push.py` makes it with `tools/sbom.py`, [SBOM.md](SBOM.md)). It needs an agreed artifact type for firmware first; to be raised as an issue before any code, after reading the image builder's source.

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
