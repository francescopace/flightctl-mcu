# Roadmap

## v1.0.0 (In progress)

Agent v1.0.0 depends on the [upstream Flight Control changes](FLIGHTCTL-LIMITATIONS.md#upstream-blockers-for-agent-v100).

Testing is still pending on the following boards.

- [ ] **[Test]** ST NUCLEO-F767ZI — Enroll over Ethernet, verify status and reconnect with the same identity after a power cycle.
- [ ] **[Test]** Raspberry Pi Pico 2 W (SC1634) — Enroll over Wi-Fi, run a signed WASM app and measure minimum free heap.
- [ ] **[Test]** Nordic nRF7002-DK — Enroll over Wi-Fi, verify status, run a signed WASM app and measure minimum free heap.

## v2.0.0 (Planned)

- [ ] **[Test]** Test LTE-M or NB-IoT enrollment and status on nRF91 or an external modem. Measure long-poll traffic and power.
- [ ] **[Test]** Test a Linux gateway relaying nRF7002-DK identity, spec, status and firmware over BLE or Thread.
- [ ] **[Test]** Track sampled availability, restart causes, minimum heap and hourly traffic in a [multi-week fleet test](../tools/soak/collect.py).
- [ ] **[Security]** Enroll with a non-exportable TF-M key. Power-cycle and verify identity on nRF7002-DK, then Pico 2 W.
- [ ] **[Security]** Verify flash encryption for stored credentials and app data on a supported Zephyr target. [Current protections](SECURITY.md#zephyr)
- [ ] **[Benchmark]** Compare Toit and interpreted WASM on the same board using Richards and DeltaBlue.
- [ ] **[Apps]** Evaluate optional WAMR AOT for compute-heavy apps. Measure latency, RAM, flash and impact on interpreted apps. Test stopping a stuck AOT app.
- [ ] **[Apps]** Evaluate signed ELF for trusted apps that need chip-specific libraries or miss latency targets in WASM.
- [ ] **[Firmware]** Support opt-in coredump uploads after reboot to an operator-selected destination.
- [ ] **[Firmware]** Evaluate fleet-scheduled sleep, RTC/GPIO/timer wakeups and status after the [disconnect timeout](FLIGHTCTL-LIMITATIONS.md#5-fixed-5-minute-disconnect-timeout-devicedisconnectedtimeout).
- [ ] **[WASM]** Read a sensor with a BLE GATT client. Assess GATT server, HID, radio, mesh and Matter as deployments require.
- [ ] **[WASM]** Assess local TCP and UDP sockets with fleet-approved destinations and limits on connections and buffers.
- [ ] **[WASM]** Evaluate display drawing and private, quota-limited files with random access.
- [ ] **[WASM]** Assess native hashing, message authentication and encryption.
- [ ] **[WASM]** Compare native and WASM inference latency, flash and RAM on identical inputs.
- [ ] **[WASM]** Assess read-only access to lifecycle, OTA and watchdog status. The agent handles deployment, OTA confirmation and watchdog servicing.
- [ ] **[WASM]** Test app RPC through Flight Control. Send a self-test to a named app and verify it replies.
- [ ] **[WASM]** Evaluate HTTP GET for local REST instruments with fleet-approved endpoints and bounded responses.
- [ ] **[WASM]** Evaluate product-specific native imports for authorized apps. Test a meter read on one product and missing-import reporting on another.
- [ ] **[Flight Control]** File an issue on the firmware artifact type and image builder support for MCU firmware and apps, covering signed multi-chip indexes, deltas and [SBOMs](SBOM.md).
