# Roadmap

What is left before 1.0 and after it. What the agent does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md).

## v1.0 (In progress)

Verification:

- [ ] **Blocked** — Collect an idle HTTP/2 window on a C3 with an empty console list for comparison with HTTP/1.1. Eleven outstanding console entries kept the server from holding the long-poll open during the 330 s sample. [Traffic measurements](RESULTS.md#idle-traffic) · [Console session lifetime](FLIGHTCTL-LIMITATIONS.md#15-a-console-session-whose-user-left-before-the-device-came-stays-listed)
- [ ] **Blocked** — Measure idle HTTP/2 traffic on the Zephyr N8R8 with PSRAM disabled and an empty console list. The previous window retained 15 console entries. [Traffic measurements](RESULTS.md#idle-traffic) · [Zephyr on ESP32-S3](ZEPHYR.md#on-an-esp32-s3)

What an app can do and hold:

- [ ] Determine where the C3 stops processing a rendered spec under load when a console client times out. The timeout recurred with the HTTP/2 buffer fallback enabled. Capture the last completed step before reset. [Console timing](RESULTS.md#timings)
- [ ] CoreMark slower and less even on the C5 and C6 since v3.12 than with the same `-Os` firmware before it. Not explained yet ([RESULTS.md](RESULTS.md#coremark)).
- [ ] CoreMark with Zephyr on an ESP32-S3 without PSRAM: it runs only from the N8R8's PSRAM, and the C heap is about 20 KB short ([RESULTS.md](RESULTS.md#coremark)).
- [ ] Verify GPIO interrupts and I2C, SPI, UART, ADC and PWM with wired devices. On Zephyr, verify devicetree hardware names on a board and measure ESP32-S3 RS-485 driver-enable timing with a transceiver and a responding peer. See [APPS.md](APPS.md).

### Blocked on Flight Control

Each needs one of the [limitations in FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md) lifted:

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll (limitation 3).
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device (limitation 13).
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one. Written and switched off (limitation 9).
- [ ] The firmware from the software catalog, as apps already can (limitation 11).
- [ ] Revoking a batch's enrollment certificate from `tools/enrollment.sh`. Flight Control's main branch has the API (limitation 8); it waits for a release.

### Blocked on hardware availability

- [ ] **ESP32-S3 manufactured without PSRAM** — Install Zephyr z30 or later over USB and verify the firmware on that board. The older z28 failed to download its update with one app running and a C heap low of 368 B. The N8R8 measurements with PSRAM disabled are in [RESULTS.md](RESULTS.md#zephyr-on-the-esp32-s3).
- [ ] **ST NUCLEO-F767ZI (ordered)** — Repeat the [emulated Arm Ethernet test](ZEPHYR.md#on-an-emulated-arm-cortex-m3) on the physical board. Enroll the Zephyr agent over Ethernet, verify that Flight Control receives status updates, then power-cycle the board and confirm it reconnects with the same device identity.
- [ ] **Raspberry Pi Pico 2 W with headers (RP2350, SC1634, ordered)** — Enroll the Zephyr agent over WiFi, run a signed WASM app and record the minimum free heap with the app active.
- [ ] **Nordic nRF7002-DK (ordered)** — Run the Zephyr agent on the nRF5340 and enroll over the nRF7002 WiFi connection. Verify status updates, deploy a signed WASM app and measure the minimum free heap while the app runs.

## v2.0 (Planned)

Proposed for 2.0, from what similar products offer ([COMPARISON.md](COMPARISON.md)):

- [ ] Let operators enable automatic coredump uploads after reboot to an external destination of their choice, such as an S3-compatible bucket.
- [ ] Verify enrollment and status over LTE-M or NB-IoT using an nRF91 or an external cellular modem with a supported host board and network subscription. Measure traffic and power with the spec long-poll active.
- [ ] Use the nRF7002-DK as the BLE or Thread device in a Linux gateway test. The gateway needs a matching radio interface and must relay the device's identity, spec, status and firmware. [Board radios](https://www.nordicsemi.com/Products/Development-hardware/nRF7002-DK)
- [ ] Integrate a persistent, non-exportable TF-M device key on Zephyr. Use the ordered nRF7002-DK for the first enrollment and reconnect test, then verify the Pico 2 W integration. Check that the same identity authenticates after a power cycle. STM32U5 coverage would require that board. [TF-M on nRF7002-DK](https://docs.zephyrproject.org/latest/boards/nordic/nrf7002dk/doc/index.html#building-the-secure-firmware-with-tf-m)
- [ ] Verify flash encryption separately on a Zephyr target that supports it. Cover the device's stored credentials and application data. [Current Zephyr protections](SECURITY.md#zephyr)
- [ ] Apps against [Toit](https://toit.io) and against WAMR's AOT mode, on the same ESP32-C6 ([the benchmarks](COMPARISON.md#tied-to-one-chip-vendor)).
- [ ] A soak test: a fleet of boards left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.
