# Roadmap

What is left before 1.0 and after it. What the agent does today is in the [README](../README.md#what-maps-to-what), and how it was verified in [RESULTS.md](RESULTS.md).

## v1.0 (In progress)

Verification:

- [ ] Rerun the Git-managed fleet test on the host, Zephyr `native_sim` and Arm QEMU with the previous inline registry credentials removed before applying the Secret mounts. Verify that all three clients pull the app with the mounted credentials and complete decommissioning. [End-to-end run](https://github.com/francescopace/flightctl-mcu/actions/runs/44348764399)
- [ ] **Blocked** — Collect an idle HTTP/2 window on a C3 with an empty console list for comparison with HTTP/1.1. The current device still has eleven stale console entries. [Traffic measurements](RESULTS.md#idle-traffic) · [Console session lifetime](FLIGHTCTL-LIMITATIONS.md#15-a-console-session-whose-user-left-before-the-device-came-stays-listed)
- [ ] **In progress** — Test Zephyr on the ESP32-S3 N8R8 with PSRAM disabled. Verify MQTT during a metrics export, idle HTTP/2 traffic and recovery from a stuck agent using internal RAM. Physical validation on a board manufactured without PSRAM is still open. On z28 an OTA update failed with 368 B of C heap free. [Zephyr on ESP32-S3](ZEPHYR.md#on-an-esp32-s3)

What an app can do and hold:

- [ ] **In progress** — Reduce transient C5 status failures under CoreMark load. Five PUT attempts failed during the ten-swap recovery test. Identify the remaining allocation or write failures and verify the result with the same workload. [CoreMark measurements](RESULTS.md#coremark).
- [ ] **Queued** — On a provisioned S3-mini, repeat the cold boot with a growable primary and 400 s of withheld management responses. Compare secondary start and heap with the fixed-size primary.
- [ ] **Queued** — On the S3-mini, repeat the spec change that restarts the primary with a new environment and updates the secondary in the same spec. Check when free internal heap falls below the 40 KB admission floor. The original run reached 32 KB, while the agent reported 37 KB.
- [ ] **In progress** — Repeat the C6's seven-entry board-map test with the original apps to account for the remaining heap difference. Also measure free storage before and after the test to verify the 576 B decrease observed in the original run.
- [ ] **Queued** — On a provisioned C3, verify the console thread failure reaches the remote client and determine why the earlier board opened a session after the client timed out.
- [ ] CoreMark slower and less even on the C5 and C6 since v3.12 than with the same `-Os` firmware before it. Not explained yet ([RESULTS.md](RESULTS.md#coremark)).
- [ ] CoreMark with Zephyr on an ESP32-S3 without PSRAM: it runs only from the N8R8's PSRAM, and the C heap is about 20 KB short ([RESULTS.md](RESULTS.md#coremark)).
- [ ] Verify GPIO interrupts and I2C, SPI, UART, ADC and PWM with wired devices. On Zephyr, verify devicetree hardware names on a board and measure ESP32-S3 RS-485 driver-enable timing with a transceiver and a responding peer. See [APPS.md](APPS.md).

### Firmware reliability and footprint

- [ ] **Queued** — Measure the peak heap of the variable-sized metrics export on a provisioned C3 with MQTT active after a restart. Capture the largest free block before instantiation, the minimum free heap during each export and any failed or skipped early exports. See [RAM measurements](RESULTS.md#ram).

### Blocked on Flight Control

Each needs one of the [limitations in FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md) lifted:

- [ ] Battery-powered devices: deep sleep, status only on wake-up, no long-poll (limitation 3).
- [ ] The same device name after re-enrollment: a device whose certificate expired while offline enrolls again as a new device (limitation 13).
- [ ] The device key in the Digital Signature peripheral on the ESP32 chips that have no ECDSA one. Written and switched off (limitation 9).
- [ ] The firmware from the software catalog, as apps already can (limitation 11).
- [ ] Revoking a batch's enrollment certificate from `tools/enrollment.sh`. Flight Control's main branch has the API (limitation 8); it waits for a release.

### Blocked on hardware availability

- [ ] Verify the Zephyr port over Ethernet when a physical ST Nucleo H563ZI or F767ZI is available. Arm has been tested in [QEMU](ZEPHYR.md#on-an-emulated-arm-cortex-m3).

## v2.0 (Planned)

Proposed for 2.0, from what similar products offer ([COMPARISON.md](COMPARISON.md)):

- [ ] Crash reports beyond `lastFatalError`: the full coredump sent off the device, to be decoded against the release's ELF (which `tools/fw-push.py` already keeps in the registry). Flight Control has no place for a file from a device; the coredump could go to the registry as an artifact, with write credentials of its own.
- [ ] Cellular (LTE-M, NB-IoT): a modem as the link, through ESP-IDF's PPP or Zephyr's cellular drivers. A long-poll held open over cellular costs data and power, so it comes with the battery-powered devices above.
- [ ] Devices without IP (BLE, Thread) through a Linux gateway in the same fleet: the gateway's Flight Control agent, or an app on it, holds the device's identity and passes its spec, status and firmware.
- [ ] More silicon: Nordic nRF (nRF7002 WiFi, nRF91 cellular), RP2350, STM32U5, the last two with TF-M to keep the device key protected.
- [ ] On Zephyr, the device key protected and the flash encrypted: today only images are signed and rollback refused. It comes with the TF-M chips above; on an ESP32-S3 it would take Espressif's own MCUboot ([what it takes](SECURITY.md#zephyr)).
- [ ] Apps against [Toit](https://toit.io) and against WAMR's AOT mode, on the same ESP32-C6 ([the benchmarks](COMPARISON.md#tied-to-one-chip-vendor)).
- [ ] A soak test: a fleet of boards left running for weeks. The collector is ready (`tools/soak/collect.py`: time not online, restarts and their causes, the heap's low-water mark, traffic per hour); not yet run.
