# Results

Six boards were tested with Flight Control 1.4.0. The firmware sizes are for `v3.15.0` on the five ESP-IDF boards (ESP-IDF 6.1.1) and `z32` on the Zephyr ESP32-S3 N8R8 (Zephyr 4.5.0). Both use WAMR 2.4.6.

## What works

| Capability | Verified coverage |
|---|---|
| Fleet management | Both ports enrolled over mTLS, reported status, applied config and ran signed WASM apps. Firmware updates preserved device identity. Certificate renewal was also verified on a physical C6. |
| Firmware updates | Both ports installed full and delta images from a TLS registry, confirmed the new firmware after reboot and rolled back a failed release. [Delta updates](DELTA-UPDATES.md#what-was-verified) |
| <a id="http2"></a>HTTP/2 | Spec and status shared a connection on all six boards. Registry logs confirmed HTTP/2 app and firmware downloads on classic ESP32, C5, C6 and S3 with ESP-IDF, and on S3 with Zephyr. |
| Wi-Fi provisioning | A phone provisioned the C6 with ESP-IDF and the S3 with Zephyr. After correcting a password, the C6 joined the network in about 2 s, closed its access point about 8 s later and rejoined after reset. [Provisioning](PROVISIONING.md) |
| Operations | Both ports delivered device logs, remote console output and OTLP metrics. Decommissioning wiped the device key and certificate in 2.4–3 s. The resident app kept running. |

## Firmware size

ESP-IDF OTA slots are 1,703,936 B each. The firmware was built for size, with WAMR at `-O2`.

| ESP-IDF target | Image (B) | OTA slot free |
|---|---:|---:|
| ESP32 | 1,178,496 | 30.8% |
| ESP32-C3 | 1,245,136 | 26.9% |
| ESP32-C5 | 1,339,696 | 21.4% |
| ESP32-C6 | 1,343,600 | 21.1% |
| ESP32-S3 N4R2 | 1,188,112 | 30.3% |

<a id="zephyr-on-the-esp32-s3"></a>On the **Zephyr S3 N8R8** with PSRAM, the signed image is **1,038,647 B**. It uses 376,800 of 380,672 B of internal DRAM at link time, leaving **3,872 B** for build changes. The linked image already includes the 120,112 B C heap and 94,208 B kernel heap. [Zephyr board configuration](ZEPHYR.md#on-an-esp32-s3)

For scale, an earlier speed-optimized C3 build used about 430 KiB of flash for Wi-Fi and networking, 197 KiB for cryptography and TLS, 68 KiB for WAMR and 62 KiB for the agent core. The apps’ C library occupied 7.7 KiB.

## CoreMark

<a id="scores-by-board"></a>The same two unmodified WASM modules ran as the sole app on each board, with metrics export off, on ESP-IDF firmware `v3.15-rc2` and Zephyr firmware `z32`. Scores are iterations per second on WAMR’s fast interpreter. Each score passed the CRC checks with at least 10 s timed. The column sizes are the contiguous linear memory each module needs. [Benchmark modules](APPS.md#modules-built-for-no-device)

| Board | 64 KiB | 128 KiB |
|---|---:|---:|
| ESP32, 240 MHz | 14.773 | Allocation failed (72 KiB largest block) |
| ESP32-C3, 160 MHz | 13.871 | Allocation failed (88 KiB largest block) |
| ESP32-C5, 240 MHz | 21.393 | Allocation failed (76 KiB largest block) |
| ESP32-C6, 160 MHz | 11.840 | 14.294 |
| ESP32-S3 N4R2, 2 MiB quad PSRAM | 17.586 | 21.081 |
| Zephyr ESP32-S3 N8R8, 8 MiB octal PSRAM | 17.864 | 21.637 |

<a id="memory-placement-on-c5-and-c6"></a>The 64 KiB scores for C5 and C6 are medians of six installs per board on a single boot. The ranges were 21.313–21.413 on C5 and 11.827–11.867 on C6. At 64 KiB, all five ESP-IDF boards scored within 5% of their `v3.14-rc12` baseline.

On an instrumented ESP-IDF C6, two continuous WASM apps ran together at normal priority. The longest measured interval between idle task executions was 0.50 s with a 5 s watchdog. [One-shot execution](APPS.md#modules-built-for-no-device)

A separate N8R8 build with PSRAM disabled reached 17.885 iterations/s at 64 KiB and had 26,400 B of C heap free after instantiation.

## RAM

On ESP-IDF an app must leave **40 KiB of free internal RAM** after its instance is created. Its linear memory also needs a contiguous block. For example, the 1.9 KiB `trend` module used 17.7 KiB of heap on C3, including an 8 KiB thread stack. App threads used 2.5–3.4 KiB of stack with `alarm` on ESP-IDF and 3.0 KiB on Zephyr S3. [App size and limits](APPS.md#size-and-limits)

**Telemetry.** With `alarm` running and metrics every 30 s on ESP-IDF `v3.14-rc12`, the agent used the following internal heap during the first minute after app startup. The figures exclude its temporary TLS reservation.

| ESP32 | C3 | C5 | C6 | S3 N4R2 |
|---:|---:|---:|---:|---:|
| 21–25 KiB | 24–28 KiB | 21–27 KiB | 21–27 KiB | 23–37 KiB |

On a C3, an established MQTT connection used about 10 KiB. An open console session used up to 25 KiB on ESP-IDF. On Zephyr N8R8 with PSRAM and metrics off, one fixed-size app left 67,932 B of C heap free (61,516 B contiguous). Two left 56,016 B free (38,284 B contiguous).

<a id="the-registry-with-tls-and-decommissioning-on-boards"></a>**Full firmware download over TLS.** Instrumented builds downloaded the signed `v3.15-rc1` (C3) and `z32` (N8R8) releases with `alarm` resident and metrics off. Minimum free heap covers manifest resolution through connection close. The largest free block was sampled during image writes, approximately every 16 KiB.

| Board and allocator | Free at start (B) | Minimum free (B) | Smallest sampled largest block during writes (B) |
|---|---:|---:|---:|
| ESP32-C3, internal heap | 144,552 | 42,980 | 106,496 |
| Zephyr S3 N8R8, C heap | 97,176 | 31,064 | 75,748 |

## Apps on the boards

| Workload | Board result |
|---|---|
| MQTT with TLS | All five ESP-IDF chips published every 5 s for 8 min and answered device and fleet commands. Zephyr S3 published every 10 s for 45 min and answered both prefixes within 1 s. N8R8 kept its broker session during a signed firmware download and exported telemetry with MQTT connected. |
| Arduino blink beside another app | The C++ build ran beside `alarm` on ESP32, C3, C6 and S3. The Rust build beside `tinyml` ran on C3, C6 and S3. Classic ESP32 reached the agent’s heap floor and C5 lacked a contiguous block. |
| TinyML | `tinyml` and `tinymlp` classified temperature on C3, C5, C6 and S3. Classic ESP32 has no temperature sensor. |
| Peripheral access | With no external peripherals attached, `hwcheck` exercised I²C, SPI, UART, ADC and PWM on physical ESP32, C5, C6 and S3 and on an emulated C3. |

<a id="in-espressifs-emulator"></a>**Simulated peripheral coverage** ([test configurations](TESTING.md))

| Target | Verified interfaces |
|---|---|
| <a id="esp32-s3-virtual-wiring"></a>ESP-IDF S3 | GPIO edges, I²C and SPI sensor reads, UART reception and 1–2 ms PWM pulses |
| <a id="esp32-c3-virtual-adc"></a>ESP-IDF C3 | ADC returned 1,640 and 1,962 mV for inputs of 1,650 and 1,980 mV |
| <a id="zephyr-esp32-s3-hardware-names"></a>Zephyr S3 | One WASM app read a button by its devicetree name and fleet alias |
| <a id="zephyr-rs-485"></a>Zephyr `native_sim` | RS-485 held driver enable through the last bit and received the peer’s three-byte reply |

## Timings

These are observed times on the local network. Fleet rollout times include scheduling and reboot.

| Operation | Observed time |
|---|---|
| Boot to enrollment request | About 10 s on ESP-IDF, including Wi-Fi, time sync and TLS |
| Config applied and confirmed | 1–6 s on ESP-IDF |
| Signed app running after config change | 3 s on ESP-IDF C3 and about 6 s on Zephyr S3, over TLS |
| Firmware and dependent app rollout | About 2 min for five ESP-IDF boards, canary first |
| <a id="the-way-back-on-the-esp32-s3-both-ports"></a>Failed firmware rollback | With a 30 s confirmation window, the S3 reported rollback 53 s after the fleet change on ESP-IDF and 1 min 53 s on Zephyr |
| Saved app startup while management is unreachable | About 180 s on an ESP-IDF S3. A fixed-size primary allowed the secondary to start at 189 s. A growing primary kept the secondary parked. |
| Five million interpreter instructions in a counting loop | C5 0.66 s, S3 0.90 s, C3 1.06 s, ESP32 1.20 s, C6 1.30 s |

## Idle traffic

HTTP/2 management traffic including TLS, extrapolated from 5½–6 min on a persistent connection with telemetry off. Status frequency and outstanding console requests affect the bandwidth.

| Board | Configuration | Traffic |
|---|---|---:|
| ESP-IDF C3, `v3.14-rc12` | `alarm`, 120 s heartbeat, empty console list | 153 kB/h |
| Same C3 | 11 outstanding console entries | 752 kB/h |
| Zephyr S3 N8R8 | No apps or console commands, status about once a minute | 537 kB/h |

<a id="a-crash-in-the-status"></a>

## Crash diagnosis

After reboot, `lastFatalError` identifies the fault and code addresses. Native dumps decoded against the release ELF measured 8,535 B on a physical Zephyr S3 with `alarm` and 11,200–14,080 B on an emulated ESP-IDF C3 with zero or two apps. [Download and decode a dump](CONSOLE.md#download-a-crash-dump)

## Current limitations

- **Zephyr without PSRAM.** An N8R8 running one app reached 368 B of free C heap during a failed update. In a separate test with MQTT connected, 32,012 B free fell below the 61,440 B telemetry threshold. Zephyr limits downloads to 64 KiB per WASM file. Its optional 40 KiB app heap floor is disabled by default and still needs hardware validation. [Zephyr configuration](ZEPHYR.md)
- **Management outages.** The agent restarts after 10 min without an accepted status. A separate port watchdog checks for 30 min without an accepted status or a received spec. A Zephyr S3 restarted after 1,826 s without a spec while status updates continued.
- **Credentials and registries.** Ordinary firmware stores the device key in flash unless a hardware key is configured. The registry needs the fleet’s CA and must serve downloads without cross-host redirects. Secure Boot and flash encryption have ESP-IDF emulator coverage. Registry bearer tokens have host-test coverage. [Device security](SECURITY.md) · [Registry](REGISTRY.md)
- **Certificate expiry.** Flight Control 1.4 cannot revoke a batch enrollment certificate. A device offline past its management certificate’s expiry (one year by default) enrolls under a new identity and keeps its firmware, config and apps. [Flight Control limitations](FLIGHTCTL-LIMITATIONS.md)
