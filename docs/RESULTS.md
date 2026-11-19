# Results

Six boards were tested with Flight Control 1.4.0. Firmware sizes are for release `v0.9.0`, built with ESP-IDF 6.1.1 or Zephyr 4.5.0 and WAMR 2.4.6. Named temperature probes and their notifications were tested on development firmware for v0.9.1. Delta, CoreMark, RAM, timing and traffic measurements come from earlier builds. [Firmware versions used for those measurements](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md)

## What works

| Capability | Verified coverage |
|---|---|
| Fleet management | Both ports enrolled over mTLS, reported status, applied config and ran signed WASM apps. Firmware updates preserved device identity. Certificate renewal was also verified on a physical C6. |
| Named temperature sensors | The same signed WASM alarm read `line_temp` on the ESP-IDF C6 and the Zephyr S3 N8R8. Both used their physical die probes. Both reported access denied without the grant, entered alarm at a 5 °C threshold, cleared at 120 °C and reported denial again after revocation. [Sensor mapping](APPS.md#what-an-app-may-use) |
| Sensor notifications | A signed WASM probe on the physical ESP-IDF C6 received a threshold interrupt with a 4–5 °C window, closed its subscription and received another interrupt after reopening. Revoking the grant returned `FC_EPERM`. The physical Zephyr S3 die driver returned `FC_ENOTSUP` for both triggers and the alarm continued sampling. Zephyr `native_sim` exercised data-ready and threshold callbacks with an emulated sensor driver. |
| Firmware and apps before 0.9.1 | All six physical boards installed the candidate firmware and ran the signed `alarm` and `battery` modules in turn, each with zero reported app restarts. The five boards with a die probe entered alarm at 5 °C. The classic ESP32 reported its missing temperature sensor. All six reported `battery unavailable` with no pack connected. All six returned to their v0.9.0 firmware and app. |
| Firmware updates | Both ports installed full and delta images from a TLS registry, confirmed the new firmware after reboot and rolled back a failed release. [Measured deltas](#firmware-deltas) |
| <a id="http2"></a>HTTP/2 | Spec and status shared a connection on all six boards. Registry logs confirmed HTTP/2 app and firmware downloads on classic ESP32, C5, C6 and S3 with ESP-IDF, and on S3 with Zephyr. |
| Wi-Fi provisioning | A phone provisioned the C6 with ESP-IDF and the S3 with Zephyr. After a password retry, C6 joined in about 2 s, closed its AP about 8 s later and rejoined after reset. The Zephyr S3 joined in 8 s and was online with its app running 1 s after its AP closed. [Provisioning](PROVISIONING.md) |
| Operations | Both ports delivered device logs, remote console output and OTLP metrics. Decommissioning wiped the device key and certificate in 2.4–3 s. The resident app kept running. |
| Crash dumps | A physical Zephyr S3 with `alarm` produced an 8,535 B dump. Emulated ESP-IDF C3 dumps measured 11,200–14,080 B with zero or two apps. Both were retrieved and decoded. [Download and decode a dump](CONSOLE.md#download-a-crash-dump) |

## Persistent samples

A WAMR app using the in-memory test port filled a queue named `samples` with 680 four-byte readings. The next push returned `FC_ENOSPC`. Reinitializing the runtime from the saved catalog recovered all 680 readings in insertion order. A delayed acknowledgement left the next head intact. The MQTT consumer test received the same ID twice when it withheld its receipt, then received the next sample after confirming the first.

| Operation on the 680 samples | Successful writes at the port KV interface | Record bytes submitted |
|---|---:|---:|
| Fill the queue | 680 | 1,401,480 |
| Peek and acknowledge every head | 680 | 1,397,400 |
| Peek, reject a stale ID or reject a full queue | 0 | 0 |

On all six physical boards, a signed test module stored three known samples on the candidate firmware. After a device reboot it recovered and acknowledged all three in order.

The complete record ranged from 18 B when empty to 4,098 B when full, including the three-byte KV entry header outside the quota. Filesystem metadata, wear leveling and flash erase cycles depend on the port. [Queue API and delivery protocol](APPS.md#persistent-sample-queues)

## Battery telemetry

The `v0.9.1-rc.battery1` images used the same `battery` module (1,466 B) across ESP32, C3, C5, C6 and S3 N4R2 with ESP-IDF, and S3 N8R8 with Zephyr. The companion `alarm` module was 2,926 B. [Sensor API and fleet configuration](APPS.md#battery-channels)

On an emulated ESP32-C3 running the same ESP-IDF image and signed app, Flight Control's Telemetry Gateway exported the following gauges. The fleet set `low_mv` to 3,600. For the ADC test, esp-emu 1.1.0 drove GPIO3 with its potentiometer and the board map used a 2:1 divider. The readings include the ESP-IDF ADC calibration. [Espressif virtual parts](https://github.com/espressif/esp-emulator/blob/v1.1.0/docs/guides/PARTS.md)

| Input | `voltage_mv` | `low_battery` | `charge_pct` | `charging` |
|---|---:|---:|---:|---:|
| ADC at 1,650 mV | 3,280 | 1 | — | — |
| ADC raised to 1,980 mV | 3,924 | 0 | — | — |
| ADC returned to 1,650 mV | 3,280 | 1 | — | — |
| I²C register file at 0x0b, 3,700 mV and +14 mA | 3,700 | 0 | 62 | 1 |

The I²C fixture used SDA on GPIO5 and SCL on GPIO6. The emulator option was `--i2c-dev 0:0x0b,width=1,fill=0,reg=0x09=0x74,reg=0x0a=0x0e,reg=0x0d=0x3e`. Pack voltage accuracy and charger behavior still need verification on wired hardware.

In host WAMR tests, a 1,234 mV ADC input and a 5:3 divider produced 2,057 mV. At exactly the configured threshold, the app emitted `low_battery=0`. It skipped that gauge when the fleet omitted the threshold. An emulated gauge reported 62% relative charge, rejected 101%, and distinguished +200 mA charging current from idle and −200 mA discharge.

## Firmware size

ESP-IDF OTA slots are 1,703,936 B each. The firmware was built for size, with WAMR at `-O2`.

| ESP-IDF target | Image (B) | OTA slot free |
|---|---:|---:|
| ESP32 | 1,178,496 | 30.8% |
| ESP32-C3 | 1,245,136 | 26.9% |
| ESP32-C5 | 1,339,696 | 21.4% |
| ESP32-C6 | 1,343,600 | 21.1% |
| ESP32-S3 N4R2 | 1,188,112 | 30.3% |

<a id="zephyr-on-the-esp32-s3"></a>On the **Zephyr S3 N8R8** with PSRAM, the signed image is **1,038,663 B**. It uses 376,800 of 380,672 B of internal DRAM at link time, leaving **3,872 B** for build changes. The linked image already includes the 120,112 B C heap and 94,208 B kernel heap. [Zephyr board configuration](ZEPHYR.md#on-an-esp32-s3)

The Zephyr S3 development build with the named temperature probe uses 377,008 B of internal DRAM, leaving 3,664 B at link time. Its signed image is 1,039,959 B.

MQTT added 12.9 KB flash on C3 and 12.7 KB on Zephyr S3. Field provisioning added 17.4 KB flash and 16 B static RAM to an ESP-IDF 6.0.3 S3 debug build.

## Firmware deltas

| Update | Target | Download |
|---|---|---|
| Release with substantial code changes | ESP-IDF on ESP32, C5, C6 and S3 | 12–15% of the full image |
| Fix release | Same boards | 2–3% |
| Debug to release build | ESP-IDF S3 | 511 KB (about half the image), 13 s |
| Successive Zephyr builds | Zephyr S3 | 19 KB (2.2%), 8 s |

Each board reported `Applied` in `status.os.lastDelta.outcome`. C3 delta coverage was emulated. [Publish and inspect a delta](DELTA-UPDATES.md)

## CoreMark

<a id="scores-by-board"></a>The same two unmodified WASM modules ran as the sole app on each board, with metrics export off. Scores are iterations per second on WAMR’s fast interpreter. Each score passed the CRC checks with at least 10 s timed. The column sizes are the contiguous linear memory each module needs. [Benchmark modules](APPS.md#modules-built-for-no-device)

| Board | 64 KiB | 128 KiB |
|---|---:|---:|
| ESP32, 240 MHz | 14.773 | Allocation failed (72 KiB largest block) |
| ESP32-C3, 160 MHz | 13.871 | Allocation failed (88 KiB largest block) |
| ESP32-C5, 240 MHz | 21.393 | Allocation failed (76 KiB largest block) |
| ESP32-C6, 160 MHz | 11.840 | 14.294 |
| ESP32-S3 N4R2, 2 MiB quad PSRAM | 17.586 | 21.081 |
| Zephyr ESP32-S3 N8R8, 8 MiB octal PSRAM | 17.864 | 21.637 |

<a id="memory-placement-on-c5-and-c6"></a>The 64 KiB scores for C5 and C6 are medians of six installs per board on a single boot. The ranges were 21.313–21.413 on C5 and 11.827–11.867 on C6.

On an instrumented ESP-IDF C6, two continuous WASM apps ran together at normal priority. The longest measured interval between idle task executions was 0.50 s with a 5 s watchdog. [One-shot execution](APPS.md#modules-built-for-no-device)

A separate N8R8 build with PSRAM disabled reached 17.885 iterations/s at 64 KiB and had 26,400 B of C heap free after instantiation.

## RAM

On ESP-IDF, the 1.9 KiB `trend` module used 17.7 KiB of heap on C3, including an 8 KiB thread stack. App threads used 2.5–3.4 KiB of stack with `alarm` on ESP-IDF and 3.0 KiB on Zephyr S3. [App size and limits](APPS.md#size-and-limits)

**Telemetry.** With `alarm` running and metrics every 30 s on ESP-IDF, the agent used the following internal heap during the first minute after app startup. The figures exclude its temporary TLS reservation.

| ESP32 | C3 | C5 | C6 | S3 N4R2 |
|---:|---:|---:|---:|---:|
| 21–25 KiB | 24–28 KiB | 21–27 KiB | 21–27 KiB | 23–37 KiB |

On a C3, an established MQTT connection used about 10 KiB. An open console session used up to 25 KiB on ESP-IDF. Closing one on an instrumented C6 reclaimed 8,304 B when its thread was joined.

On Zephyr N8R8 with PSRAM and metrics off, one fixed-size app left 67,932 B of C heap free (61,516 B contiguous). Two left 56,016 B free (38,284 B contiguous).

<a id="the-registry-with-tls-and-decommissioning-on-boards"></a>**Full firmware download over TLS.** Instrumented C3 and N8R8 builds downloaded signed firmware with `alarm` resident and metrics off. Minimum free heap covers manifest resolution through connection close. The largest free block was sampled during image writes, approximately every 16 KiB.

| Board and allocator | Free at start (B) | Minimum free (B) | Smallest sampled largest block during writes (B) |
|---|---:|---:|---:|
| ESP32-C3, internal heap | 144,552 | 42,980 | 106,496 |
| Zephyr S3 N8R8, C heap | 97,176 | 31,064 | 75,748 |

With PSRAM disabled, a Zephyr S3 N8R8 running one app had only **368 B** of free C heap during a failed firmware update. In a separate run with MQTT connected, it had 32,012 B free and skipped telemetry because the export requires 61,440 B. [Zephyr board configuration](ZEPHYR.md#on-an-esp32-s3)

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
| ESP-IDF C3 | `alarm`, 120 s heartbeat, empty console list | 153 kB/h |
| Same C3 | 11 outstanding console entries | 752 kB/h |
| Zephyr S3 N8R8 | No apps or console commands, status about once a minute | 537 kB/h |
