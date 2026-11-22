# ESP-IDF

ESP-IDF 6.1.1 runs the agent on ESP32, C3, C5, C6 and S3. All five chips enrolled over Wi-Fi and ran signed apps. [Hardware matrix](DEVICES.md#hardware-compatibility) · [App benchmarks](BENCHMARKS.md)

## Build profiles

| Chip or module | Build selection | CPU clock |
|---|---|---:|
| ESP32, C5 | `TARGET=esp32` or `TARGET=esp32c5` | 240 MHz |
| C3, C6 | `TARGET=esp32c3` or `TARGET=esp32c6` | 160 MHz |
| S3 with internal RAM or 2 MiB quad PSRAM | `TARGET=esp32s3` | 240 MHz |
| S3 with 8 MiB octal PSRAM (N8R8, N16R8) | `TARGET=esp32s3 VARIANT=r8` | 240 MHz |

Match the profile on the first USB flash and subsequent OTA updates. [Build and flash](QUICKSTART.md#3-build-and-flash-a-board-esp-idf) · [Provisioning](PROVISIONING.md) · [Security variants](SECURITY.md)

## Configuration

| Setting | Effect on the device |
|---|---|
| Firmware `-Os`, WAMR `-O2` | Keeps firmware within the OTA slot while optimizing interpreted apps for speed. |
| Dynamic TLS receive buffers | Small records allocate on demand. The agent reserves a contiguous block at startup for a full record. |
| WASM memory placement | The interpreter prefers internal RAM and can fall back to PSRAM. On SRAM-only C5/C6 the loader reserves space early for eligible linear memories. |
| App admission | Startup needs contiguous linear memory and an app thread stack. After startup the agent requires free internal RAM for reconnecting. |

| Figure | Size (KiB) | Setting it belongs to |
|---|---:|---|
| S3 Mini instruction cache | 16 | Firmware `-Os`, WAMR `-O2` |
| Smallest eligible linear memory the loader reserves early on SRAM-only C5/C6 | 16 | WASM memory placement |
| App thread stack | 8 | App admission |
| Internal RAM the agent requires after startup, for reconnecting | 40 | App admission |

Dynamic TLS receive buffers: the contiguous block reserved at startup for a full record is 16.7 KB.

[Build defaults](../port/espidf/sdkconfig.defaults) · [App limits](APPS.md#size-and-limits) · [Runtime optimizations](WAMR.md)

## Firmware size and OTA

ESP-IDF 6.1.1, WAMR 2.4.6. Each OTA slot holds 1,703,936 bytes in the 4 MiB layout. [Partition table](../port/espidf/partitions.csv) · [Verified updates and rollback](DELTA-UPDATES.md#what-was-verified)

| Board | Image (B) | OTA slot free (%) |
|---|---:|---:|
| ESP32 | 1,193,136 | 30.0 |
| C3 | 1,265,712 | 25.7 |
| C5 | 1,362,928 | 20.0 |
| C6 | 1,365,904 | 19.8 |
| S3 Mini | 1,202,896 | 29.4 |

## Runtime memory

Additional heap used by a workload on the C3. A dash means no value is given.

| C3 workload | Heap reading | Free before (B) | Free after (B) | Additional heap used (KiB) |
|---|---|---:|---:|---:|
| Run `trend` | — | — | — | 18.6 |
| Configure broker and connect over TLS with `mqttdemo` resident | Free heap | 116,564 | 91,744 | 24.2 |
| Open console with `alarm` running | Internal heap | 129,760 | 111,936 | 17.4 |

`trend` is a 1,617 B module. Its figure includes the 8 KiB thread stack.

| Internal heap free with `alarm` (B) | ESP32 | C3 | C5 | C6 | S3 Mini |
|---|---:|---:|---:|---:|---:|
| Minimum | 18,704 | 35,788 | 107,544 | 23,992 | 130,284 |
| Maximum | 113,608 | 133,476 | 108,720 | 204,520 | 153,020 |

The diagnostic build sampled byte-addressable internal heap every second for 60 s after app initialization, including active TLS allocations, with metrics every 30 s. [Earlier measurements excluding the TLS reservation](https://github.com/francescopace/flightctl-mcu/blob/aaf93f36891556c3d0913dcd40e50a7d6a843be0/docs/RESULTS.md#ram)

S3 N8R8, after neural inference. The heap is the default capability heap and includes available PSRAM. [Same-board workload](BENCHMARKS.md#esp32-s3-n8r8-comparison-on-one-board)

| S3 N8R8 build | Image (B) | Heap free (B) | Heap total (B) |
|---|---:|---:|---:|
| Single core, v0.9.4 | 1,194,928 | 8,529,308 | 8,725,932 |
| Dual core | 1,203,968 | 8,518,448 | 8,719,564 |

Full signed firmware download on the C3 with `alarm` resident and metrics off. Minimum free covers manifest lookup through connection close. The largest free block was sampled during writes, about every 16 KiB. [Zephyr download measurements](ZEPHYR.md#memory-and-ota-on-hardware)

| Heap | Free at start (B) | Minimum free (B) | Lowest largest free block during writes (B) |
|---|---:|---:|---:|
| C3, internal | 136,588 | 46,812 | 94,208 |
