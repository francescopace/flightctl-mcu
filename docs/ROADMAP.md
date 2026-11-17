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

Checks not made yet:

- [ ] The rest of HTTP/2 on the boards. On the five ESP-IDF chips and on the ESP32-S3 with Zephyr the spec's long-poll and the status share one HTTP/2 connection, with the receive buffer from the block reserved at start; on the ESP32, C5, C6 and S3 (ESP-IDF) firmware and apps are downloaded from the registry over HTTP/2 with the agent's connection closed meanwhile, and the TLS buffers are dynamic again (only full-size records take the reserved block). Left: a certificate renewal, Zephyr's downloads over HTTP/2 on a board, and the traffic it saves or costs. (The end-to-end tests pass over HTTP/2, in CI and in the emulator.)
- [ ] On ESP-IDF, the core's own WiFi provisioning (`core/src/fc_prov.c`, as on Zephyr): built for every chip, and it passes the end-to-end tests in the emulator (C5, S3, C6), where a network given in the field is joined and kept across restarts. Not yet run on a board. (The core's delta decoder, which replaced detools' library, runs on the boards: the ESP32, C5, C6 and S3 took v3.13-rc1 and rc2 as deltas, `Applied`.)
- [ ] A soak test: the lab fleet left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.
- [ ] `lastFatalError` on the ESP32-S3 with Zephyr: the `pc` and the backtrace read from Zephyr's exception frame, built, not yet run (no emulator runs this build; on Arm and with ESP-IDF it is verified, [RESULTS.md](RESULTS.md)). A risk on that build: about 200 bytes of DRAM are left when it links (static data, not the heap the firmware runs with), so the next static buffer in the core or the Zephyr port must take its room from something else, or the S3 build no longer links.

What an app can do and hold:

