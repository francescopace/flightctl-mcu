# Results

## App benchmarks

ESP32, C3, C5, C6 and S3 Mini (N4R2) run ESP-IDF. The Zephyr S3 is an N8R8. Times are medians unless marked otherwise.

| Workload and measure | ESP32 | C3 | C5 | C6 | S3 Mini | Zephyr S3 |
|---|---:|---:|---:|---:|---:|---:|
| `tinyml` forest inference (ms) | 0.15 | 0.19 | 0.09 | 0.16 | 0.06 | 0.07 |
| `tinymlp` neural network inference (ms) | 0.84 | 4.34 | 1.38 | 2.97 | 0.75 | 1.50 |
| `trend` temperature sample (ms) | No die sensor | 0.18 | 0.07 | 0.10 | 0.37 | 0.05 |
| `battery` poll without a pack (ms) | 0.41 | 0.09 | 0.06 | 0.05 | 0.07 | 0.04 |
| `alarm` full timer callback (µs) | Timer off | 514.0 | 442.0 | 382.5 | 502.0 | 434.0 |
| `battery` full timer callback without a pack (µs) | 1,049.0 | — | — | — | — | — |
| `mqttdemo` command to acknowledgement over TLS (ms) | 127.2 | 121.9 | 120.3 | 120.0 | 122.7 | 183.5 |
| Same MQTT round trip, maximum (ms, rounded) | 156 | 190 | 184 | 185 | 184 | 288 |

