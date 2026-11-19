# Roadmap

## v1.0.0 (In progress)

Release 1.0 depends on [five Flight Control changes](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-100). We need to open upstream issues and submit patches for the outstanding changes.

- [ ] **[WASM]** Grant per-app access to named sensors via `FC_ALLOW`. Test one alarm on ESP-IDF and Zephyr with different probes sharing a name and reporting millidegrees Celsius. Support direct reads and, where the driver supports them, data-ready and threshold notifications through `on_event`.
- [ ] **[WASM]** Queue offline samples within each app's quota. Deliver oldest first, delete after delivery, verify restart recovery and measure capacity and flash writes.
- [ ] **[WASM]** Expose read-only battery voltage via named sensors, plus charge level and charging status when available. Report low battery via `fc_metric`.
- [ ] **[WASM]** Pass readings between local sensor and alarm apps while the broker is offline. Bound the queue, define overflow behavior and measure both apps' RAM use together.
- [ ] **[Test]** ST NUCLEO-F767ZI — Enroll over Ethernet, verify status and reconnect with the same identity after a power cycle.
- [ ] **[Test]** Raspberry Pi Pico 2 W (SC1634) — Enroll over Wi-Fi, run a signed WASM app and measure minimum free heap.
- [ ] **[Test]** Nordic nRF7002-DK — Enroll over Wi-Fi, verify status, run a signed WASM app and measure minimum free heap.

## v2.0.0 (Planned)

- [ ] **[Firmware]** Support opt-in coredump uploads after reboot to an operator-selected destination.
- [ ] **[Test]** Verify LTE-M or NB-IoT enrollment and status with an nRF91 or external modem. Measure traffic and power during spec long-polling.
- [ ] **[Test]** Test a Linux gateway relaying nRF7002-DK identity, spec, status and firmware over BLE or Thread.
- [ ] **[Security]** Enroll with a non-exportable TF-M key and verify the same identity after a power cycle, first on nRF7002-DK, then on Pico 2 W.
- [ ] **[Security]** Verify flash encryption for stored credentials and app data on a supported Zephyr target. [Current protections](SECURITY.md#zephyr)
- [ ] **[Benchmark]** On one ESP32-C6, compare Toit and WASM with Richards and DeltaBlue, then WAMR AOT and interpreter performance with CoreMark.
- [ ] **[Test]** Track downtime, restart causes, minimum heap and hourly traffic during a multi-week fleet soak with `tools/soak/collect.py`.
- [ ] **[Flight Control]** Open a Flight Control issue on firmware artifact type and image builder support for MCU firmware and apps. Consider signed multi-chip indexes, deltas and [SBOMs](SBOM.md).
- [ ] **[WASM]** Read a sensor with a BLE GATT client. Assess GATT server, HID, radio, mesh and Matter as deployments require.
- [ ] **[WASM]** Assess local TCP and UDP sockets with fleet-approved destinations and limits on connections and buffers.
- [ ] **[WASM]** Evaluate display drawing and private, quota-limited files with random access.
- [ ] **[Firmware]** Evaluate fleet-scheduled sleep with RTC, GPIO or timer wakeups. Verify device status after the [disconnect timeout](FLIGHTCTL-LIMITATIONS.md).
- [ ] **[WASM]** Evaluate native hashing, message authentication and encryption. Compare native and WASM inference on identical inputs for latency, flash and RAM.
- [ ] **[WASM]** Assess app access to lifecycle, firmware update and watchdog status. Keep deployment, OTA confirmation and watchdog servicing under agent control.
- [ ] **[WASM]** Let an operator send a request to a named app (RPC) and receive its reply through Flight Control. Evaluate the transport using a self-test request and verify that the reply comes from the selected app.
- [ ] **[WASM]** Evaluate an HTTP client for apps that read local instruments over REST. Test a GET request to a fleet-approved endpoint with a bounded response.
- [ ] **[WASM]** Evaluate product-specific native functions that firmware can register for authorized apps. Test a meter-reading function supplied by one product and check how the agent reports a missing import on another.
