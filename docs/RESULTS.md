# Results

## Firmware size

The measured firmware uses ESP-IDF 6.1.1 or Zephyr 4.5.0, both with WAMR 2.4.6.
Each ESP-IDF OTA slot holds 1,703,936 B. The ESP-IDF builds optimize for size and compile WAMR with `-O2`.

| ESP-IDF target | Image (B) | OTA slot free |
|---|---:|---:|
| ESP32 | 1,193,136 | 30.0% |
| ESP32-C3 | 1,265,712 | 25.7% |
| ESP32-C5 | 1,362,928 | 20.0% |
| ESP32-C6 | 1,365,904 | 19.8% |
| ESP32-S3 N4R2 | 1,202,896 | 29.4% |

On the Zephyr S3 N8R8 with PSRAM, the signed image is 1,041,735 B. Only **3,456 B of internal DRAM** remain at link time for additional drivers and other build changes. The image already uses 377,216 of 380,672 B, including the 120,112 B C heap and the 94,208 B kernel heap. [Zephyr board configuration](ZEPHYR.md#on-an-esp32-s3)

## RAM

A 1.9 KiB `trend` module used 17.7 KiB of heap on ESP-IDF C3, including an 8 KiB thread stack. An established MQTT connection used about 10 KiB on C3. An open console used up to 25 KiB on ESP-IDF. [App sizing and heap reserve](APPS.md#size-and-limits)

These ESP-IDF measurements cover internal heap usage in the first minute after `alarm` started, with metrics every 30 s. They exclude the temporary TLS reservation. [Original heap measurements](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#ram)

| ESP32 | C3 | C5 | C6 | S3 N4R2 |
|---:|---:|---:|---:|---:|
| 21–25 KiB | 24–28 KiB | 21–27 KiB | 21–27 KiB | 23–37 KiB |

### Full firmware download over TLS

The C3 and the Zephyr S3 each downloaded a full signed image with `alarm` resident and metrics off.
The minimum free heap covers manifest lookup through connection close.
During image writes, the agent also sampled the largest free block about every 16 KiB. The last column gives its lowest reading. [Original download measurements](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#ram)

| Board and allocator | Free at start (B) | Minimum free (B) | Contiguous block during writes (B) |
|---|---:|---:|---:|
| ESP32-C3, internal heap | 144,552 | 42,980 | 106,496 |
| Zephyr S3 N8R8, C heap | 97,176 | 31,064 | 75,748 |

Without PSRAM, a Zephyr S3 N8R8 running one app had only 368 B of free C heap during a failed firmware update.
With MQTT connected, a separate measurement found 32,012 B free, below the 61,440 B needed for telemetry. [Zephyr memory configuration](ZEPHYR.md#on-an-esp32-s3)

### Two apps exchanging local readings

On a development build, the boards ran the same two apps without an MQTT grant. `sampler` (1,581 B) sent die temperature readings to `localalarm` (1,049 B) on the five boards with a probe. On the classic ESP32, the receiver waited for input. [Peer permissions and overflow](APPS.md#pass-readings-between-local-apps)

| Physical board | Free with both apps (B) | Heap used by the pair (B) |
|---|---:|---:|
| ESP32, ESP-IDF | 81,672 | 37,460 |
| ESP32-C3, ESP-IDF | 101,376 | 36,944 |
| ESP32-C5, ESP-IDF | 77,592 | 40,424 |
| ESP32-C6, ESP-IDF | 173,108 | 40,716 |
| ESP32-S3 N4R2, ESP-IDF | 2,216,104 | 38,896 |
| ESP32-S3 N8R8, Zephyr | 71,440 | 20,420 |

Heap used by the pair is the drop in free heap after both apps started. The console stayed open throughout. ESP-IDF counts all free heap, including available PSRAM. Zephyr counts only the C heap.

## Firmware deltas

All five ESP-IDF boards and the Zephyr S3 applied a signed delta from the preceding lab release. Downloads ranged from 82,957 to 104,703 B (6.6–8.7% of the full image for each board). All six kept their device identities after reboot. Earlier ESP-IDF deltas on ESP32, C5, C6 and S3 ranged from 2–3% for fixes to 12–15% for larger changes. [Delta sizes and download times](DELTA-UPDATES.md#what-was-verified)

## CoreMark

All six boards ran the module with 64 KiB of contiguous linear memory. Only the C6 and the two S3s could allocate 128 KiB. Each score is the median of three fresh module installs on the measured firmware, in iterations per second, with one module running and metrics export off. Failed allocations show the largest free block. [Benchmark modules](APPS.md#standalone-modules)

| Board | 64 KiB | 128 KiB |
|---|---:|---:|
| ESP32, 240 MHz | 18.751 | Failed (76 KiB) |
| ESP32-C3, 160 MHz | 15.229 | Failed (88 KiB) |
| ESP32-C5, 240 MHz | 24.343 | Failed (64 KiB) |
| ESP32-C6, 160 MHz | 14.597 | 18.600 |
| ESP32-S3 N4R2, 2 MiB quad PSRAM | 22.862 | 28.912 |
| Zephyr ESP32-S3 N8R8, 8 MiB octal PSRAM | 20.443 | 25.633 |

WAMR uses 16-bit handler offsets and dispatches frequent instruction pairs directly. The ESP-IDF S3 keeps its 16 KiB instruction cache. [Interpreter patch](../third_party/patches/wamr-wasm32-compact-dispatch.patch)

In an earlier build with PSRAM disabled, the Zephyr S3 N8R8 ran the 64 KiB module at 17.885 iterations/s and had 26,400 B of C heap free after instantiation.

## Persistent samples

With the app’s full 4,096 B KV quota, a queue named `samples` held 680 four-byte readings. Each push or successful acknowledgement rewrote the record. Filling the queue wrote about 1.4 MB to the in-memory port’s KV interface. Draining it wrote another 1.4 MB. Actual flash wear depends on the filesystem and wear leveling. In a separate test, all six physical boards recovered three queued readings in order after reboot. [Queue API and acknowledgements](APPS.md#persistent-sample-queues)

## Apps on the boards

| Workload | Board results |
|---|---|
| Temperature alarms | Five boards read their die probe. The classic ESP32 needs an external source. On C6 and Zephyr S3 the same alarm could read `line_temp` only while its grant was active. The C6 probe delivered threshold interrupts. The Zephyr S3 driver returned `FC_ENOTSUP`, so the alarm polled. |
| MQTT over TLS | All five ESP-IDF targets sustained a 5 s publish interval for 8 min. Zephyr S3 sustained a 10 s interval for 45 min, answered commands within 1 s and kept its broker connection during a signed firmware download. |
| Arduino blink alongside another app | C++ blink ran with `alarm` on ESP32, C3, C6 and S3. Rust blink ran with `tinyml` on C3, C6 and S3. For the latter pair, ESP32 reached the agent’s heap reserve and C5 lacked a contiguous block. |
| TinyML | `tinyml` and `tinymlp` classified temperature on ESP-IDF C3, C5, C6 and S3. |
| Peripheral access | `hwcheck` exercised I²C, SPI, UART, ADC and PWM on physical ESP32, C5, C6 and S3 boards without external peripherals, and on an emulated C3. [Peripheral tests](TESTING.md#peripheral-tests) |
| Battery telemetry | An emulated C3 reported the low-voltage threshold crossing, charge level and charging state. The six physical boards reported `battery unavailable` with no pack attached. Voltage accuracy and charger behavior still need a wired-pack test. [Battery channels](APPS.md#battery-channels) |

## Timings

These times were measured on the local network. Fleet rollout times include scheduling and reboot.

| Operation | Observed time |
|---|---|
| Boot to enrollment request | About 10 s on ESP-IDF, including Wi-Fi, time sync and TLS |
| Wi-Fi provisioning from a phone | ESP-IDF C6 joined in about 2 s and closed its access point about 8 s later. Zephyr S3 joined in 8 s and had its app running 1 s after its access point closed. |
| Config applied and confirmed | 1–6 s on ESP-IDF |
| Signed app running after config change | 3 s on ESP-IDF C3 and about 6 s on Zephyr S3, over TLS |
| Firmware and dependent app rollout | About 2 min for five ESP-IDF boards, canary first |
| Rollback after a failed firmware update | With a 30 s confirmation window, the S3 reported rollback 53 s after the fleet change on ESP-IDF and 1 min 53 s on Zephyr |
| Saved app startup while management is unreachable | About 180 s on an ESP-IDF S3. A fixed-size primary allowed the secondary to start at 189 s. A growing primary kept the secondary parked. |
| Decommissioning | Both ports erased the device key and certificate in 2.4–3 s. The resident app kept running. |

## Idle traffic

Each board used one HTTP/2 connection for both spec and status. The hourly rates below come from 5½–6 min of management traffic, including TLS, with telemetry off. [Original traffic measurements](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#idle-traffic)

| Board | Configuration | Traffic |
|---|---|---:|
| ESP32-C3 (ESP-IDF) | `alarm` running, 120 s heartbeat, no remote console sessions listed | 153 kB/h |
| ESP32-C3 (ESP-IDF) | `alarm` running, 11 remote console sessions listed | 752 kB/h |
| Zephyr S3 N8R8 | No apps or console commands, status about once a minute | 537 kB/h |

Flight Control lists each remote console request in the device spec (`spec.consoles`). A request can stay there after the operator leaves. With 11 requests still listed for the C3, Flight Control returned the spec immediately on each poll, increasing management traffic. [Console session lifetime](FLIGHTCTL-LIMITATIONS.md#15-a-console-session-whose-user-left-before-the-device-came-stays-listed)