- [ ] A large app's memory on the classic ESP32, the C5 and the C3, every time. CoreMark's 64 KB takes one free block of 68 KB, and in v3.12 one install or swap in seven still failed on each for want of it (largest free block 48 and 64 KB, [RESULTS.md](RESULTS.md#coremark)); after an hour of app changes the C5's largest block was 60 KB with the `alarm` app. To find: what stays in the middle of the heap from one app to the next, and whether an app that failed for want of memory is tried again on its own. With v3.13 it fitted once in two tries on the ESP32 and the C5 and once in five on the C3 (only right after a restart; a swap found 22 KB the largest of 66 KB free), with the metrics on or off.
- [ ] A floor of heap for the agent. While CoreMark ran, the heap went down to 1.8 KB on the C6, 2.3 KB on the classic ESP32 and 108 bytes on the C3, where one of the agent's TLS handshakes failed for want of memory while CoreMark's memory was taken. An app whose memory leaves the agent less than 32 KB of internal heap, once its instance is made, is now refused ([APPS.md](APPS.md#size-and-limits)); passes the unit tests. Left: the figure measured on each chip (the heap left after the instance, against the lowest while the agent reconnects and takes a spec), and CoreMark refused on the C3 and running on the others.
- [ ] CoreMark slower and less even on the C5 and C6 in v3.12 than with the same `-Os` firmware before it: 11.45–11.98 on the C6 against 12.2, and on the C5 about 19.7 or 22.3 from one run to the next, once 14.8 ([RESULTS.md](RESULTS.md#coremark)). Not explained yet. With v3.13, four runs on the C6: 11.81 to 11.98; one on the C5: 19.46.

- [ ] Code that a module runs as it is instantiated: a start section, or an exported `__wasm_call_ctors` (C++ constructors, TinyGo). WAMR runs it before the app's call context exists, with no instruction budget, so a loop there is never stopped: the agent's spec loop waits for the app's thread forever, the app can be neither updated nor removed, and the module, already saved, starts again at boot. An `fc` call from there gets no app (a null pointer). The record of apps that crash the device as they start does not catch it: it is a loop, not a crash, and ESP-IDF's task watchdog only logs. Seen on the host; not yet tried on a board. A module can do this by mistake as well as on purpose.
- [ ] CoreMark with Zephyr. With ESP-IDF it runs on every chip ([RESULTS.md](RESULTS.md#coremark)); the S3 runs wasm-coremark's 128 KB in its PSRAM, with an image for quad PSRAM and one for octal. With Zephyr both builds run on the ESP32-S3 N8R8 from its PSRAM (z26: 17.8 and 21.7). That build is for the N8R8 alone, and an S3 without PSRAM is left without room for CoreMark.
- [ ] Apps against [Toit](https://toit.io), the other way to change code on an ESP32 without reflashing ([COMPARISON.md](COMPARISON.md#tied-to-one-chip-vendor)), and against WAMR's AOT mode, on the same ESP32-C6. Toit publishes no figures against native code or WebAssembly, only against MicroPython; the emulator's timings follow the host, not the chip. Three benchmarks:
  - Richards, an operating system's task scheduler: method calls and pointers between objects. It exists in Toit, written by its team, and in C, which builds to a `.wasm` unchanged with a small allocator for its few blocks taken at start. Toit against the interpreter, then AOT;
  - DeltaBlue, a constraint solver that allocates and frees objects at every step: what a language with a garbage collector does best. The `.wasm` needs a real `malloc` and `free` (wasi-libc's, or emmalloc), so the result also helps decide whether apps get `malloc` (below);
  - CoreMark in AOT, against the interpreter's 12.2 iterations per second on the C6 ([RESULTS.md](RESULTS.md#coremark)). Not in Toit: CoreMark is defined by its C source, and a port to a language without 16-bit unsigned integers would measure the port. AOT takes `wamrc` and a module per architecture (Xtensa, RISC-V), executable memory (on the C6, ESP-IDF's memory protection off), and another way to stop an app than the instruction count, which AOT code does not keep. A test image is enough for the measure.
- [ ] Two more well-known apps as demos, from their published `.wasm` where the firmware can: the blink of [wasm3-arduino](https://github.com/wasm3/wasm3-arduino/tree/main/wasm_apps) (built from C++, Rust, AssemblyScript and TinyGo), and a TinyML model. Tried on the host. What each one needs:
  - the blink imports `arduino` (`getPinLED`, `pinMode`, `digitalWrite`, `delay`), with pin numbers that would map to board-map names granted in `FC_ALLOW`. Its `_start`, an endless loop with `delay(900)`, would run as CoreMark's `run()` does; `delay` would have to wait longer than an `fc` call may (100 ms at a time). Memory: 64 KB for the C++ build, which exports no `__heap_base`, and for the TinyGo one, which does; 2 KB for Rust. The AssemblyScript build calls `getPinLED` from its start function (above);
  - for TinyML, there is no ready `.wasm` for a microcontroller: Edge Impulse's is for a browser or Node, built with Emscripten (it imports `env.abort`). Instead, the C that [emlearn](https://emlearn.readthedocs.io) generates builds unchanged with `fc_app.h`. A random forest on `die_temp` takes 2.5 KB, imports only `fc` and runs on the firmware as it is. An MLP also needs `expf`, `tanhf` and `logf`, so the math functions belong with the C library of the app interface (below);
  - CoreMark's WASI build would need, beyond the above, reference types and 8 functions of `wasi_snapshot_preview1`: the two for arguments, the clock, `fd_write` (to the log), `proc_exit` and 3 file calls.
- [ ] The app interface for 1.0 ([APPS.md](APPS.md)). An app has its state, log, config, a key/value store, timers and events, and GPIO with interrupts and LEDs under the names of a board map that the fleet extends; a call that does not return is stopped. To come:
  - I2C, SPI, UART (with RS-485), ADC and PWM with devices wired to them. With ESP-IDF they open and answer on the ESP32, C5, C6 and S3 with nothing wired (an I2C bus scanned, a transfer, a UART write, a floating ADC pin, a PWM), and in the emulator on the C3 too; on Zephyr, where they come from the devicetree, they pass on `native_sim`'s emulated drivers: a board that names its hardware, and RS-485;
  - the instruction budget of a call, now 5 million of WAMR's operations (from 20 million), measured on the boards with v3.14-rc1: 0.66 s on the C5, 0.9 s on the S3, 1.06 s on the C3, 1.2 s on the ESP32 and 1.3 s on the C6, the slowest. The C library's 1 s limit counts only the time spent in it and in the waits, so a loop that calls it is no longer stopped by that limit before its budget on the ESP32, the C3 and the C6 (unit tests; not yet run on a board);
  - the C library ([APPS.md](APPS.md#the-c-library)): strings, `snprintf`, `printf` to the app's log, `strtol`, `ctype`, from WAMR's libc-builtin; passes the unit tests on the host (with a loop of native calls stopped after a second, as the budget does not count them), and builds with ESP-IDF and Zephyr. It costs 7.7 KB of flash on the C3; the tables of native functions are const, in flash, so the static RAM is 1.5 KB lower than before it (`fc`'s table moved too). Left: a run on a board and on Zephyr, with the app thread's stack measured after `printf` (the firmware's formatter runs on the app's 8 KB thread); `%f` on Zephyr, whose boards are built with picolibc's printf without floating point; and whether apps get `malloc` (a heap in each app's memory) and the math functions (`expf`, `tanhf`, `logf` for an MLP).
- [ ] MQTT for the apps. The calls are in the SDK (`fc_mqtt_publish`, `fc_mqtt_subscribe`, granted by `"mqtt"`); the firmware answers them with `FC_ENOTSUP`. To come:
  - one connection to the broker for all the apps, with the broker and its CA from `/etc/fc/mqtt.json`. Its TLS connection takes the RAM that one TLS connection at a time frees, and a second block in the TLS receive-buffer pool: unlike the agent's connection, it stays open during downloads. Even then, a C3 at its lowest has room for one more TLS handshake at a time;
  - for the payloads received, a pool shared by the apps' event queues, which could otherwise hold 8 KB per app.
- [ ] A size limit for an app that the chip can hold. The device accepts a `.wasm` up to 64 KB, which no chip can run: in RAM an app costs about 3 times its `.wasm` once loaded, plus its linear memory, about 3.5 KB for the instance and 8 KB for its thread ([RESULTS.md](RESULTS.md#ram)). The limit should follow the heap, and the thread stack, the largest part of what a small app costs, can shrink.
- [ ] A new firmware and an app that needs it, in one spec. The device checks a spec's apps before it installs the firmware, so the running firmware refuses an app that imports a function only the new one has (`env.snprintf`, before v3.14), and the firmware is held back with it ([RESULTS.md](RESULTS.md#current-limitations)); today such a release takes two steps. The fix is in the device: when the spec also changes the firmware, install the firmware first and check the apps with the new one, or leave an app that fails on a missing import for after the restart.

The documentation:

- [ ] Screenshots of Flight Control's UI, of real boards on Flight Control 1.4, in `docs/img/`: README, QUICKSTART, DEVICES, ZEPHYR, APPS (with the software catalog), DELTA, CONSOLE, SBOM and REGISTRY have theirs. Left:
  - PROVISIONING: an enrollment request waiting for approval, with its labels (a board enrolling);
  - METRICS: the device's and an app's metrics in Prometheus. The UI of 1.4 has no view of metrics, and the kind setup runs no Prometheus, only the gateway's exporter; `alarm` sends no metric of its own (`trend` does);
  - SBOM: a device with a vulnerability listed (Trustify running, with a test advisory); the panels shot show none.

  None for COMPARISON, DEPENDENCIES, RESULTS, SECURITY and TESTING: what they show is not in the UI.

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