Inference includes feature extraction and prediction on identical inputs. Sampling includes sensor reads. Full callbacks also include entry into WASM. Zephyr simulates the legacy `die_temp` input. A dash means unmeasured. [Workloads and timing methods](TESTING.md#app-benchmark-methods)

Direct WAMR stack lookup reduced elapsed callback time by 21.1–43.4% on ESP-IDF with identical linked IRAM and static data sizes. CoreMark showed no repeatable gain. [Stack-scan comparison and S3 Mini profile](TESTING.md#wamr-callback-measurements)

### CoreMark

Median iterations/s over three successful installs of the same signed module, one app running, metrics export off. Column sizes are contiguous linear memory. Failed allocations show the largest free block. [Benchmark modules](APPS.md#standalone-modules)

| Board | 64 KiB | 128 KiB |
|---|---:|---:|
| ESP32, 240 MHz | 19.129 | Failed (76 KiB) |
| C3, 160 MHz | 15.271 | Failed (88 KiB) |
| C5, 240 MHz | 24.374 | Failed (64 KiB) |
| C6, 160 MHz | 14.591 | 18.654 |
| S3 Mini, 2 MiB quad PSRAM | 22.924 | 28.941 |
| Zephyr S3, 8 MiB octal PSRAM | 20.423 | 25.648 |
| Zephyr S3, PSRAM disabled | 20.429 | — |

With PSRAM disabled, the instrumented Zephyr build had 30,896 B of C heap free after the final instantiation. WAMR uses 16-bit handler offsets and direct dispatch for frequent instruction pairs. The S3 Mini instruction cache is 16 KiB. [Interpreter patch](../third_party/patches/wamr-wasm32-compact-dispatch.patch)

## Memory and compatibility

### Firmware size

ESP-IDF 6.1.1 and Zephyr 4.5.0 use WAMR 2.4.6. ESP-IDF builds optimize for size (WAMR at `-O2`) with 1,703,936 B per OTA slot.

| Board | Image (B) | OTA slot free |
|---|---:|---:|
| ESP32 | 1,193,136 | 30.0% |
| C3 | 1,265,712 | 25.7% |
| C5 | 1,362,928 | 20.0% |
| C6 | 1,365,904 | 19.8% |
| S3 Mini | 1,202,896 | 29.4% |
| Zephyr S3 with PSRAM, signed | 1,041,735 | — |

Zephyr S3 has **3,456 B internal DRAM free at link time**. The build uses 377,216 of 380,672 B, including 120,112 B for the C heap and 94,208 B for the kernel heap. [Zephyr board configuration](ZEPHYR.md#on-an-esp32-s3)

### RAM

| ESP-IDF C3 workload | Additional heap used |
|---|---:|
| Run `trend` (1,617 B module) | 18.6 KiB including the 8 KiB thread stack |
| Configure broker and connect over TLS with `mqttdemo` resident | 24.2 KiB (116,564 → 91,744 B free) |
| Open console with `alarm` running | 17.4 KiB (129,760 → 111,936 B internal heap free) |

| Internal heap free with `alarm` (B) | ESP32 | C3 | C5 | C6 | S3 Mini |
|---|---:|---:|---:|---:|---:|
| Minimum | 18,704 | 35,788 | 107,544 | 23,992 | 130,284 |
| Maximum | 113,608 | 133,476 | 108,720 | 204,520 | 153,020 |

The diagnostic build sampled byte-addressable internal heap every second for 60 s after app initialization, including active TLS allocations, with metrics every 30 s. [Earlier measurements excluding the TLS reservation](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#ram)

`tinyml` uses 1.2 KB of linear memory (4,121 B module) and `tinymlp` uses 2.4 KB (6,774 B module). The `samples` queue holds 680 four-byte readings at the full 4,096 B per-app quota. [App sizing](APPS.md#size-and-limits) · [Queue writes and recovery](APPS.md#persistent-sample-queues)

### App combinations

| Signed apps running, console heap reading (B) | ESP32 | C3 | C5 | C6 | S3 Mini | Zephyr S3 |
|---|---:|---:|---:|---:|---:|---:|
| Blink alone, Rust · free | 39,900 | 57,948 | 32,424 | 129,144 | 2,170,244 | 85,932 |
| Blink alone, C++ · free | 38,100 | 57,728 | 32,464 | 128,976 | 2,170,072 | 85,656 |
| Blink alone, AssemblyScript · free | 30,584 | 50,224 | 26,632 | 121,432 | 2,164,308 | 78,768 |
| Blink alone, TinyGo · free | 26,060 | 38,356 | 18,584 | 115,360 | 2,156,252 | 72,852 |
| `sampler` + `localalarm` · free | 81,224 | 101,048 | 76,188 | 170,136 | 2,214,664 | 72,152 |
| Same pair · drop in free heap | 37,640 | 37,284 | 38,876 | 39,188 | 37,768 | 19,652 |

The drop is relative to the console reading before either app started. ESP-IDF includes available PSRAM. Zephyr reports the C heap.

C++ blink ran alongside `alarm` on ESP32, C3, C6 and both S3s. Rust blink ran alongside `tinyml` on C3, C6 and S3 Mini. Classic ESP32 had room for a 2,488 B layer with blink resident. C5 had a 62 KiB contiguous block when Rust blink needed 64 KiB.

`sampler` (1,581 B) and `localalarm` (1,049 B) exchanged die temperature without an MQTT grant on five boards. Classic ESP32 needs an external source and the receiver waited for input. [Local app permissions](APPS.md#pass-readings-between-local-apps) · [Hardware checks](TESTING.md#catalog-app-checks)

On esp-emu 1.2.0 with a 2:1 divider, the signed `battery` app reported 3,280 and 3,924 mV on ESP32-C3 and 3,300 and 3,960 mV on ESP32-S3 for inputs of 1,650 and 1,980 mV. Both reported 3,700 mV, 62% charge and charging from the simulated Smart Battery gauge. Battery accuracy and charging with a physical pack remain unverified. [Battery telemetry tests](TESTING.md#end-to-end-tests)

### Full firmware download over TLS

| Board and allocator, signed image with `alarm` resident | Free at start (B) | Minimum free (B) | Lowest reading of largest free block during writes (B) |
|---|---:|---:|---:|
| C3, internal heap | 136,588 | 46,812 | 94,208 |
| Zephyr S3, C heap | 97,184 | 30,140 | 76,676 |
| Zephyr S3 without PSRAM, C heap | 94,184 | 41,260 | — |

Metrics off. Minimum free heap covers manifest lookup through connection close. The largest block was sampled about every 16 KiB written. Without PSRAM, Zephyr S3 completed the update. A separate console reading with `mqttdemo` connected showed 46,184 B free against a default telemetry export threshold of 61,440 B. [Download measurements](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#ram) · [Zephyr memory configuration](ZEPHYR.md#on-an-esp32-s3)

## Operational timings and traffic

### Timings

Local network. Rollouts include scheduling and reboot.

| Operation | Physical boards | Emulation |
|---|---|---|
| Boot to enrollment request visible in API | — | ESP32-S3, 7.3–10.2 s over three clean starts. Zephyr `native_sim`, 0.6 s on its simulated network. |
| Wi-Fi provisioning | In earlier phone tests, C6 joined in ~2 s and closed its access point ~8 s later. Zephyr S3 joined in 8 s and ran its app 1 s after access point closure. | C3 installer, 6.6 s. Zephyr radio simulation passed immediate and deferred joins. |
| Alarm threshold change to confirmed state | C6, 4.4 s. Zephyr S3, 6.3 s. | — |
| Config change to signed `battery` app confirmed running over TLS | C3, 9.0 s. Zephyr S3, 9.9 s. | — |
| Firmware and dependent app rollout | Five ESP-IDF boards in 10 min 2 s, including 9 min 9 s for the canary during an API interruption and 53 s for the other four. Independently scheduled Zephyr S3, 50 s. | — |
| Fleet change to confirmed rollback | S3 Mini, 66.7 s. Zephyr S3, 118.8 s. Both restored the signed stable image with a 30 s confirmation window. | — |
| Cached app startup with management offline | — | ESP-IDF S3, fixed-size primary at 181.0 s after boot and secondary at 182.7 s. Expandable primary at 181.2 s, secondary still parked 45 s later. |
| Decommissioning | In earlier tests both ports erased the device key and certificate in 2.4–3 s with the resident app still running. | C3 wipe began at 3.6 s, unenrolled status reported after reboot at 7.2 s. Zephyr `native_sim` reported both events by 6.1 s. |

### Firmware deltas

All six boards retained their identities after signed delta updates of 10,816–14,114 B (1.03–1.04% of the full image). Other ESP-IDF deltas on ESP32, C5, C6 and S3 ranged from 2–3% for fixes to 12–15% for larger changes. [Delta sizes and download times](DELTA-UPDATES.md#what-was-verified)

### Idle traffic

| Board | Workload | Listed console requests | Management traffic over TLS |
|---|---|---:|---:|
| Emulated C3 | `alarm`, 120 s heartbeat | 0 | 216 kB/h |
| Physical C3 | `alarm`, 120 s heartbeat | 19 | 351 kB/h |
| Zephyr S3 | No apps or console commands during sampling | 16 | 563 kB/h |
| ESP-IDF C3 | `alarm`, device spec returned immediately on each poll | 11 | 752 kB/h |

Rates include sent and received bytes with telemetry off. The first three use 360 s of device counters each. Console requests can remain in `spec.consoles` after disconnect. [Traffic measurements](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#idle-traffic) · [Console session lifetime](FLIGHTCTL-LIMITATIONS.md#15-a-console-session-whose-user-left-before-the-device-came-stays-listed)
