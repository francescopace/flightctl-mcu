# Roadmap

What is left before 1.0 and after it. What the agent does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md).

## Next

Chips ([support levels](DEVICES.md#support-levels)):

- [ ] The Zephyr port on a real Arm board with Ethernet, an ST Nucleo (H563ZI or F767ZI). Arm is tried only in QEMU so far ([ZEPHYR.md](ZEPHYR.md#on-an-emulated-arm-cortex-m3)).

Checks not made yet:

- [ ] The rest of HTTP/2 on the boards ([REGISTRY.md](REGISTRY.md)):
  - a certificate renewal over it;
  - the traffic it saves or costs ([RESULTS.md](RESULTS.md#idle-traffic) has HTTP/1.1's only).
- [ ] z30 or later on the Zephyr ESP32-S3 without PSRAM, flashed over USB: z28 cannot update itself over the air there (368 bytes of C heap at the lowest). Then on it: MQTT beside a metrics export, the idle traffic over HTTP/2, and the watchdog of a stuck agent ([ZEPHYR.md](ZEPHYR.md#on-an-esp32-s3)).
- [ ] The core's own WiFi provisioning on an ESP-IDF board: it passes in the emulator only ([PROVISIONING.md](PROVISIONING.md)).
- [ ] `lastFatalError` after a real crash on the ESP32-S3 with Zephyr: the code is on the board but has never run on Xtensa ([RESULTS.md](RESULTS.md#a-crash-in-the-status)).

What an app can do and hold:

- [ ] A large app's memory on the classic ESP32 and the C5, every time: CoreMark runs on all five chips, but the classic ESP32 refuses one swap in three and the C5 leaves the agent too little heap for its status ([RESULTS.md](RESULTS.md#coremark)).
- [ ] Apps the spec removes, stopped and freed before the device writes the spec's config and downloads its firmware: a C3 holding two apps, 39 KB free, could not download a new firmware (the registry's manifest, `HTTP -1`, every 30 s), the apps being removed only after it.
- [ ] CoreMark slower and less even on the C5 and C6 since v3.12 than with the same `-Os` firmware before it. Not explained yet ([RESULTS.md](RESULTS.md#coremark)).
- [ ] CoreMark with Zephyr on an ESP32-S3 without PSRAM: it runs only from the N8R8's PSRAM, and the C heap is about 20 KB short ([RESULTS.md](RESULTS.md#coremark)).
- [ ] The app interface for 1.0 ([APPS.md](APPS.md)):
  - GPIO interrupts, and I2C, SPI, UART, ADC and PWM with devices wired to them, on a board; on Zephyr also a board that names its hardware in the devicetree, and RS-485;
  - the 1 s limit of a call in the device's functions, on a board (unit tests only);
  - the app's stack with the C library, measured on Xtensa.
- [ ] MQTT for the apps ([APPS.md](APPS.md#mqtt)):
  - on the C3 with an app using MQTT, the first metrics exports after a restart fail for want of memory, the heap down to 6 to 10 KB ([RESULTS.md](RESULTS.md#ram));
  - on the boards: the connection kept through a download, and MQTT with Zephyr beside a metrics export on the ESP32-S3, with z29's wait for heap.

## Blocked on Flight Control

Each needs one of the [limitations in FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md) lifted:

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll (limitation 3).
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device (limitation 13).
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one. Written and switched off (limitation 9).
- [ ] The firmware from the software catalog, as apps already can (limitation 11).
- [ ] Revoking a batch's enrollment certificate from `tools/enrollment.sh`. Flight Control's main branch has the API (limitation 8); it waits for a release.

## After 1.0

Proposed for 2.0, from what similar products offer ([COMPARISON.md](COMPARISON.md)):

- [ ] Crash reports beyond `lastFatalError`: the full coredump sent off the device, to be decoded against the release's ELF (which `tools/fw-push.py` already keeps in the registry). Flight Control has no place for a file from a device; the coredump could go to the registry as an artifact, with write credentials of its own.
- [ ] Cellular (LTE-M, NB-IoT): a modem as the link, through ESP-IDF's PPP or Zephyr's cellular drivers. A long-poll held open over cellular costs data and power, so it comes with the battery-powered devices above.
- [ ] Devices without IP (BLE, Thread) through a Linux gateway in the same fleet: the gateway's Flight Control agent, or an app on it, holds the device's identity and passes its spec, status and firmware.
- [ ] More silicon: Nordic nRF (nRF7002 WiFi, nRF91 cellular), RP2350, STM32U5, the last two with TF-M to keep the device key protected.
- [ ] On Zephyr, the device key protected and the flash encrypted: today only images are signed and rollback refused. It comes with the TF-M chips above; on an ESP32-S3 it would take Espressif's own MCUboot ([what it takes](SECURITY.md#zephyr)).
- [ ] Apps against [Toit](https://toit.io) and against WAMR's AOT mode, on the same ESP32-C6 ([the benchmarks](COMPARISON.md#tied-to-one-chip-vendor)).
- [ ] A soak test: a fleet of boards left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.

## Open questions

- Where the agent lives long term: in the Flight Control organization, as an ESP-IDF component in the Espressif registry, or both.
- Support policy: which Flight Control and ESP-IDF versions are supported, and for how long.
- Compliance work for products that ship it (for example the EU Cyber Resilience Act, ETSI EN 303 645).
