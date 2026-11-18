# Roadmap

What is left before 1.0 and after it. What the agent does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md).

## v1.0.0 (In progress)

The MCU agent is implemented and usable today with Flight Control 1.4.0. Before the official 1.0.0 release, we need Flight Control to address five upstream limitations. We will open issues or submit pull requests for the outstanding changes. [Five upstream blockers for agent 1.0.0](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-100)

### Blocked on hardware availability

- [ ] **ST NUCLEO-F767ZI (ordered)** — Repeat the [emulated Arm Ethernet test](ZEPHYR.md#on-an-emulated-arm-cortex-m3) on the physical board. Enroll the Zephyr agent over Ethernet, verify that Flight Control receives status updates, then power-cycle the board and confirm it reconnects with the same device identity.
- [ ] **Raspberry Pi Pico 2 W with headers (RP2350, SC1634, ordered)** — Enroll the Zephyr agent over WiFi, run a signed WASM app and record the minimum free heap with the app active.
- [ ] **Nordic nRF7002-DK (ordered)** — Run the Zephyr agent on the nRF5340 and enroll over the nRF7002 WiFi connection. Verify status updates, deploy a signed WASM app and measure the minimum free heap while the app runs.

## v2.0.0 (Planned)

- [ ] Let operators enable automatic coredump uploads after reboot to an external destination of their choice, such as an S3-compatible bucket.
- [ ] Verify enrollment and status over LTE-M or NB-IoT using an nRF91 or an external cellular modem with a supported host board and network subscription. Measure traffic and power with the spec long-poll active.
- [ ] Use the nRF7002-DK as the BLE or Thread device in a Linux gateway test. The gateway needs a matching radio interface and must relay the device's identity, spec, status and firmware. [Board radios](https://www.nordicsemi.com/Products/Development-hardware/nRF7002-DK)
- [ ] Integrate a persistent, non-exportable TF-M device key on Zephyr. Use the ordered nRF7002-DK for the first enrollment and reconnect test, then verify the Pico 2 W integration. Check that the same identity authenticates after a power cycle. STM32U5 coverage would require that board. [TF-M on nRF7002-DK](https://docs.zephyrproject.org/latest/boards/nordic/nrf7002dk/doc/index.html#building-the-secure-firmware-with-tf-m)
- [ ] Verify flash encryption separately on a Zephyr target that supports it. Cover the device's stored credentials and application data. [Current Zephyr protections](SECURITY.md#zephyr)
- [ ] Apps against [Toit](https://toit.io) and against WAMR's AOT mode, on the same ESP32-C6 ([the benchmarks](COMPARISON.md#tied-to-one-chip-vendor)).
- [ ] A soak test: a fleet of boards left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.
- [ ] Open a Flight Control issue to agree on a firmware artifact type and discuss extending its image builder to MCU firmware and apps. A firmware build could publish a signed multi-chip index, a delta from the previous release and an SBOM, as `tools/fw-push.py` does today. [Firmware SBOM](SBOM.md)
