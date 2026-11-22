# App benchmarks

[Same board, three ports](#esp32-s3-n8r8-comparison-on-one-board) · [CoreMark, Richards and DeltaBlue](#coremark-richards-and-deltablue) · [Catalog apps](#catalog-apps) · [WAMR and Toit](WAMR.md#comparison-with-toit)

Every table uses these protocols and names. [Inputs, checks and timing methods](TESTING.md#app-benchmark-methods)

| Measure | Unit | Protocol |
|---|---|---|
| CoreMark | iterations/s, higher is faster | Median of three fresh installations of the signed module. The 64 KiB module is the wasm3 build. |
| Richards, DeltaBlue | ms/iteration, lower is faster | Median of three batches of ten checked iterations. |
| Timing probes (forest, neural, `trend`, `battery`) | µs/operation, lower is faster | Each probe alone. Median of seven batches after two warm-ups, divided by 256 operations (32 for the neural network). The device clock resolves 1 ms per batch, so values are given to the nearest 10 µs. |

- **S3 Mini** is the ESP32-S3 Mini N4R2 (4 MiB flash, 2 MiB quad PSRAM). **N8R8** is the ESP32-S3 DevKitC with 8 MiB flash and 8 MiB octal PSRAM.
- **Forest** and **neural** are the `tinyml` and `tinymlp` inference probes. They include feature extraction and prediction on identical inputs. The `trend` and `battery` probes include the sensor read.
- **`trend256`** is the probe that calls the `trend` sample function 256 times per batch.
- A dash means not measured or not recorded.

## ESP32-S3 N8R8 comparison on one board

One N8R8 ran agent v0.9.4 and fetched the same signed modules by digest. ESP-IDF 6.1.1, Zephyr 4.5.0 and NuttX 13.1.0 used one 240 MHz core, 80 MHz flash and 40 MHz octal PSRAM. The two-core row uses the standard dual-core ESP-IDF build. `trend` read the physical on-chip sensor, so its time includes the driver and input-dependent work. [Fixed-input test](#temperature-with-matched-input)

| Port | CPU cores | Forest (µs) | Neural (µs) | `trend` sample (µs) | `battery` poll, no pack (µs) |
|---|---:|---:|---:|---:|---:|
| ESP-IDF | 1 | 100 | 1,880 | 210 | 30 |
| Zephyr | 1 | 70 | 880 | 40 | 40 |
| NuttX | 1 | 70 | 840 | 1,310 | 30 |
| ESP-IDF | 2 | 60 | 1,880 | 420 | 90 |

<details>
<summary>Individual batches (ms per batch)</summary>

| Probe | Port | CPU cores | Batch 1 | Batch 2 | Batch 3 | Batch 4 | Batch 5 | Batch 6 | Batch 7 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Forest | ESP-IDF | 1 | 25 | 26 | 25 | 25 | 25 | 25 | 25 |
| Forest | Zephyr | 1 | 18 | 18 | 18 | 18 | 18 | 18 | 18 |
| Forest | NuttX | 1 | 20 | 19 | 19 | 18 | 19 | 18 | 18 |
| Forest | ESP-IDF | 2 | 20 | 16 | 15 | 15 | 15 | 16 | 16 |
| Neural | ESP-IDF | 1 | 60 | 46 | 35 | 62 | 67 | 45 | 61 |
| Neural | Zephyr | 1 | 28 | 28 | 28 | 28 | 27 | 28 | 28 |
| Neural | NuttX | 1 | 29 | 27 | 34 | 27 | 27 | 27 | 30 |
| Neural | ESP-IDF | 2 | 61 | 54 | 64 | 60 | 54 | 48 | 63 |
| `trend` | ESP-IDF | 1 | 28 | 53 | 40 | 125 | 40 | 60 | 72 |
| `trend` | Zephyr | 1 | 422 | 88 | 16 | 8 | 9 | 9 | 9 |
| `trend` | NuttX | 1 | 384 | 336 | 319 | 359 | 331 | 317 | 335 |
| `trend` | ESP-IDF | 2 | 101 | 199 | 107 | 104 | 146 | 57 | 155 |
| `battery` | ESP-IDF | 1 | 8 | 8 | 8 | 12 | 9 | 8 | 8 |
| `battery` | Zephyr | 1 | 8 | 9 | 9 | 9 | 9 | 9 | 8 |
| `battery` | NuttX | 1 | 8 | 8 | 8 | 8 | 8 | 8 | 8 |
| `battery` | ESP-IDF | 2 | 30 | 18 | 22 | 18 | 28 | 28 | 15 |

</details>

### Temperature with matched input

The same N8R8 read its physical sensor, then supplied the same 40/41 °C sequence to `trend256`. Each batch contained 256 reads with buffered state logs and the Wi-Fi agent active. Elapsed time includes preemption. ESP-IDF and Zephyr ran seven batches on each of two boots, NuttX on four boots, with tracing off.

| Port | Round-robin slice (ms) | Boots | Median of batch averages (µs/sample) | Mean across batches (µs/sample) | Batches above 1,000 µs/sample | Batches measured |
|---|---:|---:|---:|---:|---:|---:|
| Zephyr 4.5.0 | — | 2 | 348.7 | 405.6 | 0 | 14 |
| NuttX 13.1.0 | 20 | 4 | 497.4 | 659.1 | 3 | 28 |
| ESP-IDF 6.1.1 | — | 2 | 847.1 | 858.6 | 0 | 14 |

[NuttX scheduler comparison](NUTTX.md#temperature-and-scheduling) · [Zephyr log buffering](ZEPHYR.md#state-logging) · [What the timings include](TESTING.md#separating-temperature-acquisition-from-the-app) · [Zephyr and ESP-IDF batches](benchmarks/esp32s3-trend-buffered-three-ports.json) · [NuttX batches](benchmarks/esp32s3-nuttx-trend-20ms-untraced.json)

<details>
<summary>Agent v0.9.4 diagnostic builds: sensor read, rest of the sample and serial state output</summary>

S3 N8R8, one 240 MHz core. All values are medians of batch averages in µs/sample. The fixed 40/41 °C trace triggers 120 state calls and 32 stdout writes per 256 samples.

Serial state output toggled per port: state logs on the serial console and in the log buffer, against the log buffer only. Each port read the physical sensor and received the fixed trace. The NuttX runs used a 200 ms slice. [Batch counters](benchmarks/esp32s3-trend-buffered-three-ports.json)

| Port | State log output | Physical sensor (µs) | Rest of `trend` (µs) | Complete sample (µs) | Change in complete sample (%) |
|---|---|---:|---:|---:|---:|
| ESP-IDF 6.1.1 | Serial and buffer | — | — | 1,069.4 | — |
| ESP-IDF 6.1.1 | Buffer only | 113.6 | 722.8 | 847.1 | −20.8 |
| Zephyr 4.5.0 | Serial and buffer | — | — | 2,163.5 | — |
| Zephyr 4.5.0 | Buffer only | 41.6 | 307.1 | 348.7 | −83.9 |
| NuttX 13.1.0 | Serial and buffer | — | — | 827.4 | — |
| NuttX 13.1.0 | Buffer only | 56.7 | 386.5 | 478.2 | −42.2 |

Separate runs. Each port performed a physical read and passed the same fixed trace to the app. These runs used the experimental persistent descriptor on NuttX. Each value is the median of seven batch averages on each of two boots. [Batch counters](benchmarks/esp32s3-n8r8-trend-matched.json)

| Port | Physical sensor (µs) | Rest of `trend` (µs) | Complete sample (µs) | Boot 1 (µs) | Boot 2 (µs) |
|---|---:|---:|---:|---:|---:|
| ESP-IDF 6.1.1 | 76.4 | 870.2 | 949.0 | 940.0 | 957.9 |
| Zephyr 4.5.0 | 40.5 | 2,122.9 | 2,163.4 | 2,163.4 | 2,163.4 |
| NuttX 13.1.0 | 99.0 | 663.7 | 763.1 | 762.6 | 763.6 |

Diagnostic firmware. Each port ran two separate installations with live readings and one control installation with the fixed trace. Live input triggered different numbers of state changes on each port. Across the 63 measured batches, elapsed cycles converted to milliseconds differed from the app clock by less than 1 ms per batch. [Per-batch counters](benchmarks/esp32s3-n8r8-temperature.json)

| Port | Complete sample, physical sensor (µs) | Native sensor callback (µs) | App outside the sensor callback, live input (µs) | App outside the sensor callback, fixed trace (µs) |
|---|---:|---:|---:|---:|
| ESP-IDF | 104.4 | 24.2 | 81.7 | 845.0 |
| Zephyr | 443.1 | 20.7 | 423.4 | 2,162.8 |
| NuttX | 1,172.1 | 328.5 | 844.4 | 662.1 |

</details>

## CoreMark, Richards and DeltaBlue

The managed and standalone runs with v0.9.5 apps used WAMR 2.4.6. The OS column gives a version where the run's notes or data file give one. [Richards and DeltaBlue inputs and checks](TESTING.md#richards-and-deltablue)

### Managed devices

One app ran at a time with the agent online. The three C5 Mini rows used the same signed modules (CoreMark v0.9.5-wasm3, Richards v0.9.5 and DeltaBlue v0.9.5). Rows on agent v0.9.4 are the v0.9.5-rc.1 side of the [SDK release comparison](#sdk-release-comparison). The echo column is the time for a Flight Control console echo over Wi-Fi during a separate DeltaBlue run.

| Board | OS | Agent | Apps | CPU clock (MHz) | CoreMark (iterations/s) | Richards (ms/iteration) | DeltaBlue (ms/iteration) | Console echo (ms) | Raw data |
|---|---|---|---|---:|---:|---:|---:|---:|---|
| ESP32-S3 DevKitC N8R8 | Zephyr | v0.9.6 | v0.9.5 | — | 20.453 | 848.9 | 853.1 | — | — |
| ESP32-S3 DevKitC N8R8 | NuttX | v0.9.6 | v0.9.5 | — | 19.312 | 909.9 | 953.0 | — | — |
| ESP32-S3 Mini N4R2 | ESP-IDF | v0.9.4 | v0.9.5-rc.1 | — | 22.871 | 789.9 | 854.3 | — | [JSON](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json) |
| ESP32-S3 Mini N4R2 | Zephyr | `0.9.6-bench` or `0.9.6-n4r2-bench` | v0.9.5 | — | 20.469 | 849.0 | 878.4 | — | — |
| ESP32-S3 Mini N4R2 | NuttX | v0.9.6 | v0.9.5 | — | 18.959 | 923.0 | 1,228.2 | — | — |
| ESP32-C6 | ESP-IDF | v0.9.4 | v0.9.5-rc.1 | 160 | 14.595 | 1,226.8 | 1,038.5 | — | [JSON](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json) |
| ESP32-C6 | Zephyr | `0.9.6-bench` or `0.9.6-n4r2-bench` | v0.9.5 | — | 14.790 | 1,169.1 | 982.2 | — | — |
| ESP32-C6 | NuttX | — | v0.9.5 | — | 14.685 | 1,217.6 | 1,003.9 | — | — |
| ESP32-C3 | ESP-IDF | v0.9.4 | v0.9.5-rc.1 | 160 | 15.246 | 1,145.0 | 1,207.7 | — | [JSON](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json) |
| ESP32-C3 Mini | Zephyr | `0.9.6-bench` or `0.9.6-n4r2-bench` | v0.9.5 | — | 15.602 | 1,138.0 | 1,264.0 | — | — |
| ESP32-C3 Mini | NuttX | — | v0.9.5 | — | 14.791 | 1,197.0 | 1,176.8 | — | — |
| ESP32 | ESP-IDF | v0.9.4 | v0.9.5-rc.1 | 240 | 18.880 | 962.5 | 904.8 | — | [JSON](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json) |
| ESP32 rev. 1.0 | Zephyr | `0.9.6-bench` or `0.9.6-n4r2-bench` | v0.9.5 | — | 15.512 | 1,049.4 | 1,140.9 | — | — |
| ESP32 rev. 1.0 | NuttX 13.1.0 | 0.9.6 | v0.9.5 | 240 | 15.363 | 1,129.1 | 1,207.2 | 323 | [JSON](benchmarks/esp32-classic-c5-nuttx.json) |
| ESP32-C5 Mini (4 MiB) | ESP-IDF 6.1.1 | 0.9.6 | v0.9.5 | 240 | 24.347 | 886.9 | 660.1 | 260 | [JSON](benchmarks/esp32-c5-espidf-240mhz.json) |
| ESP32-C5 Mini (4 MiB) | Zephyr 4.5.0 | `0.9.6-bench` | v0.9.5 | 240 | 24.339 | 742.0 | 640.3 | 828 | [JSON](benchmarks/esp32-c5-zephyr-hp-sram-240mhz.json) |
| ESP32-C5 Mini (4 MiB) | NuttX 13.1.0 | 0.9.6 | v0.9.5 | 240 | 23.855 | 772.4 | 628.6 | 801 | [JSON](benchmarks/esp32-c5-nuttx-240mhz.json) |
| ST NUCLEO-F767ZI | Zephyr 4.5.0 | `0.9.6+cache.1` | v0.9.5 | 216 | 24.681 | 720.2 | 606.2 | — | — |
| ST NUCLEO-F767ZI | NuttX 13.1.0 | — | v0.9.5 | 216 | 23.489 | 751.0 | 633.0 | — | [JSON](benchmarks/nuttx-managed-cortex-m.json) |
| Nordic nRF7002-DK | Zephyr 4.5.0 | — | v0.9.5 | 128 | 9.114 | 1,892.6 | 1,579.8 | — | — |
| Nordic nRF7002-DK | NuttX 13.1.0 | — | v0.9.5 | 128 | 8.830 | 2,011.9 | 1,709.1 | — | [JSON](benchmarks/nuttx-managed-cortex-m.json) |
| Raspberry Pi Pico 2 W | Zephyr 4.5.0 | — | v0.9.5 | 150 | 16.961 | 1,067.0 | 1,252.0 | — | — |
| Raspberry Pi Pico 2 W | NuttX, kernel `d50b7688` | — | v0.9.5 | 150 | 17.650 | 1,021.8 | 1,022.6 | — | [JSON](benchmarks/nuttx-managed-cortex-m.json) |

- NuttX booted with Simple Boot on ESP32, C3, C5 and C6. The S3 rows used the v0.9.6 release images.
- The Zephyr C5 Mini allocates from high-performance SRAM first. [Heap order comparison](#richards-and-ram-placement-on-the-zephyr-c5)
- Richards on the Zephyr ESP32 rev. 1.0 used the earlier allocator configuration.
- The Zephyr Nucleo ran with its caches on. The NuttX Nucleo ran with the agent online over ST-LINK. The NuttX Pico 2 W ran with Wi-Fi active.
- The NuttX Nordic ran on the application core with its instruction cache enabled. It installed CoreMark three times, then Richards and DeltaBlue once each, and reported all five results over the J-Link serial network bridge on the same boot.
- One boot: Flight Control received all ESP-IDF C5 Mini results on one boot. The Zephyr C5 Mini CoreMark runs shared one boot. The NuttX C5 Mini and the NuttX ESP32 rev. 1.0 each completed all three benchmarks on one boot. On the NuttX Pico 2 W and Nucleo all five installations completed on one boot.
- The NuttX C5 Mini read back a 240.000 MHz hardware clock.
- The agent builds of the four Zephyr Espressif rows other than the C5 Mini are not recorded per board.

| NuttX Pico 2 W during the signed DeltaBlue run | Heap (B) |
|---|---:|
| Free after instantiation | 224,672 |
| Raw heap low-water mark | 90,464 |
| Free reported by the agent, counting memory it could release | 194,536 |

<details>
<summary>Individual measurements</summary>

Three measurements in run order. The Zephyr Nucleo, Nordic, Pico and S3 rows come from one four-board run on Zephyr 4.5.0 with WAMR 2.4.6, signed v0.9.5 apps and metrics off. Its S3 ran at 240 MHz and is a separate run from the S3 row in the table above.

| Board | OS | Workload | Run 1 | Run 2 | Run 3 | Median |
|---|---|---|---:|---:|---:|---:|
| ST NUCLEO-F767ZI | Zephyr | CoreMark (iterations/s) | 24.685 | 24.661 | 24.681 | 24.681 |
| ST NUCLEO-F767ZI | Zephyr | Richards (ms/iteration) | 720.2 | 726.7 | 720.1 | 720.2 |
| ST NUCLEO-F767ZI | Zephyr | DeltaBlue (ms/iteration) | 605.9 | 606.2 | 612.8 | 606.2 |
| Nordic nRF7002-DK | Zephyr | CoreMark (iterations/s) | 9.114 | 9.112 | 9.126 | 9.114 |
| Nordic nRF7002-DK | Zephyr | Richards (ms/iteration) | 1,892.6 | 1,895.3 | 1,889.1 | 1,892.6 |
| Nordic nRF7002-DK | Zephyr | DeltaBlue (ms/iteration) | 1,574.8 | 1,579.8 | 1,580.3 | 1,579.8 |
| Raspberry Pi Pico 2 W | Zephyr | CoreMark (iterations/s) | 16.961 | 16.961 | 16.960 | 16.961 |
| Raspberry Pi Pico 2 W | Zephyr | Richards (ms/iteration) | 1,064.6 | 1,067.0 | 1,067.9 | 1,067.0 |
| Raspberry Pi Pico 2 W | Zephyr | DeltaBlue (ms/iteration) | 1,252.0 | 1,251.7 | 1,254.8 | 1,252.0 |
| ESP32-S3 DevKitC N8R8, four-board run | Zephyr | CoreMark (iterations/s) | 20.455 | 20.439 | 20.426 | 20.439 |
| ESP32-S3 DevKitC N8R8, four-board run | Zephyr | Richards (ms/iteration) | 850.1 | 851.6 | 848.9 | 850.1 |
| ESP32-S3 DevKitC N8R8, four-board run | Zephyr | DeltaBlue (ms/iteration) | 852.3 | 854.4 | 850.7 | 852.3 |
| ESP32-C5 Mini (4 MiB) | Zephyr | CoreMark (iterations/s) | 20.114 | 24.339 | 24.428 | 24.339 |
| Raspberry Pi Pico 2 W | NuttX | CoreMark (iterations/s) | 17.651 | 17.639 | 17.650 | 17.650 |

</details>

<details>
<summary>Earlier NuttX runs</summary>

| Board | Agent | CPU clock (MHz) | CoreMark (iterations/s) | Richards (ms/iteration) | DeltaBlue (ms/iteration) | Console echo (ms) | Raw data |
|---|---|---:|---:|---:|---:|---:|---|
| ESP32-C5 Mini (4 MiB) | 0.9.6 | 160 | 15.922 | 1,158.6 | 964.8 | 505 | [JSON](benchmarks/esp32-classic-c5-nuttx.json) |
| ESP32 rev. 1.0, a run before the one in the table | — | — | 15.431 | 1,122.4 | 1,200.9 | — | — |

</details>

### Richards and RAM placement on the Zephyr C5

One ESP32-C5 Mini at 240 MHz on Zephyr 4.5.0. The first two rows ran the full suite on agent 0.9.6. The single-heap row is an earlier Richards run. High-performance SRAM first is the current order. It cut Richards time by 28.5% against RTC RAM first.

| Heap order | CoreMark (iterations/s) | Richards (ms/iteration) | DeltaBlue (ms/iteration) | Raw data |
|---|---:|---:|---:|---|
| High-performance SRAM first (current) | 24.339 | 742.0 | 640.3 | [JSON](benchmarks/esp32-c5-zephyr-hp-sram-240mhz.json) |
| RTC RAM first | 22.693 | 1,037.7 | 666.7 | [JSON](benchmarks/esp32-c5-zephyr-240mhz.json) |
| Single heap in high-performance SRAM | — | 734.9 | — | — |

A controlled run on agent 0.9.6 installed the same signed Richards v0.9.5 module with Wi-Fi active and no console probe during timing.

| Zephyr main thread priority | Single heap in high-performance SRAM (ms/iteration) | Multiple heaps, RTC RAM first (ms/iteration) |
|---|---:|---:|
| 0 | 735.1 | 1,031.6 |
| 14 (current) | 740.2 | 1,076.3 |

[Batch timings, console probes, memory addresses and firmware hashes](benchmarks/esp32-c5-zephyr-richards-allocator.json)

### Standalone runtime

The modules ran from flash with Wi-Fi and the agent stopped. NuttX used the nRF5340 application core and the Pico 2 board configuration on the physical Pico 2 W. The Zephyr ESP32 runs used one boot per board.

| Board | OS | CPU clock (MHz) | CoreMark (iterations/s) | Richards (ms/iteration) | DeltaBlue (ms/iteration) |
|---|---|---:|---:|---:|---:|
| ST NUCLEO-F767ZI | NuttX 13.1.0 | 216 | 23.524 | 766.5 | 636.5 |
| Nordic nRF7002-DK | NuttX 13.1.0 | 128 | 13.775 | 1,317.6 | 1,126.2 |
| Raspberry Pi Pico 2 W | NuttX 13.1.0 | 150 | 17.611 | 1,006.7 | 982.6 |
| ESP32 | Zephyr 4.5.0 | 240 | 16.365 | 1,048.7 | 914.5 |
| ESP32-C5 Mini | Zephyr 4.5.0 | 240 | 24.317 | 740.8 | 600.7 |

### CoreMark module sizes

Agent online, one app, metrics export off. Values are medians over three successful installations per module. The 128 KiB module needs 128 KiB of contiguous linear memory. Rows on agent v0.9.4 ran the v0.9.5-rc.1 apps in the same run as the [SDK release comparison](#sdk-release-comparison). [Module requirements](APPS.md#standalone-modules) · [Raw data](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json)

| Board | OS | Agent | CPU clock (MHz) | PSRAM | 64 KiB module (iterations/s) | 128 KiB module (iterations/s) | 128 KiB allocation |
|---|---|---|---:|---|---:|---:|---|
| ESP32 | ESP-IDF | v0.9.4 | 240 | — | 18.880 | — | No fit |
| ESP32-C3 | ESP-IDF | v0.9.4 | 160 | — | 15.246 | — | No fit |
| ESP32-C5 | ESP-IDF | v0.9.4 | 240 | — | 24.542 | — | No fit |
| ESP32-C6 | ESP-IDF | v0.9.4 | 160 | — | 14.595 | 18.612 | Ran |
| S3 Mini | ESP-IDF | v0.9.4 | — | 2 MiB quad | 22.871 | 28.939 | Ran |
| N8R8 | Zephyr | v0.9.4 | — | 8 MiB octal | 20.412 | 25.634 | Ran |
| N8R8 | Zephyr | v0.9.3 | — | Disabled | 20.429 | — | — |

<details>
<summary>Earlier runs on the same six boards</summary>

[Runs on one boot with the v0.9.4 apps](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json) · [First v0.9.5-rc.1 run, with the v0.9.3 medians](benchmarks/v0.9.5-rc.1.json)

| Agent | Apps | Board | 64 KiB module (iterations/s) | 128 KiB module (iterations/s) | Largest free block when 128 KiB failed (KiB) |
|---|---|---|---:|---:|---:|
| v0.9.4 | v0.9.4, same boot as the table above | ESP32 | 18.957 | No fit | — |
| v0.9.4 | v0.9.4, same boot as the table above | ESP32-C3 | 15.279 | No fit | — |
| v0.9.4 | v0.9.4, same boot as the table above | ESP32-C5 | 24.544 | No fit | — |
| v0.9.4 | v0.9.4, same boot as the table above | ESP32-C6 | 14.603 | 18.600 | — |
| v0.9.4 | v0.9.4, same boot as the table above | S3 Mini | 22.882 | 28.908 | — |
| v0.9.4 | v0.9.4, same boot as the table above | N8R8, Zephyr | 20.447 | 25.623 | — |
| v0.9.4 | v0.9.5-rc.1, first run | ESP32 | 19.014 | No fit | 60 |
| v0.9.4 | v0.9.5-rc.1, first run | ESP32-C3 | 15.288 | No fit | 88 |
| v0.9.4 | v0.9.5-rc.1, first run | ESP32-C5 | 24.412 | No fit | 76 |
| v0.9.4 | v0.9.5-rc.1, first run | ESP32-C6 | 14.596 | 18.625 | — |
| v0.9.4 | v0.9.5-rc.1, first run | S3 Mini | 22.920 | 28.973 | — |
| v0.9.4 | v0.9.5-rc.1, first run | N8R8, Zephyr | 20.418 | 25.638 | — |
| v0.9.3 | v0.9.3 | ESP32 | 19.129 | Failed | 76 |
| v0.9.3 | v0.9.3 | ESP32-C3 | 15.271 | Failed | 88 |
| v0.9.3 | v0.9.3 | ESP32-C5 | 24.374 | Failed | 64 |
| v0.9.3 | v0.9.3 | ESP32-C6 | 14.591 | 18.654 | — |
| v0.9.3 | v0.9.3 | S3 Mini | 22.924 | 28.941 | — |
| v0.9.3 | v0.9.3 | N8R8, Zephyr | 20.423 | 25.648 | — |

</details>

### SDK release comparison

The five ESP-IDF boards and the Zephyr N8R8 ran agent v0.9.4 with the SDK v0.9.4 and SDK v0.9.5-rc.1 builds of Richards and DeltaBlue. Each pair shares a board and boot. SDK v0.9.4 uses the first-fit allocator and SDK v0.9.5-rc.1 the free list. [Allocator change](WAMR.md#sdk-allocator) · [Raw data](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json)

| Board | OS | SDK | Richards (ms/iteration) | DeltaBlue (ms/iteration) | DeltaBlue speedup |
|---|---|---|---:|---:|---:|
| ESP32 | ESP-IDF | v0.9.4 | 966.0 | 3,412.0 | — |
| ESP32 | ESP-IDF | v0.9.5-rc.1 | 962.5 | 904.8 | 3.77× |
| ESP32-C3 | ESP-IDF | v0.9.4 | 1,142.6 | 4,333.1 | — |
| ESP32-C3 | ESP-IDF | v0.9.5-rc.1 | 1,145.0 | 1,207.7 | 3.59× |
| ESP32-C5 | ESP-IDF | v0.9.4 | 734.2 | 2,624.2 | — |
| ESP32-C5 | ESP-IDF | v0.9.5-rc.1 | 731.5 | 671.4 | 3.91× |
| ESP32-C6 | ESP-IDF | v0.9.4 | 1,232.8 | 4,240.0 | — |
| ESP32-C6 | ESP-IDF | v0.9.5-rc.1 | 1,226.8 | 1,038.5 | 4.08× |
| S3 Mini | ESP-IDF | v0.9.4 | 791.9 | 2,864.3 | — |
| S3 Mini | ESP-IDF | v0.9.5-rc.1 | 789.9 | 854.3 | 3.35× |
| N8R8 | Zephyr | v0.9.4 | 851.5 | 3,397.2 | — |
| N8R8 | Zephyr | v0.9.5-rc.1 | 851.4 | 941.9 | 3.61× |

<details>
<summary>First paired trial on agent v0.9.4</summary>

Same protocol, one boot per board, before the run above. [Raw data](benchmarks/v0.9.5-rc.1.json)

| Board | DeltaBlue, SDK v0.9.4 (ms/iteration) | DeltaBlue, SDK v0.9.5-rc.1 (ms/iteration) | DeltaBlue speedup | Richards, SDK v0.9.4 (ms/iteration) | Richards, SDK v0.9.5-rc.1 (ms/iteration) | Richards speedup |
|---|---:|---:|---:|---:|---:|---:|
| ESP32 | 3,396.0 | 897.8 | 3.78× | 960.9 | 959.7 | 1.00× |
| ESP32-C3 | 4,336.2 | 1,204.6 | 3.60× | 1,139.8 | 1,141.4 | 1.00× |
| ESP32-C5 | 2,624.4 | 667.9 | 3.93× | 727.4 | 726.9 | 1.00× |
| ESP32-C6 | 4,239.1 | 1,040.7 | 4.07× | 1,232.0 | 1,227.3 | 1.00× |
| S3 Mini | 2,867.0 | 854.7 | 3.35× | 792.8 | 791.5 | 1.00× |
| N8R8, Zephyr | 3,397.2 | 946.2 | 3.59× | 851.8 | 851.2 | 1.00× |

</details>

## Catalog apps

ESP32, C3, C5, C6 and the S3 Mini ran ESP-IDF. The N8R8 ran Zephyr with simulated temperature. The tables report the first run of the v0.9.5-rc.1 apps on agent v0.9.4. The ESP32 has no die sensor. [Raw data, with the v0.9.3 medians](benchmarks/v0.9.5-rc.1.json)

| Board | OS | Forest (µs) | Neural (µs) | `trend` sample (µs) | `battery` poll, no pack (µs) |
|---|---|---:|---:|---:|---:|
| ESP32 | ESP-IDF | 70 | 840 | — | 440 |
| ESP32-C3 | ESP-IDF | 200 | 4,530 | 60 | 50 |
| ESP32-C5 | ESP-IDF | 70 | 1,940 | 90 | 40 |
| ESP32-C6 | ESP-IDF | 140 | 3,060 | 110 | 50 |
| S3 Mini | ESP-IDF | 60 | 720 | 340 | 120 |
| N8R8 | Zephyr | 70 | 880 | 50 | 40 |

`mqttdemo` command to acknowledgement over TLS, 48 QoS 1 round trips per board. The page's sources give a maximum only for agent v0.9.3.

| Board | OS | Median, agent v0.9.4 (ms) | Maximum, agent v0.9.3 (ms, rounded) |
|---|---|---:|---:|
| ESP32 | ESP-IDF | 142.56 | 156 |
| ESP32-C3 | ESP-IDF | 131.10 | 190 |
| ESP32-C5 | ESP-IDF | 125.15 | 184 |
| ESP32-C6 | ESP-IDF | 127.84 | 185 |
| S3 Mini | ESP-IDF | 130.40 | 184 |
| N8R8 | Zephyr | 192.11 | 288 |

<details>
<summary>Agent v0.9.4 with the v0.9.4 and v0.9.5-rc.1 apps on one boot</summary>

A later run paired both app sets on each board. The probe and `mqttdemo` modules are identical in the two sets. [Raw data](benchmarks/v0.9.4-v0.9.5-rc.1-repeat.json) Probe columns are median batch times. Divide by 256 (32 for the neural network) for the time per operation.

| Board | Apps | Forest batch (ms) | Neural batch (ms) | `trend` batch (ms) | `battery` batch (ms) | MQTT round trip, median (ms) | `sampler` + `localalarm` heap used (B) |
|---|---|---:|---:|---:|---:|---:|---:|
| ESP32 | v0.9.4 | 19 | 27 | — | 106 | 124.3 | 39,844 |
| ESP32 | v0.9.5-rc.1 | 19 | 27 | — | 110 | 125.4 | 38,032 |
| ESP32-C3 | v0.9.4 | 47 | 139 | 52 | 23 | 124.1 | 39,960 |
| ESP32-C3 | v0.9.5-rc.1 | 47 | 154 | 62 | 23 | 125.4 | 40,176 |
| ESP32-C5 | v0.9.4 | 17 | 55 | 19 | 10 | 127.2 | 37,384 |
| ESP32-C5 | v0.9.5-rc.1 | 17 | 48 | 26 | 9 | 124.9 | 37,576 |
| ESP32-C6 | v0.9.4 | 36 | 51 | 44 | 13 | 127.7 | 39,148 |
| ESP32-C6 | v0.9.5-rc.1 | 34 | 106 | 44 | 14 | 124.4 | 37,288 |
| S3 Mini | v0.9.4 | 15 | 60 | 57 | 31 | 123.8 | 39,440 |
| S3 Mini | v0.9.5-rc.1 | 38 | 23 | 40 | 20 | 126.6 | 37,404 |
| N8R8, Zephyr | v0.9.4 | 18 | 48 | 12 | 10 | 186.4 | 19,668 |
| N8R8, Zephyr | v0.9.5-rc.1 | 18 | 49 | 12 | 10 | 180.3 | 19,644 |

</details>

<details>
<summary>Agent v0.9.3 with the v0.9.3 apps</summary>

| Board | OS | Forest (µs) | Neural (µs) | `trend` sample (µs) | `battery` poll, no pack (µs) | MQTT round trip, median (ms) |
|---|---|---:|---:|---:|---:|---:|
| ESP32 | ESP-IDF | 150 | 840 | — | 410 | 127.23 |
| ESP32-C3 | ESP-IDF | 190 | 4,340 | 180 | 90 | 121.94 |
| ESP32-C5 | ESP-IDF | 90 | 1,380 | 70 | 60 | 120.27 |
| ESP32-C6 | ESP-IDF | 160 | 2,970 | 100 | 50 | 119.96 |
| S3 Mini | ESP-IDF | 60 | 750 | 370 | 70 | 122.69 |
| N8R8 | Zephyr | 70 | 1,500 | 50 | 40 | 183.53 |

</details>

[App checks on hardware](DEVICES.md#app-checks-on-hardware)

### Timer callbacks

Elapsed time of a full periodic callback, including entry into WASM. [Method](TESTING.md#app-benchmark-methods) The ESP-IDF baseline used the FreeRTOS stack scan. Zephyr is the control and used its standard implementation in both runs. The second run is the agent v0.9.3 run of this page. On the ESP32, `alarm` disables its timer without a die sensor, so the row uses `battery` without a pack. [Stack lookup patch](../third_party/patches/wamr-espidf-stack-info.patch)

| Board | OS | App | Baseline run (µs) | Run with direct stack lookup on ESP-IDF (µs) | Time saved by direct stack lookup (%) |
|---|---|---|---:|---:|---:|
| ESP32 | ESP-IDF | `battery` | 1,329.5 | 1,049.0 | 21.1 |
| ESP32-C3 | ESP-IDF | `alarm` | 830.5 | 514.0 | 38.1 |
| ESP32-C5 | ESP-IDF | `alarm` | 633.5 | 442.0 | 30.2 |
| ESP32-C6 | ESP-IDF | `alarm` | 676.0 | 382.5 | 43.4 |
| S3 Mini | ESP-IDF | `alarm` | 722.5 | 502.0 | 30.5 |
| N8R8 | Zephyr | `alarm` | 431.0 | 434.0 | — |

An instrumented ESP-IDF S3 Mini build with direct stack lookup and dispatch in flash ran each catalog workload for 24 samples after warm-up. CPU times include synchronous native calls. Both models received identical inputs after the sensor read.

| S3 Mini workload | Measure | Median (µs) |
|---|---|---:|
| `alarm` | CPU per reading | 550 |
| `mqttdemo` | CPU per periodic publish | 2,227 |
| `sampler` | CPU per reading and queue write | 7,787 |
| `localalarm` | Elapsed from sensor reading to callback entry | 399 |
| `tinyml` | CPU per full callback | 1,575 |
| `tinyml` | CPU after the sensor read | 1,146 |
| `tinymlp` | CPU per full callback | 2,550 |
| `tinymlp` | CPU after the sensor read | 2,116 |

### S3 Mini N4R2

The same S3 Mini ran agent v0.9.4 on NuttX and ESP-IDF. The Zephyr row is the N8R8 with simulated temperature. The ESP-IDF and Zephyr rows repeat the [catalog table](#catalog-apps).

| Board | OS | Forest (µs) | Neural (µs) | `trend` sample (µs) | `battery` poll, no pack (µs) |
|---|---|---:|---:|---:|---:|
| S3 Mini | NuttX | 70 | 1,030 | 2,200 | 40 |
| S3 Mini | ESP-IDF | 60 | 720 | 340 | 120 |
| N8R8 | Zephyr | 70 | 880 | 50 | 40 |

<details>
<summary>Median batch times (ms per batch)</summary>

| Board | OS | Forest | Neural | `trend` | `battery` |
|---|---|---:|---:|---:|---:|
| S3 Mini | NuttX | 19 | 33 | 564 | 9 |
| S3 Mini | ESP-IDF | 15 | 23 | 87 | 30 |
| N8R8 | Zephyr | 18 | 28 | 12 | 10 |

</details>

### App combinations

Console free heap with the signed apps running, on agent v0.9.4 with the v0.9.5-rc.1 apps. ESP-IDF includes available PSRAM and Zephyr reports its C heap. The last column is the drop from the same board with both apps unloaded. [Raw data, with the v0.9.3 readings](benchmarks/v0.9.5-rc.1.json)

| Board | OS | Rust blink (B) | C++ blink (B) | AssemblyScript blink (B) | TinyGo blink (B) | `sampler` + `localalarm` (B) | Heap used by that pair (B) |
|---|---|---:|---:|---:|---:|---:|---:|
| ESP32 | ESP-IDF | 36,152 | 36,988 | 29,268 | 25,112 | 74,000 | 38,116 |
| ESP32-C3 | ESP-IDF | 59,568 | 59,348 | 51,944 | 45,744 | 94,792 | 37,564 |
| ESP32-C5 | ESP-IDF | 35,256 | 34,984 | 27,584 | 21,500 | 69,844 | 40,028 |
| ESP32-C6 | ESP-IDF | 125,632 | 125,300 | 117,824 | 111,728 | 165,176 | 38,740 |
| S3 Mini | ESP-IDF | 2,173,844 | 2,173,540 | 2,166,092 | 2,159,936 | 2,208,200 | 38,500 |
| N8R8 | Zephyr | 85,920 | 85,652 | 78,764 | 72,852 | 66,880 | 19,668 |

`sampler` (1,581 B) and `localalarm` (1,049 B) exchanged die temperature without an MQTT grant on five boards. The ESP32 needs an external source and the receiver waited for input. [Local app permissions](APPS.md#pass-readings-between-local-apps)

<details>
<summary>Agent v0.9.3 with the v0.9.3 apps</summary>

Each pair figure is within 1.2 KiB of the v0.9.4 column above.

| Board | OS | Rust blink (B) | C++ blink (B) | AssemblyScript blink (B) | TinyGo blink (B) | `sampler` + `localalarm` (B) | Heap used by that pair (B) |
|---|---|---:|---:|---:|---:|---:|---:|
| ESP32 | ESP-IDF | 39,900 | 38,100 | 30,584 | 26,060 | 81,224 | 37,640 |
| ESP32-C3 | ESP-IDF | 57,948 | 57,728 | 50,224 | 38,356 | 101,048 | 37,284 |
| ESP32-C5 | ESP-IDF | 32,424 | 32,464 | 26,632 | 18,584 | 76,188 | 38,876 |
| ESP32-C6 | ESP-IDF | 129,144 | 128,976 | 121,432 | 115,360 | 170,136 | 39,188 |
| S3 Mini | ESP-IDF | 2,170,244 | 2,170,072 | 2,164,308 | 2,156,252 | 2,214,664 | 37,768 |
| N8R8 | Zephyr | 85,932 | 85,656 | 78,768 | 72,852 | 72,152 | 19,652 |

C++ blink ran alongside `alarm` on ESP32, C3, C6 and both S3 boards. Rust blink ran alongside `tinyml` on C3, C6 and the S3 Mini. The ESP32 had room for a 2,488 B layer with blink resident. The C5 had a 62 KiB contiguous block when Rust blink needed 64 KiB.

</details>
