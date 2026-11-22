# Roadmap

## v1.0.0 (In progress)

The remaining upstream dependency for v1.0.0 is enrollment with an RSA device key. [Flight Control blocker](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-v100)

## v2.0.0 (Planned)

### Connectivity and fleet tests

- Test LTE-M or NB-IoT enrollment and status on nRF91 or an external modem. Measure long-poll traffic and power.
- Test a Linux gateway relaying nRF7002-DK identity, spec, status and firmware over BLE or Thread.
- Track sampled availability, restart causes, minimum heap and hourly traffic in a [multi-week fleet test](../tools/soak/collect.py).

### Security

- Enroll with a non-exportable TF-M key. Power-cycle and verify identity on nRF7002-DK, then Pico 2 W.
- Verify flash encryption for stored credentials and app data on a supported Zephyr target. [Current protections](SECURITY.md#zephyr)

### Firmware

- Support opt-in coredump uploads after reboot to an operator-selected destination. Test that a device that opted in delivers its dump there after reboot and that other devices upload none.
- Evaluate fleet-scheduled sleep, RTC/GPIO/timer wakeups and status after the [disconnect timeout](FLIGHTCTL-LIMITATIONS.md#5-fixed-5-minute-disconnect-timeout-devicedisconnectedtimeout). Record the status Flight Control shows for a device that sleeps past the timeout, for each wakeup source.

### App runtime

- Evaluate optional WAMR AOT for compute-heavy apps. Measure latency, RAM, flash and impact on interpreted apps. Test stopping a stuck AOT app.
- Evaluate signed ELF for trusted apps that need chip-specific libraries or miss latency targets in WASM.
- Compare native and WASM inference latency, flash and RAM on identical inputs.

### App API

- Read a sensor with a BLE GATT client. Assess GATT server, HID, radio, mesh and Matter as deployments require.
- Assess local TCP and UDP sockets with fleet-approved destinations and limits on connections and buffers. Test that an app reaches only approved destinations and stays within the limits.
- Evaluate display drawing and private, quota-limited files with random access.
- Assess native hashing, message authentication and encryption.
- Assess read-only app access to lifecycle, OTA and watchdog status.
- Test app RPC through Flight Control. Send a self-test to a named app and verify it replies.
- Evaluate HTTP GET for local REST instruments with fleet-approved endpoints and bounded responses. Test that an app reads only approved endpoints and that a response stays within its bound.
- Evaluate product-specific native imports for authorized apps. Test a meter read on one product and missing-import reporting on another.

### Flight Control

- File an issue on the firmware artifact type and image builder support for MCU firmware and apps, covering signed multi-chip indexes, deltas and [SBOMs](SBOM.md).
