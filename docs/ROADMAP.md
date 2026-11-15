# Roadmap

flightctl-mcu lets microcontrollers be managed by an unmodified [Flight Control](https://github.com/flightctl/flightctl): enrollment, monitoring, configuration, firmware OTA and WebAssembly apps, in the same fleets as Linux devices.

What it does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md). This page is what is left.

## Principles

- **No server changes.** Everything goes through the public agent API. Gaps in Flight Control are raised upstream, not patched locally.
- **Portable core.** `core/` is POSIX C with mbedTLS, cJSON and WAMR. Chip and RTOS specifics live behind `port/`.
- **Verifiable without hardware.** Every feature also runs on the simulated device, so CI can check it against a real Flight Control on kind. Hardware tests add to that; they don't replace it.

## Next

Chips ([support levels](DEVICES.md#support-levels)):

- [ ] The Zephyr port on a real Arm board with Ethernet: an ST Nucleo (H563ZI or F767ZI). In QEMU (`mps2/an385`, a Cortex-M3 on Ethernet) it passes the end-to-end test, in CI ([ZEPHYR.md](ZEPHYR.md#on-an-emulated-arm-cortex-m3)).
- [ ] The heap left on the C3 by v3.7 or later and its smaller status thread (it freed 37 KB on the classic ESP32; the C5 has 131 KB free with v3.8, against 59 KB with v3.0).

Checks not made yet:

- [ ] The rest of HTTP/2 and the TLS receive-buffer pool on the boards. They run on the five ESP-IDF chips (v3.9) and on the ESP32-S3 with Zephyr (z24): the spec's long-poll and the status over one HTTP/2 connection, the receive buffers from the reserved blocks. Left: a download from the registry over HTTP/2 on a board (the downloads of v3.9 and z24 were made by the firmware before them), a certificate renewal, the end-to-end tests, and the RAM and traffic it saves or costs. On ESP-IDF the TLS buffers are no longer dynamic: the two reserved blocks (34 KB) stay taken while the connections are idle, where dynamic buffers used to give that RAM back between records.
- [ ] On ESP-IDF, the core's own WiFi provisioning (`core/src/fc_prov.c`, as on Zephyr) and delta decoder (`core/src/fc_delta.c`), which replace Espressif's `network_provisioning` and detools' library: built for every chip; the decoder gave the same images as detools on 300 random patches and 8 pairs of firmware images, and passed its fuzzer. Not yet run in the emulator nor on a board.
- [ ] The way back from a firmware that does not work, on the C6. It is tested in the emulator and on the ESP32-S3 with both ports.
- [ ] A soak test: the lab fleet left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); there has been no fleet to watch for weeks.
- [ ] Two parts of decommissioning and the registry never tried: the `FactoryReset` target (the CLI and the UI send `Unenroll` only), and Bearer tokens against a real registry (Harbor, Quay); `registry:3` asks for Basic.
- [ ] The Zephyr build for the ESP32-S3 in CI. It needs the Xtensa toolchain; CI builds `native_sim` and, with the Arm toolchain, `mps2/an385`.
- [ ] Zephyr: the report of the last fatal error is only printed on the console at boot. Report it in the device's status too.

What the Zephyr port does not have, and the ESP-IDF firmware has:

- [ ] Protection of the device beyond signed images and anti-rollback: the device key is in the storage partition in clear, the flash is not encrypted. On the ESP32-S3 it takes Espressif's MCUboot port; a chip with TF-M (RP2350, nRF54L, STM32U5) would keep the key protected ([SECURITY.md](SECURITY.md#zephyr)).

What Flight Control offers and the agent does not answer yet:

- [ ] The remote terminal: `flightctl console device/<name>` and the Terminal tab of the UI. Flight Control puts the session in the rendered spec (`consoles`), and the agent is to open a gRPC stream for it on the endpoint it already uses. That takes gRPC over the core's HTTP/2 client, the stream sharing the agent's connection instead of a third TLS connection, which the boards shortest of RAM may not have room for; and a closed set of commands in place of a shell, which a microcontroller does not have (status, heap, apps, log, reboot).
- [ ] The device's log from the UI. Flight Control has no API for logs: the Log tab runs `journalctl` through the terminal's channel. So it comes after the terminal: a log kept in RAM (today it goes to the serial port only), shown by a command, and then an answer to the commands the UI sends, which holds only as long as the UI sends those.

What an app can do and hold:

- [ ] Code that a module runs as it is instantiated: a start section, or an exported `__wasm_call_ctors` (C++ constructors, TinyGo). WAMR runs it before the app's call context exists, with no instruction budget. Seen on the host, with the firmware's app manager: a module whose start function loops can never be removed, because the agent waits for its thread forever, and an `fc` function called from there gets no app (a null pointer). A spec that changes or removes the app waits on that thread too, and the module is saved, to start again at boot. Still so with v3.7, whose spec loop, on the main task, then waits there. The record of apps that crash the device as they start (v3.5) counts only restarts after a panic or a watchdog reset, and ESP-IDF's task watchdog is set to log, not reset; the loop runs at the agent's priority, so it shares the CPU with it. Not yet tried on a board. A module can do this by mistake as well as on purpose.
- [ ] CoreMark with Zephyr. With ESP-IDF it runs on every chip with v3.8 ([RESULTS.md](RESULTS.md#coremark)); from v3.9 the S3 runs wasm-coremark's 128 KB in its PSRAM, with an image for quad PSRAM and one for octal. With Zephyr (z24) it runs on the ESP32-S3 N8R8 from its PSRAM; that build is for the N8R8 alone, and an S3 without PSRAM is left without room for CoreMark.
- [ ] Two more well-known apps as demos, from their published `.wasm` where the firmware can: the blink of [wasm3-arduino](https://github.com/wasm3/wasm3-arduino/tree/main/wasm_apps) (built from C++, Rust, AssemblyScript and TinyGo), and a TinyML model. Tried on the host. What each one needs:
  - the blink imports `arduino` (`getPinLED`, `pinMode`, `digitalWrite`, `delay`), with pin numbers that would map to board-map names granted in `FC_ALLOW`. Its `_start`, an endless loop with `delay(900)`, would run as CoreMark's `run()` does; `delay` would have to wait longer than an `fc` call may (100 ms at a time). Memory: 64 KB for the C++ build, which exports no `__heap_base`, and for the TinyGo one, which does; 2 KB for Rust. The AssemblyScript build calls `getPinLED` from its start function (above);
  - for TinyML, there is no ready `.wasm` for a microcontroller: Edge Impulse's is for a browser or Node, built with Emscripten (it imports `env.abort`). Instead, the C that [emlearn](https://emlearn.readthedocs.io) generates builds unchanged with `fc_app.h`. A random forest on `die_temp` takes 2.5 KB, imports only `fc` and runs on the firmware as it is. An MLP also needs `expf`, `tanhf` and `logf`, so the math functions belong with the libc question of the app interface (below);
  - CoreMark's WASI build would need, beyond the above, reference types and 8 functions of `wasi_snapshot_preview1`: the two for arguments, the clock, `fd_write` (to the log), `proc_exit` and 3 file calls.
- [ ] The app interface for 1.0 ([APPS.md](APPS.md)). An app has its state, log, config, a key/value store, timers and events, and GPIO with interrupts and LEDs under the names of a board map that the fleet extends; a call that does not return is stopped. To come:
  - I2C, SPI, UART (with RS-485), ADC and PWM, on ESP-IDF, then on Zephyr through the devicetree; GPIO on Zephyr;
  - metrics for Flight Control's Telemetry Gateway, which takes OTLP over gRPC only: gRPC over the core's HTTP/2 client, as for the remote terminal, and a protobuf encoder;
  - the instruction budget of a call, measured on a C3 so that it comes to about a second there;
  - whether apps get WAMR's small libc (`memcpy`, `snprintf`...), a few KB of firmware, since building a payload by hand is the biggest burden on a real app.
- [ ] MQTT for the apps. The calls are in the SDK (`fc_mqtt_publish`, `fc_mqtt_subscribe`, granted by `"mqtt"`); the firmware answers them with `FC_ENOTSUP`. To come:
  - one connection to the broker for all the apps, with the broker and its CA from `/etc/fc/mqtt.json`. Its TLS connection takes the RAM that the HTTP/2 client frees by putting the spec and the status on one connection, and a third slot in the TLS receive-buffer pool. Even then, a C3 at its lowest has room for one more TLS handshake at a time;
  - for the payloads received, a pool shared by the apps' event queues, which could otherwise hold 8 KB per app.
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
