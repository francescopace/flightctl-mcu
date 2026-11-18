# Results

What works, where it was verified, and the measurements. The devices are microcontrollers with no Linux, managed by an unmodified **Flight Control 1.4.0** (the upstream of Red Hat Edge Manager) on [kind](https://kind.sigs.k8s.io/). The figures come from development builds before 1.0.

## What works

**Boards**: the classic ESP32, ESP32-C3, C5, C6 and S3 with ESP-IDF, and the ESP32-S3 with Zephyr. **Emulator**: the real firmware in Espressif's emulator ([below](#in-espressifs-emulator)). **Tests**: the end-to-end tests in CI with the simulated device and Zephyr's emulated builds, unit tests and host tests ([TESTING.md](TESTING.md)). ✓ verified, ◐ in part (what is missing is in [current limitations](#current-limitations) or the linked page), – not tried.

| Function | What it does | Boards | Emulator | Tests | More |
|---|---|---|---|---|---|
| Enrollment | Device key, CSR, mTLS | ✓ | ✓ | ✓ | [QUICKSTART](QUICKSTART.md#5-approve-it) |
| Automatic approval | From an allow list | – | – | ✓ | [QUICKSTART](QUICKSTART.md#5-approve-it) |
| Status | Full agent API status, `Unknown` when offline | ✓ | ✓ | ✓ | [Label sync](FLIGHTCTL-LIMITATIONS.md#label-sync) |
| HTTP/2 | Spec, status and downloads on one connection | ◐ | ✓ | ✓ | [Below](#http2) |
| Resource alerts | CPU, heap, storage against the fleet's rules | ◐ | – | ✓ | [Timings](#timings) |
| Config | Files from the spec, no reboot | ✓ | – | ✓ | [QUICKSTART](QUICKSTART.md#6-give-it-firmware-config-and-an-app) |
| Firmware OTA | A/B slots, staged rollouts | ✓ | ✓ | – | [Timings](#timings) |
| Firmware deltas | A patch from the running release | ✓ | ✓ | ✓ | [DELTA](DELTA-UPDATES.md) |
| Firmware and apps in one spec | The firmware first, then the apps that need it | ✓ | ✓ | ✓ | [APPS](APPS.md#size-and-limits) |
| Automatic rollback | Back to the previous firmware if the new one fails | ✓ | ✓ | – | [The way back](#the-way-back-on-the-esp32-s3-both-ports) |
| Mixed fleets | One fleet for every chip | ✓ | – | – | [DEVICES](DEVICES.md) |
| WASM apps | Installed and updated without a reboot | ✓ | ✓ | ✓ | [APPS](APPS.md) |
| App interface | Hardware by name, timers, events, storage, C library | ◐ | – | ✓ | [APPS](APPS.md#ports) |
| An app's limits | CPU budget per call, heap kept for the agent | ◐ | ✓ | ✓ | [APPS](APPS.md#size-and-limits) |
| Modules built for no device | Any WebAssembly module, run once | ✓ | – | ✓ | [APPS](APPS.md#modules-built-for-no-device) |
| Demo apps | emlearn's TinyML model, wasm3-arduino's blink as published | ◐ | – | ✓ | [APPS](APPS.md#the-blink-of-wasm3-arduino) |
| Apps that crash the device | Not started again | – | ✓ | ✓ | [APPS](APPS.md#how-an-app-runs) |
| Crash reports | `lastFatalError` in the status | ✓ | ✓ | ✓ | [Crash](#a-crash-in-the-status) |
| Native coredump | Download from flash with console access and open with the release ELF | S3 (Zephyr) | ✓ | ✓ | [Download results](#a-crash-in-the-status) |
| Certificate renewal | Same key, new certificate | C6 | ✓ | ✓ | [TESTING](TESTING.md) |
| Signed images | Cosign signatures checked on the device | ✓ | ✓ | ✓ | [SECURITY](SECURITY.md#two-signing-keys) |
| Registry with TLS and a password | CA and `auth.json` from the fleet | ✓ | ✓ | ✓ | [REGISTRY](REGISTRY.md) |
| Registry tokens | Bearer tokens, as Quay and Harbor use | – | – | ✓ | [TESTING](TESTING.md#registry-tokens) |
| Decommissioning | Key and certificate wiped | ✓ | ✓ | ✓ | [QUICKSTART](QUICKSTART.md#7-take-it-out) |
| Remote console | `flightctl console` and the UI's terminal | ✓ | – | ✓ | [CONSOLE](CONSOLE.md) |
| Device log | The UI's Logs tab, previous boot included | ✓ | – | ✓ | [CONSOLE](CONSOLE.md#the-log) |
| Metrics | OTLP to the Telemetry Gateway | ✓ | – | ✓ | [METRICS](METRICS.md) |
| MQTT for the apps | One broker connection for all apps, TLS | ◐ | – | ✓ | [APPS](APPS.md#mqtt) |
| Known vulnerabilities | SBOM per release, CVEs through Trustify | ✓ | – | – | [SBOM](SBOM.md) |
| Field provisioning | WiFi over the device's access point | C6 (ESP-IDF), S3 (Zephyr) | ✓ | ✓ | [PROVISIONING](PROVISIONING.md) |
| Secure Boot, flash encryption, anti-rollback | The `secure` variant | – | ✓ | – | [SECURITY](SECURITY.md) |

<a id="http2"></a>HTTP/2: the spec's long-poll and the status share one connection on the five ESP-IDF chips and on the ESP32-S3 with Zephyr; firmware and apps come over it on the classic ESP32, C5, C6 and S3 and on the ESP32-S3 with Zephyr (every request HTTP/2 in the registry's log); the end-to-end tests run over it in CI and in the emulator. An ESP32-C6 with a software P-256 key renewed its management certificate over this connection and continued reporting under the same identity. A test build brought the local renewal deadline forward. The server used its normal signer and certificate lifetime.

A board keeps its identity across firmware updates; only a full flash erase makes it enroll again as a new device.

On a physical ESP32-C6 running ESP-IDF firmware `v3.14-rc12`, ESP SoftAP Prov reported a wrong WiFi password. The installer retried from the phone. The C6 obtained an IP address about 2 s after receiving the corrected credentials and closed its access point about 8 s later. After a USB reset it rejoined the saved network automatically. Flight Control received an Online status for the same device with a new boot ID.

## Measurements

### Firmware size

ESP-IDF 6.0.3, built for size (`-Os`), with WAMR at `-O2`:

| Chip | Image, v3.12 | Free in its 1,664 KB OTA slot | Built for speed (`-O2`), v3.10 | Local build, 18 Nov 2026 (B) |
|---|---|---|---|---:|
| ESP32 | 978 KB | 41% | 1,121 KB | 1,137,936 |
| ESP32-S3 | 977 KB | 41% | 1,118 KB | 1,141,872 |
| ESP32-C3 | 1,024 KB | 38% | 1,192 KB | 1,199,712 |
| ESP32-C5 | 1,107 KB | 33% | 1,282 KB | 1,291,472 |
| ESP32-C6 | 1,110 KB | 33% | 1,288 KB | 1,296,816 |

The local build uses the ordinary ESP-IDF configuration without development Wi-Fi settings. For the two published releases in the table, `-Os` saves 141 to 178 KB per image (about 13%). Delta sizes are in [DELTA-UPDATES.md](DELTA-UPDATES.md).

| Component, on the C3 built for speed | Flash |
|---|---|
| WiFi, WPA and lwIP | about 430 KB |
| Cryptography and TLS (mbedTLS 4, TF-PSA-Crypto) | about 197 KB |
| WAMR (fast interpreter) | 68 KB |
| **The agent** (`core/`) | **about 62 KB**; 79 KB with cJSON, WiFi provisioning, the delta decoder and the ESP-IDF port |
| The apps' C library | 7.7 KB |
| WiFi provisioning with its access point, on the S3 | 17.4 KB ([PROVISIONING.md](PROVISIONING.md#in-the-field-the-wifi-network)) |
| Floating point in `printf` (picolibc), Zephyr | 5.4 KB on the S3, 4.7 KB on a Cortex-M3, no RAM |
| The apps' `expf`, `logf` and `tanhf` | 3.6 KB on the C3, 2.5 KB on the S3 with Zephyr, no RAM |

On the ESP32-S3 with Zephyr, linking the same configuration before and after the app KV, libc import, reconciliation, config notification, certificate and HTTP/2 buffer changes reduced the unsigned image from 1,039,268 to 1,038,932 bytes (336 bytes). The removed table of 38 libc name pointers accounted for 152 bytes in the original link map. Both links left 4,968 bytes of DRAM free.

Almost all the space goes to the network stack and to cryptography, which any securely connected device needs anyway.

The first Zephyr ESP32-S3 build with atomic app updates and the trimmed WAMR libc table linked at 375,720 of 380,672 B of DRAM, leaving 4,952 B. Its C heap arena is 120,112 B. The ELF contains 240 B for the 15-entry WAMR table, down from 832 B for 52 entries. The whole unsigned image is 1,037,668 B, 1,552 B smaller than the preceding two-slot build. That whole-image difference includes the catalog and metrics changes. In the Xtensa link maps, the retained sections from `libc_builtin_wrapper.c` shrank by 1,184 B of text, 1,144 B of read-only data (including the table), and 52 B of literals relative to the 52-entry libc build. These section sizes exclude padding between objects. The ordinary N8R8 build with the RTC crash record, retry changes, PSRAM TLS records and 32 POSIX mutexes used 375,680 of 380,672 B of DRAM, leaving **4,992 B**. Its unsigned image is 1,038,004 B and its signed application is 1,038,168 B.

The host app tests injected a catalog write failure and simulated reboot on either side of the catalog commit with both app slots occupied. The restarted app loaded the module selected by its saved version. The test also covered migration from the older catalog.

### RAM

An instrumented ESP32-C6 kept its software P-256 CSR in a 420 B allocation for 417 B including the terminator. With no apps, the management renewal POST started with 203,028 B free and reached 191,420 B. The GET that installed the issued certificate reached 193,928 B from the same starting level. On a second build with a larger trace buffer, the telemetry renewal POST went from 162,548 to 150,944 B free (11,604 B used). Fetching and installing the issued certificate went from 163,504 to 154,728 B (8,776 B used). Both roles installed new certificates for the same device, valid for another year. These internal-heap measurements include concurrent agent work.

On Zephyr ESP32-S3 with one `alarm` app, allocating descriptors on demand raised steady free C heap from 61,660 B to 66,744 B, while the largest free block fell from 60,180 B to 44,036 B. With `alarm` and `trend` the largest block fell from 49,500 B to 30,180 B. These measurements precede the Zephyr allocator change that protects the largest block. After that change, on an N8R8 without the exporter, one fixed-size app left 67,932 B free with a 61,516 B largest block. Two fixed-size apps left 56,016 B free with 38,284 B contiguous. With a growable primary and the secondary parked, the figures were 68,184 and 21,956 B.

An ESP32-S3 N8R8 running Zephyr with a resident `alarm` app installed a full 1,037,990 B signed firmware over HTTP/2 with flash writes paced for the test. The registry sent the complete body in 58.38 s. The board logged installation after 63 s, rebooted, confirmed the new image and resumed the app. The device reported that it used no delta. The classic ESP32 also completed a full 1,131,232 B OTA with `alarm` running. An address-restricted TCP relay paced its original TLS connection to about 24 KiB/s. The relay carried the transfer for 60.27 s. The new firmware confirmed and reported UpToDate with no delta.

An ESP32-C3 applying a spec that removed two resident apps had 115,060 B free at firmware fetch and 136,592 B before the manifest. It downloaded a 1,200,416 B full image in 6 s, restarted and confirmed it. The device rejected a spec with two primaries and preserved its running apps and files. Builds with slot reporting expose the running ESP-IDF partition label or MCUboot slot in `customInfo.bootSlot`.

On the C6, changing a map containing its onboard LED and removing the apps left 1,032 B less internal heap than the empty post-reboot sample with one app, and 1,104 B less with two. The allocation trace covered installation, map changes and removal. It retained all allocations of 64 B or more without overflowing. The second run had one extra live 128 B allocation in NVS. The original seven-entry map had left about 9.4 KB after two apps. A separate allocation found in both traces belonged to a finished remote console thread (8,192 B of stack and 112 B of thread storage). After the spec loop began joining finished console threads, the same two-app trace contained neither allocation. Internal heap went from 171,764 B empty to 171,592 B after removing both apps. The largest free block went from 126,976 to 98,304 B. Repeating with two copies of the original peripheral and metrics test app on the LED-only map left 1,016 B less total heap. With telemetry enabled, the gateway observed both cumulative counters at 1, 7 and 13 over successive 30 s exports. The map reload between the last two exports restarted both apps, as shown by their per-instance gauges, while both counters continued.

On the physical C6 at `f09c6c6b`, two apps retained 125,816 B of free heap on the LED-only map. Expanding to the original seven devices and returning to the LED left 124,988 B after one cycle and 124,948 B after two, a difference of 828 B and 868 B respectively. After both apps were removed, free heap was 1,732–1,740 B below the empty post-boot sample and the largest block had fallen from 139,264 to 118,784 B. The one-app and two-app traces both ended with the same 23 live allocations of at least 64 B, totalling 6,261 B. Free NVS storage returned to 635,744 B after each trial, with the same 17 stored objects and sizes. Across the entire firmware round trip, free NVS on the original firmware increased by 32 B. The earlier 576 B decrease did not recur.

**App metrics on the ESP32-C6.** The ESP-IDF compiler gives the per-app table a fixed size of 456 bytes for eight metrics and bookkeeping. The same allocation also holds one copy of the app name and its terminator. The previous table requested 1,408 bytes as soon as either app published. The figures below are heap requests before allocator rounding and overhead. The static list pointer occupies four bytes in both builds.

| Publishing apps | Previous heap request | Current heap request | Bytes saved |
|---|---:|---:|---:|
| None | 0 B | 0 B | 0 B |
| `main` | 1,408 B | 461 B | 947 B |
| `main` and `aux` | 1,408 B | 921 B | 487 B |

A single publisher with the longest supported app name (47 bytes) requests 504 bytes, saving 904 bytes. Linking both versions for the ESP32-S3 with Zephyr produced the same unsigned image size (1,038,932 bytes) and the same free DRAM at link time (4,968 bytes). The change in the largest contiguous block on a running device remains to be measured.

**Identity and HTTP/2 on the host.** Across 32 CSRs generated with a software P-256 device key, the retained PEM allocation was 417 bytes each, down from 2,048 bytes. The HTTP/2 reader requested 100 bytes for a 100-byte first header fragment, down from 8,192 bytes. Further fragments grow the buffer up to the same 8 KiB limit. The 1,631-byte saving is resident for the generated device CSR. The 8,092-byte saving lasts while a short header block is being received. Neither figure includes allocator overhead.

**App slots on the ESP32-S3 with Zephyr.** Moving the slot storage into the C heap increased its arena from 116,736 to 120,112 bytes. The linked descriptor is 896 bytes and its runner's handles, events and output buffer take 768 bytes. An isolated run of Zephyr's allocator on the host used the S3's chunk format, eight-byte alignment and those ELF sizes. Relative to two static slots, an otherwise empty arena gained 3,376 free bytes with zero apps, **1,696 with one** and 16 with two. Removing the second allocation restored the one-app result. The largest free block had the same gain in this test. Firmware running Wi-Fi, TLS and WASM can place other allocations between these blocks.

Internal heap with the `alarm` app running; on the ESP32, C5, C6 and S3 after 40 minutes of CoreMark installs and swaps:

| Board | Firmware | Free | Lowest | Largest internal block |
|---|---|---|---|---|
| Classic ESP32 | v3.12 | 154.0 KB | 2.3 KB | 76.0 KB |
| ESP32-C5 | v3.12 | 109.2 KB | 5.3 KB | 60.0 KB |
| ESP32-C6 | v3.12 | 205.2 KB | 1.8 KB | 164.0 KB |
| ESP32-S3 N4R2 | v3.12 | 161.2 KB | – (PSRAM counted) | 84.0 KB |
| ESP32-C3 | v3.13-rc6, an hour after boot | 70 to 75 KB | 2.8 KB, at start-up | 36.0 KB |

| Board | Lowest during a metrics export, every 30 s (v3.13-rc6) | Free once MQTT is connected (v3.14-rc6) |
|---|---|---|
| Classic ESP32 | 79 to 100 KB | 91 KB |
| ESP32-S3 | 93 to 108 KB | 96 KB |
| ESP32-C5 | 26 to 49 KB | 44 KB |
| ESP32-C6 | 25 to 44 KB, WiFi buffers at ESP-IDF's defaults | 141 KB |
| ESP32-C3 | 19 to 20 KB; 13 KB right after it took a whole firmware | 39 KB |
| ESP32-S3, Zephyr (z26) | 8.3 KB of the C heap, 55 KB free, steady | – |

| Part | What it takes |
|---|---|
| Device log | 4 KB |
| Resource alerts | about 4 KB: 65.6 KB free on the C3 five minutes after boot |
| App interface and C library | about 4.5 KB of static RAM; 1.5 KB less since the function tables are const, in flash |
| A WASM app: `trend`, 1.9 KB of `.wasm`, on the C3 | 17.7 KB of heap: the loaded module about 5 KB, the instance and its WAMR stack about 3.5 KB, its memory about 1 KB, its thread's stack 8 KB |
| The MQTT connection, on the C3 | 10 KB of heap (49 KB free before, 39 KB after, v3.14-rc7), and 29 to 31 KB free at the lowest during its handshake; the client's thread used 3.2 to 4.2 KB of its 8 KB stack |
| The metrics connection, Zephyr on the S3 | its 17 KB receive buffer, from the C heap |
| The `tinyml` app's memory | 1.2 KB, with 4.1 KB of `.wasm` |
| The `tinymlp` app's memory | 2.4 KB, with 6.8 KB of `.wasm` |
| The memory of wasm3-arduino's blink, any of its four builds | 64 KB, in one block |

The heap's lowest in the first 25 s after a restart (v3.14-rc7, three restarts each; the block held while a TLS connection opens left out):

| Board | `alarm` alone | `alarm` and `mqttdemo`, MQTT connected |
|---|---|---|
| ESP32-C3 | 51 to 55 KB | 16 to 19 KB |
| ESP32-C5 | 56 to 58 KB | 21 to 24 KB |
| Classic ESP32 | 67 to 69 KB | 33 to 34 KB |
| ESP32-C6 | 151 to 154 KB | 119 to 120 KB |

The difference is what the app and MQTT keep (`mqttdemo`'s instance 19 KB and its thread, the client's 8 KB thread and 4 KB pool), not a handshake beside another: the lowest comes during the first metrics export, before the broker's handshake. With the held block counted, the heap's own lowest is 1 to 10 KB on every chip, with or without MQTT. On the C3 with MQTT connected, a metrics export goes down to 6 to 10 KB, and the first one or two after a restart fail for want of memory (a 4.4 KB allocation); the later ones are sent. In a repeat on C3 rc12 with the same two apps, MQTT connected before the first two successful exports after restart. Their minimum internal heap was 53,412 and 51,520 B. The gateway reported one cumulative skip. The C5 goes down to 9 to 15 KB and sends them. With Zephyr on the ESP32-S3, 4,112 bytes of the 372 KB DRAM segment are left at link time, with a 12 KB status-thread stack and a 92 KB kernel heap.

On a physical C3 at `ec3cf3a`, the largest free block before starting `alarm` and `mqttdemo` was 90,112 B. The exporter deferred its first attempt at 7 s while the agent was busy. MQTT was connected for the next five exports, which all succeeded. The gateway reported one skip. The two minimum-heap columns show the allocator's raw reading and the agent's reading adjusted for its temporary TLS reservation.

| Uptime at export | Largest block before export | Minimum internal heap (ESP-IDF) | Minimum internal heap (agent) |
|---:|---:|---:|---:|
| 18 s | 77,824 B | 15,672 B | 60,636 B |
| 49 s | 61,440 B | 27,000 B | 60,528 B |
| 80 s | 61,440 B | 28,412 B | 59,704 B |
| 111 s | 61,440 B | 27,740 B | 56,248 B |
| 142 s | 61,440 B | 27,756 B | 58,704 B |

What the agent took of the internal heap once an app's instance was made, up to a minute after, with a metrics export every 30 s; the block held while a TLS connection opens left out ([APPS.md](APPS.md#size-and-limits)). The 40 KB the device keeps for the agent comes from these:

| Board | `alarm` (v3.14-rc12) | CoreMark's 64 KB build (v3.14-rc12) | Left at the lowest with CoreMark |
|---|---|---|---|
| Classic ESP32 | 21 to 25 KB | 35 KB | 10 KB |
| ESP32-C3 | 24 to 28 KB | 39 KB | 24 KB |
| ESP32-C5 | 21 to 27 KB | 33 to 34 KB | 7 KB, and two status updates failed |
| ESP32-C6 | 21 to 27 KB | 34 to 47 KB | 87 KB |
| ESP32-S3 N4R2 | 23 to 37 KB | 33 to 52 KB | 33 KB |

Without a metrics export the agent took 12 to 16 KB (v3.14-rc7): reconnecting and sending its status; an export adds 10 to 15 KB, a spec applied meanwhile up to 38 KB in all, an MQTT connection about 10 KB, a console session up to 25 KB while it is open, its buffers included ([CONSOLE.md](CONSOLE.md#limits)). On the S3 the TLS block is never held (its largest block is in PSRAM), so its lowest is the heap's own: 18 KB taken with no app and no export (v3.14-rc7). Since v3.14-rc12 the apps saved by a previous run start once the device's first status is accepted, 4 to 9 s after the firmware starts on the boards; before, they started before WiFi, with the heap the network and the agent take later still free.

Stack used at most (`stackMinFreeBytes`), of the stack each thread has; here a KB is 1,024 bytes:

| Firmware | Spec loop, on the main task | Status thread | App thread |
|---|---|---|---|
| Classic ESP32 | 8.9 of 24 KB (installing an app) | 4.2 of 12 KB | 3.4 of 8 KB |
| ESP-IDF v3.14-rc10 and rc11, the five chips | 8.3 to 9.1 of 14 KB (C3, C5, C6) or 16 KB (ESP32, S3), downloading a whole firmware or a delta | at most 5 KB, of 8 KB (C3, C5, C6) or 10 KB (ESP32, S3) | |
| Zephyr on the ESP32-S3 | 5.4 of 16 KB (polling, no firmware download) | 4.7 KB (now given 12 KB; no certificate renewal) | 3.0 of 8 KB (`alarm`) |
| Zephyr on a Cortex-M3 (`mps2/an385` in QEMU) | | | 2.7 of 8 KB (`printf` of `%f`); 2.5 without the C library |
| ESP32-S3-mini, ESP-IDF v3.14-rc12 | | | 3,636 B used, 4,556 B free of 8,192 B with `libccheck` alone and all checks passing |
| ESP32-C3 in the emulator, v3.14-rc4 | | | 2.8 of 8 KB (`printf` and `snprintf` of `%f`) |
| ESP-IDF with the `alarm` app | | | 3.4 of 8 KB on the classic ESP32 and the S3, 2.5 to 2.6 on the C3, C5 and C6 |

Installing an app or downloading firmware takes about twice the stack of polling. `printf` of `%f` adds little to an app's thread (0.2 KB on the Cortex-M3). The C3 with the `alarm` app uses 1% of its CPU.

### CoreMark

With the two-slot firmware, ten swaps under load on classic ESP32 started CoreMark ten times (14.09–14.24 iterations/s). The C5 started seven times (17.6–22.3 iterations/s). All completed runs passed CRC. The three refusals on C5 had largest free blocks of 58–62 KiB for a 64 KiB allocation. Classic ESP32 had six failed status updates and C5 had four, all subsequently accepted on C5. These measurements precede the idle-priority and app retry changes.

The classic ESP32 repeated the same swaps on successive builds. All completed runs passed CRC, and none logged a task watchdog.

| Change included | Completed | Iterations/s | Secondary resumed after CoreMark | Failed status attempts |
|---|---:|---:|---|---:|
| Idle priority, retry from saved module | 10/10 | 8.948–9.143 | 8–35 s, median 18 s | 14 |
| Wake secondary on primary completion | 2/2 | 9.156–9.215 | Same logged second | 1 |
| Drain the successful status response | 3/3 | 9.010–9.236 | 0–1 s | 2 |
| Allow a failed SDK receive allocation to use the reserved TLS record | 3/3 | 9.016–9.167 | 13, 1 and 1 s | 0 |

Nine of the initial fourteen failures followed a TLS read reporting insufficient memory. A successful status PUT echoes the whole device object. Streaming a 5,596 B response to completion saved an 8,192 B buffer in a host test. The two subsequent board failures came from a 6,248 B ESP-IDF TLS receive allocation. The last build in the table lets that allocation borrow an unused reserved TLS record when internal RAM is fragmented. The 13 s wakeup in that run occurred with old console entries still listed. The agent now checks once per second during that polling delay while a one-shot primary is running and another app is waiting for it.

The ESP32-C5 completed all ten swaps on a single boot at 9.022–11.156 iterations/s, with ten valid CRCs, no task-watchdog reports and no reset. The secondary resumed 90–1,090 ms after completion. Five status PUT attempts failed and were retried. The device applied all ten removal specs. On the ninth swap a 64 KiB linear allocation encountered a 60 KiB largest block. Closing idle management TLS increased free RAM from 106,008 to 122,860 B while the largest block stayed at 61,440 B. Unloading the saved module allowed its replacement instance to start on the same spec 30.12 s after the refusal, with 46 KiB left for the agent. Two earlier ten-swap trials each completed nine swaps and kept one primary waiting for at least eight minutes.

All five failed C5 PUTs followed a hardware AES allocation failure during a TLS write. With small TLS allocations (up to 2 KiB, excluding receive records) placed preferentially in RTC FAST memory, an instrumented C5 repeated the same ten swaps. All ten passed CRC with no allocation refusal or reset. Across 41 status attempts, 39 returned HTTP 200 and two returned HTTP 429. The next retry was accepted, and no TLS write failed. The secondary resumed in 60–1,010 ms. Median CoreMark was 9.830 iterations/s, compared with 9.841 before the allocator change.

#### Memory placement on C5 and C6

A controlled comparison installed the identical 7,769 B wasm3 module three times per boot on one 160 MHz C6. The four release blocks were v3.10, v3.12, v3.12 and v3.10. Their median scores were 12.253, 11.983, 11.783 and 12.200 iterations/s respectively. All twelve runs passed. Across both boots v3.10 ranged from 12.189 to 12.264, and v3.12 from 10.911 to 12.204. In the two releases the fast interpreter's dispatch had the same opcodes and registers on each chip (6,696 instructions on C5, 7,502 on C6). Its linked address and some immediate operands differed.

On C5 and C6, v3.12 reserves the largest free heap block while WAMR loads a module and again while creating its thread. This keeps contiguous space for the app's linear memory. It can also place the interpreter's decoded bytecode or the thread's native stack in slower RTC FAST memory. The 64 KiB linear memory itself stayed in main SRAM in every instrumented run.

With the same wasm3 module installed eight times per board on an instrumented v3.12, four loads held the largest free block and four left it available, in two A/B/B/A cycles. The thread used the usual v3.12 allocation policy throughout. Task time was sampled at the module's timing calls and includes interrupts charged to the running task.

| Board | Largest block during module load | Installs with decoded bytecode in RTC | Iterations/s | Task ms/iteration |
|---|---|---:|---:|---:|
| C5 | Held | 4/4 (3,236–7,514 B) | 15.824–18.001 | 55.0–61.0 |
| C5 | Available | 0/4 | 18.736–19.577 | 50.5–50.6 |
| C6 | Held | 1/4 (2,328 B) | 11.604–12.074 | 82.1–85.2 |
| C6 | Available | 0/4 | 11.763–11.797 | 83.9 |

Capping both temporary reservations at 72 KiB for this 64 KiB app kept all decoded bytecode in main SRAM across four further installs per board. With the SDK's usual stack allocation, the C5 placed all four native stacks in RTC FAST memory (18.693–19.640 iterations/s) and the C6 placed all four in main SRAM (12.050–12.090).

A final comparison kept the same cap and selected the heap for the native stack at task creation in main/RTC/RTC/main order. On the C6 the decoded bytecode stayed in main SRAM in all four installs. With the stack in main SRAM, CoreMark scored 12.047 and 12.088 iterations/s (81.9–82.1 task ms/iteration). With the stack in RTC FAST memory it scored 11.809 and 11.821 (83.7 task ms/iteration). These placements account for a repeatable part of the release difference. The original release runs did not record allocation addresses.

On the C5 the same stack selection exposed the cost in contiguous RAM. Both main-SRAM stack requests left a largest block of 62 KiB and the module could not start. With the stack requested in RTC FAST memory, one install passed at 18.358 iterations/s with all decoded bytecode in main SRAM, and one was refused with a 64 KiB largest block. The v3.12 allocator needed 68 KiB for this module. The 72 KiB reservation cap was specific to that benchmark.

On ESP-IDF, C5 and C6 builds without PSRAM can leave main SRAM available for decoded bytecode and the native stack by reserving a block sized for the module's linear memory. [Allocation policy and fallback](APPS.md#size-and-limits)

The same physical boards each ran six installs of the same wasm3 module per firmware, removing the app between installs and staying on one boot per series. Both builds used ESP-IDF 6.0.3, WAMR 2.4.5, size optimization for the firmware and `-O2` for the interpreter. CoreMark ran alone at the agent's current app priority with metrics disabled. The instrumented builds compared the reference policy at `c10e3a6b` with the policy in `f208b530`. SRAM placement and scores below count completed runs.

| Board | Firmware | Completed | All decoded bytecode in main SRAM | Native stack in main SRAM | Median iterations/s | Range |
|---|---|---:|---:|---:|---:|---:|
| C5 | Reference | 5/6 | 3/5 | 4/5 | 11.104 | 7.679–11.124 |
| C5 | Module-sized reservation | 6/6 | 6/6 | 6/6 | 11.085 | 10.994–11.128 |
| C6 | Reference | 6/6 | 6/6 | 5/6 | 6.027 | 5.941–6.055 |
| C6 | Module-sized reservation | 6/6 | 6/6 | 6/6 | 6.030 | 5.995–6.055 |

On the reference C5, the two slowest runs placed 6,676 and 4,382 bytes of decoded bytecode in RTC FAST memory. One install was refused with a largest free block of 36 KiB. With the new policy WAMR used the reserved 65,536 B block in all twelve installs. Immediately before allocating app handles and stack, the firmware reported a largest free block of 64 KiB in four C5 installs and one C6 install, measured in whole KiB. The C5's slow outliers disappeared in this series. Median throughput on each board was close to its reference.

#### Scores by board

Iterations per second of [CoreMark](https://github.com/eembc/coremark) 1.0's two published WebAssembly builds, unmodified, on WAMR's fast interpreter ([APPS.md](APPS.md#modules-built-for-no-device)). ESP-IDF v3.14-rc12 built for size, swapped in for the `alarm` app; the 128 KB build with v3.13. Every run was valid (its CRCs right, a timed part of 10 s or more), with the agent connected and metrics sent every 30 s or none:

| Board | wasm3's build (64 KB of memory) | wasm-coremark's build (128 KB) | Native, one core (Espressif) |
|---|---|---|---|
| Classic ESP32 (240 MHz) | 14.23 | does not fit: largest free block 64–76 KB | about 505 |
| ESP32-C3 (160 MHz) | 14.21 | does not fit: largest free block 36–38 KB | about 409 |
| ESP32-C6 (160 MHz) | 12.10 | 13.24–13.71 | |
| ESP32-C5 (240 MHz) | 19.70 (18.9–22.3 across installs in the earlier release measurements) | does not fit: largest free block 72 KB | |
| ESP32-S3 N4R2 (2 MB quad PSRAM) | 18.09 | 21.04–21.64, in PSRAM | about 614 |
| ESP32-S3, PSRAM off | 18.88–18.95 (v3.10, built for speed) | does not fit: no internal block of 128 KB left once running | about 614 |
| ESP32-S3 N8R8 with Zephyr, PSRAM disabled | 17.885 in internal DRAM (one run) | – | |
| ESP32-S3 N8R8 with Zephyr | 17.83–17.84, in PSRAM (z26) | 21.69–21.70, in PSRAM (z26) | about 614 |

| | |
|---|---|
| Against native code | about 30 times slower, the instruction count included; WAMR's AOT mode should be about 3 times slower, with one module per architecture instead of one for every device; not measured |
| Firmware built for size, WAMR still at `-O2` | about 5% slower than built for speed on the S3, 7% on the classic ESP32, nothing on the C5 and C6 |
| Classic ESP32 at 240 MHz | 2.2 times faster than at ESP-IDF's debug default of 160 MHz |
| Contiguous block for CoreMark linear memory on ESP-IDF | 64 KiB for the wasm3 build, 128 KiB for the wasm-coremark build. The earlier allocator needed 68 KiB and 136 KiB respectively. |
| ESP32-S3 with PSRAM | an app's memory goes to PSRAM when no internal block is large enough: the `esp32s3` image for quad PSRAM or none, `esp32s3-r8` for octal ([DEVICES.md](DEVICES.md)) |
| Zephyr on the S3 N8R8 | the octal PSRAM is a heap of its own for app memories of 16 KB or more; its driver takes 4 KB of internal RAM |
| Zephyr on the S3 N8R8 with PSRAM disabled | The 64 KiB wasm3 benchmark ran with a 140,592 B C heap and a 79,872 B kernel heap. This gives the C heap 20 KiB more, using 14 KiB from the kernel heap and 6 KiB of DRAM previously free at link time. The instrumented build left 1,392 B free at link time. |

The N8R8 ran CoreMark as its only app, with MQTT and telemetry export off. Both the application and MCUboot were built with PSRAM disabled. A runtime check placed the C heap arena in internal DRAM.

| CoreMark on Zephyr, internal RAM | C heap free | Largest free C heap block | Kernel heap free |
|---|---:|---:|---:|
| Before loading the WASM module | 105,036 B | 93,284 B | 29,216 B |
| Before creating the WAMR instance | 92,940 B | 79,748 B | 23,400 B |
| After creating the instance | 26,400 B | 14,204 B | 23,400 B |

Flight Control received the completed score at 62 s uptime and another status at 122 s with the same score. The device then applied a new spec replacing CoreMark with `alarm`. At 128 s it reported `alarm` running and the new spec up to date. All three reports carried the same boot ID.

With the earlier ESP-IDF allocator, the 64 KiB build used about 105 KB of heap in total. The loaded module took about 18 KB, its thread stack 8.7 KB and its instance about 71 KB including the 68 KiB block. In v3.14-rc12 the allocator could serve the same linear memory from a 64 KiB block. The following swaps used that release with a 40 KiB heap floor for the agent.

| | |
|---|---|
| Swapped in for `alarm` | run on all five chips |
| Classic ESP32 | refused once in three swaps (`would leave the agent 30 KB`) |
| ESP32-C5 | run with 7 to 11 KB left to the agent at the lowest, and two status updates failed meanwhile; added beside a finished CoreMark, `alarm` was refused (`layer is 2303 bytes (max 1641 with this heap)`) |
| Lowest heap seen, earlier releases | 2.3 KB on the classic ESP32, the agent still reporting; 108 bytes on the C3 after a failed swap, where one TLS handshake failed (`PSA_ERROR_INSUFFICIENT_MEMORY`) |

### ESP32-S3 virtual wiring

With ESP-IDF 6.0.3 and esp-emu 1.1.0, the ESP32-S3 app hardware drivers passed the same wiring test on macOS and Linux. The test firmware calls the port's drivers directly. [Test circuit](../tests/espidf-hw/board.json)

| Interface | Observed result |
|---|---|
| GPIO interrupts | Pressing and releasing the virtual button on GPIO5 delivered the falling and rising edges, with input levels 0 and 1. |
| I²C | The SHT31 at address `0x44` returned 22.5 °C and 45% relative humidity. Both CRC bytes matched. |
| SPI | The TMP123 returned `0x0c80` (25 °C). Moving its chip select from GPIO10 to GPIO14 produced `0xffff` and failed the temperature check as expected. |
| UART | The MH-Z19B reply triggered the receive callback and reported 400 ppm CO₂ with a valid checksum. |
| PWM | At a configured 50 Hz, the virtual servo on GPIO6 observed 999 µs and 1,999 µs for commanded widths of 1 ms and 2 ms. |

The emulator has no ADC model for the S3. Electrical measurements on wired boards remain open, including RS-485 driver-enable timing and peer turnaround.

### Apps on the boards

The example apps ([APPS.md](APPS.md#examples)), with ESP-IDF unless the row says otherwise:

| App | What it did |
|---|---|
| `alarm` | Opened `led0` with the firmware's pin on the C3, the C6 (its WS2812 LED) and the ESP32-S3 with Zephyr (LED and temperature simulated); a new threshold took it from alarm to ok without restarting it; on the C6 it came back from flash after a restart. It also runs on the classic ESP32. |
| `hwcheck` | With nothing wired, I2C (a bus scan, a transfer), SPI, a UART write, ADC (a floating pin) and PWM open and answer on the classic ESP32, C5, C6 and S3 and on the emulated C3. GPIO interrupts and wired devices are not tried yet. |
| `mqttdemo`, the five chips | Beside `alarm`, against a broker with TLS on the LAN: published the temperature every 5 s with none missing over 8 minutes, and received and answered a command on the device's prefix (QoS 1) and one on the fleet's. |
| `mqttdemo`, Zephyr on the S3 | Published every 10 s with none missing over 45 minutes, and answered a command on each prefix within a second. With internal TLS records the N8R8 deferred exports for lack of C heap. With both receive records in PSRAM and 32 POSIX mutexes, the telemetry gateway received a new sample while MQTT was online. |
| `tinyml` | Said `steady` on the C3, C5, C6 and S3, and `warming` when the C3 warmed by 2 °C within a minute. The classic ESP32 has no temperature sensor, and the app says so. |
| `tinymlp` | With the device's `expf`, `logf` and `tanhf`, on the C3, C5, C6 and S3: `steady` at rest, 95 to 100% sure, once in a while `warming` or `cooling` at under 95% (`warming` at 46% once, `cooling` at 93%). |
| wasm3-arduino's blink, Rust build, beside `tinyml` | Ran for 45 minutes on the C3, C6 and S3. Refused on the C5 (`allocate linear memory failed (largest free block 52 KB)`) and on the classic ESP32 (`its memory would leave the agent 22 KB of heap, less than the 40 KB it needs`), the device reporting all along. |
| wasm3-arduino's blink, C++ build, beside `alarm` | Ran on the C3, C6, S3 and the classic ESP32. The blink also runs on the host and on Zephyr's `native_sim`. |

Zephyr `native_sim` passed eight hardware API tests covering devicetree names, GPIO, ADC, PWM, I²C, SPI, UART and RS-485. The RS-485 fixture held the driver-enable signal active while the FIFO was empty and the shift register was still sending. A link with an ESP32-S3 UART1 and a driver-enable GPIO used 377,248 of 380,672 B of DRAM, leaving 3,424 B. Electrical timing and peer turnaround remain unmeasured.

On the Zephyr S3 N8R8, `mqttdemo` answered four distinct QoS 1 commands during reception of a 1,037,990 B signed firmware image. The largest encrypted TLS record observed during this transfer was 16,401 B. The broker retained one connection until the firmware reboot, and the new boot reported the expected digest.

On an ESP32-C6 running v3.14-rc12, `mqttdemo` answered distinct QoS 1 commands before and after the signed 2,261 B `trend` download. The broker recorded a single device connection throughout the transfer. Both apps were running afterward on the same boot.

On the ESP32-S3-mini with ESP-IDF v3.14-rc12, repeated 100 ms native waits in one call tripped the one-second limit. The device reported the exception and two app restarts while its firmware stayed online.

On a provisioned ESP32-S3-mini at `ec3cf3a`, the same firmware withheld management responses for 400 s on three boots. Each run started the saved primary before the first accepted status.

| Boot and primary | Restore started | Primary running | Secondary | First accepted status | Minimum internal heap |
|---|---:|---:|---|---:|---:|
| Power cycle, fixed-size primary | 180.352 s | 180.371 s | Running at 188.972 s | 413.509 s | 93,948 B |
| Power cycle, primary grew from one to three pages | 180.422 s | 180.476 s | Parked throughout and after reconnection | 409.528 s | 84,016 B |
| Software reset, primary grew from one to three pages | 180.362 s | 180.417 s | Parked throughout and after reconnection | 407.230 s | 84,160 B |

On an instrumented ESP-IDF S3-mini at `ec3cf3a`, one spec changed the primary app's environment and updated the secondary. The minimum internal heap during the primary restart and secondary reopen was 54,784 B by both the ESP-IDF allocator and the agent, leaving 13,824 B above the 40 KiB admission floor. The secondary installation completed with a minimum of 84,736 B. The device applied the spec 2 s after deferring it. A second run measured the whole spec over 77 s. Both low-water counters and an independent 5 ms sampler reported 59,880 B free at the lowest, 356 ms into spec processing, between applying the config and updating the apps. The secondary installation in this run reached 67,832 B.

### Timings

ESP-IDF boards with an early build, unless the row says otherwise:

| Operation | Time |
|---|---|
| From boot to enrollment request (WiFi, SNTP, TLS) | about 10 s |
| Config change, from apply to device confirmation | 1–6 s |
| Firmware rollout to 3 devices, canary first | 50–80 s, about 20 s per device for download and reboot |
| Rollback of a broken firmware | about 3.5 min: the 150 s confirmation window and 2 reboots |
| WASM app update across the fleet, canary first | about 50 s, nearly all of it the rollout; the swap on the device is immediate |
| App state change, to Flight Control | about 2 s |
| Memory rule added to the fleet, to `Warning` and `DeviceMemoryWarning` (end-to-end test) | 21 s; `DeviceMemoryNormal` once the rule is removed |
| MQTT connected, from the spec that gives the broker (v3.14-rc6) | within 70 s; 13 s with Zephyr on the S3 |
| Firmware and an app that needs it, in one spec, to the five ESP-IDF chips, canary first | about 2 minutes |
| An app's call budget, 5 million instructions (a counting loop, five runs each, within 5%) | 0.66 s on the C5, 0.9 s on the S3, 1.06 s on the C3, 1.2 s on the classic ESP32, 1.3 s on the C6; with the second in the device's functions, at most 2.3 s on the C6 |
| A loop in a C constructor, emulated C3 | stopped by the budget in under a second, the app removed in 3 s |
| Decommissioning, from the request to `Completed` | 2.4 to 3 s |
| A whole firmware, from `Downloading` to `Rebooting` (v3.14-rc10 to rc10.1, polled every 3 s) | 13 s on the C3, 9 s on the C5 with fewer WiFi receive buffers (9 and 6 s before), 13 s on the C6 |

On a physical ESP32-C3, a test build deliberately refused to start a console thread after its stream opened. The remote client received `could not start the console thread` and exited with status 1 in 0.927 s. The next request completed with status 0.

With `alarm` and `blinkcpp` running on the physical ESP32-C3, five consecutive console commands (four `status` and one `apps`) returned output and exited successfully in 0.261–0.361 s. The `apps` command reported both apps running. All five ran on the same boot and desired spec, with 19 outstanding console entries. During the commands, the rendered device response measured 13,073–13,077 B.

The agent reads that response in 1 KiB pages and releases them before opening the console. With the earlier contiguous buffer, a client under the same two-app load timed out after 60.135 s. The device had 22,364 B free, but its largest block was 7,680 B and it needed a 12,289 B allocation to read a 12,781 B response.

### Idle traffic

Bytes on the wire, TLS included (`netTxBytes`, `netRxBytes`), with the earlier HTTP/1.1 client and its two kept connections:

| | Traffic |
|---|---|
| Simulated device, no app, a status every minute | about 220 KB/hour: 87 KB sent, 130 KB received |
| ESP32-C3 with the `alarm` app | about 280 KB/hour |
| A connection per request instead | 560 to 730 KB/hour, mostly the TLS 1.3 handshake with mTLS |
| Exchanges | a status at once on a change, else every 2 minutes; the long-poll lasts about 4 minutes |
| TLS handshakes | 2 after boot, none in the next 10 minutes |
| A reconnection | a full handshake, about 1.5 KB received: the server issues session tickets but does not resume them |
| After a server restart | both connections reopen; the next spec applies in 2 s |

On the ESP32-C3 with `alarm`, passive socket counters measured the persistent HTTP/2 management connection, including TLS.

| ESP32-C3 sample | Duration | Sent by the board | Received by the board |
|---|---:|---:|---:|
| First measurement | 301 s | 14,676 B | 53,604 B |
| Repeat on v3.14-rc12, 11 outstanding console entries | 330 s | 14,926 B | 54,004 B |
| Empty console list, fresh identity | 330 s | 3,964 B | 10,079 B |

The empty-list window corresponds to **153 kB/hour**. All 23 samples showed the same boot, spec and connection. Only `alarm` was running, telemetry export was off, and the device sent two status updates on its 120 s heartbeat. The window with 11 outstanding entries corresponds to 752 kB/hour. Those entries cause Flight Control to return the spec immediately. The earlier HTTP/1.1 result of about 280 kB/hour used the same heartbeat but included two server restarts over ten minutes. [Console session lifetime](FLIGHTCTL-LIMITATIONS.md#15-a-console-session-whose-user-left-before-the-device-came-stays-listed)

On an ESP32-S3 N8R8 with Zephyr, six minutes of idle management traffic with no apps, telemetry exports or console commands added 11,194 B sent and 42,660 B received (537 kB/h extrapolated). Status updates ran approximately once a minute. The handshake count stayed at seven throughout this window. The C3 and Zephyr runs have different device and status configurations.

What is left is the full status, and going lower needs differential updates (limitation 6). The apps' changes go out together, one status every 15 s at most.

With PSRAM disabled on the Zephyr N8R8, the following measurements cover the HTTP/2 management connection, including TLS. No apps, MQTT broker or telemetry export were configured.

| Console entries | Duration | Sent by the board | Received by the board | Extrapolated traffic |
|---|---:|---:|---:|---:|
| 15 outstanding | 330 s | 9,868 B | 30,891 B | 445 kB/hour |
| Empty | 668 s | 19,186 B | 35,397 B | 294 kB/hour |
| Empty, final 330 s of the same run | 330 s | 10,592 B | 19,464 B | 328 kB/hour |

The 15 entries kept Flight Control from holding the spec long-poll open. A fresh device identity provided the empty-list baseline. All 46 passive samples in that run showed the same boot, rendered spec and TCP connection. The TLS handshake counter stayed at five throughout the final 330 s.

### In Espressif's emulator

The ESP32-C5, C6 and S3 each passed the end-to-end tests with ordinary and `secure` firmware at commit `ec3cf3a`. On all six runs, the device reported `ota_0` at enrollment and `ota_1` after the signed update, with the release confirmed and the app running. After a broken update, each device returned to `ota_1` and reported the rollback. The checks observed the confirmed release after 21–34 s and the rollback after 55–70 s, measured from the start of each wait. [End-to-end run](https://github.com/francescopace/flightctl-mcu/actions/runs/44357365823)

On the host, Zephyr `native_sim` and Zephyr Arm QEMU, a Git-managed fleet delivered the registry CA and credentials through Kubernetes Secrets after the inline copies had been removed. All three devices pulled and ran the app, then completed decommissioning. The host's factory reset also removed the app and erased its key and certificate. [Host](https://github.com/francescopace/flightctl-mcu/actions/runs/44357365823/job/134989082574) · [Zephyr native_sim](https://github.com/francescopace/flightctl-mcu/actions/runs/44357365823/job/134989082618) · [Zephyr Arm QEMU](https://github.com/francescopace/flightctl-mcu/actions/runs/44357365823/job/134989082724)

The figures below were measured with [esp-emu](https://github.com/espressif/esp-emulator) 0.51.0 and 4-minute certificates against the same Flight Control. The C3 ran by hand. The workflow covers the C5, S3 and C6 ([TESTING.md](TESTING.md)).

| Chip | Signed OTA (reboot into the other slot) | Unsigned app | Certificate renewal | Heap free / minimum / largest block |
|---|---|---|---|---|
| ESP32-C3 | confirmed, 19 s | refused | 6 renewals, still reporting after the first certificate expired | 57.8 / 37.6 KB / – |
| ESP32-C5 | confirmed, 15 s | refused | renewed, still reporting after the first certificate expired | 61.7 / 42.2 / 40.9 KB |
| ESP32-S3 | confirmed, 35 s | – | renewed | 121.3 / 102.1 / 73.7 KB |

The C3 figures match the board's.

### The way back on the ESP32-S3, both ports

Each port was given a firmware that reaches Flight Control on a wrong port (9443) and gives itself 30 seconds instead of 150:

| | ESP-IDF 6.0.3 | Zephyr 4.5 with MCUboot |
|---|---|---|
| Before | the release build, received as a delta ([DELTA-UPDATES.md](DELTA-UPDATES.md)) (`lastDelta: Applied`) | a build received as a delta of 28 KB (`lastDelta: Applied`) |
| Broken image | 1181 KB, whole, downloaded and restarted in 11 s | 885 KB, whole, in 14 s |
| Given up | 30 s after the agent started, by the image itself | the same |
| Back | the bootloader takes the previous slot | MCUboot swaps the slots back: 15 s |
| Reported | 53 s after the fleet changed: "failed to start and was rolled back; not retried" | 1 min 53 s, the same message |

![A firmware that failed to start, rolled back](img/rollback-events.png)

*The device's events when a release does not start: the device reboots into it, goes back to the one it ran, and Flight Control reports the update failed.*

Neither tried the image again. The ESP32-C6 was back and reporting 50 s after it restarted into a broken firmware, and with v3.13 online again 3 to 5 s after the restart back.

### A crash in the status

After a crash, the status gives `lastFatalError`: the fault, the task or thread, the uptime, the `pc` and the callers, for `addr2line` against the ELF of the firmware that crashed. `tools/fw-push.py` keeps each release's ELF in the registry, and `tools/fw-elf.py` gets it back. With ESP-IDF the panic handler records a fault, an `abort()`, the interrupt watchdog, and the task watchdog when it is set to panic (by default it only logs). On Xtensa the record has the backtrace; on RISC-V, which has no frame pointers, `ra` and the firmware's code addresses found on the stack. Zephyr gives the same text with the `pc` and `lr` on Arm, and the `pc` and Zephyr's own backtrace on the ESP32-S3.

A physical ESP32-S3 and two emulated targets enrolled in Flight Control 1.4 each retained a native coredump across a crash and reboot. Two downloads per device through `flightctl console --notty` matched byte for byte. The ELF retrieved by release digest and platform matched the build's ELF in each case. [Download and decoding commands](CONSOLE.md#download-a-crash-dump)

| Target | Bytes downloaded | What the debugger recovered |
|---|---:|---|
| ESP32-C3, emulated, ESP-IDF 6.0.3, no WASM apps | 11,200 | `esp-coredump` located the fault in `fc_fatal_test_fault` and its caller `fault_task` |
| ESP32-C3, emulated, ESP-IDF 6.0.3, `alarm` and `trend` running | 14,080 | The same two frames from a dump occupying 21.5% of the 64 KiB partition |
| MPS2 AN385, QEMU, Zephyr on Arm Cortex-M3 | 49,371 | GDB located the console test fault in `command` and unwound through `pipeline`, `exec_script` and `run` |
| ESP32-S3 DevKitC N8R8, physical, Zephyr 4.5 on Xtensa, `alarm` running | 8,535 | GDB matched PC `0x42010604` to the test fault in `command` and unwound through `pipeline`, `exec_script`, `run`, `session_thread`, `zephyr_thread_wrapper` and `z_thread_entry` |

On the C3 with both apps installed, `flightctl console` reported 569,920 B free out of 576,576 B usable in the 576 KiB NVS data partition. Repeating the crash with a 32 KiB dump partition also saved and transferred 14,080 B. The factory layout reserves 64 KiB to allow for larger task stacks at the time of a fault.

The Zephyr ESP32-S3 DevKitC build with the flash backend linked with 3,872 B of DRAM to spare.

In the emulator against Flight Control ([tests/e2e/fatal-emu.sh](../tests/e2e/fatal-emu.sh)), with test builds that crash from a task 30 s after power-on:

| Chip | Crash | `lastFatalError` |
|---|---|---|
| ESP32-C5 | write to address 0 | `Store access fault (7) in thread "fc_fault", 30112 ms after boot, pc 0x4200d96a ra 0x4200d994` |
| ESP32-C3 | write to address 0 | `… (7) …, pc 0x4200cb7e ra 0x4200cba8` |
| ESP32-S3 | write to address 0 | `StoreProhibited (29) …, pc 0x420c2d5c backtrace 0x4200ecda` |
| ESP32-C5 | `abort()`, as a failed assert makes (also run on the C3, C6 and S3) | `abort() (2) …, pc 0x4200d973 ra 0x4080c926, stack 0x4080b76e 0x4200d976 0x4200d9a2`: the `pc` is abort()'s caller; the log has the assert, the function and the task |
| ESP32-C5 | interrupt watchdog (a loop with interrupts off) | `Interrupt wdt timeout on CPU0 (24) …, pc 0x4200d97a ra 0x4200d978, stack 0x4200d9b0`, `resetReason` `int_wdt` |
| ESP32-S3 | interrupt watchdog | `… (5) …, pc 0x420c2d5e backtrace 0x4200ecda` |
| ESP32-C5, S3 | task watchdog set to panic (a loop that starves the idle task) | `task watchdog (0) …`, the `pc` in the loop, `resetReason` `task_wdt` |

In every run the `pc` decodes to the function that crashed, and its caller, the task's function, is among the addresses. The device crashed once and went on. The previous boot's log (`journalctl -b -1`, through the console) ends with `fatal: ` and the same text. A release's ELF, pushed with four platforms (ESP-IDF and Zephyr), came back byte for byte and left the release's index unchanged. The unit tests check that the record is valid before it is believed, and its text.

On a physical ESP32-S3 running Zephyr with MCUboot, a CPU exception at uptime 21,822 ms produced a new boot ID, `resetReason=panic` and a `lastFatalError` containing the PC and four return addresses. Against the same build's ELF, the PC resolves to the enabled crash-test command, followed by its command interpreter, session thread and Zephyr thread wrappers. The port keeps the 140 B record in RTC memory across warm resets.

The handler takes 799 bytes of IRAM on the ESP32, 907 on the S3 and 1,102 on the C3, C5 and C6; the record takes 140 bytes of RAM kept across restarts.

### The registry with TLS, and decommissioning, on boards

With the registry behind TLS and a password, its CA and `auth.json` from the fleet ([REGISTRY.md](REGISTRY.md)):

| | ESP32-C3, ESP-IDF 6.0.3 | ESP32-S3, Zephyr |
|---|---|---|
| App over TLS | running 3 s after the config, no restart | about 6 s |
| Firmware over TLS, whole | 1234 KB in 5 s | 886 KB |
| Firmware over TLS, as a delta | not tried | 17 KB (`Applied`) |
| Decommissioning, from request to Completed | 3 s | 2.4 s |

After decommissioning each board wiped its key and certificate, showed as `Decommissioned` and kept running its app without enrolling again; erased, each enrolled again as a new device. The heap during a download over TLS is not measured yet.

### Zephyr on the ESP32-S3

An ESP32-S3-DevKitC with Zephyr 4.5 and MCUboot ([ZEPHYR.md](ZEPHYR.md#on-an-esp32-s3)), against Flight Control 1.4.0:

| | |
|---|---|
| Enrollment | key made by PSA from the chip's random generator, certificate for a year |
| WASM app | the signed `alarm` app runs (WAMR's fast interpreter on Xtensa); a copy signed with another key was refused |
| Firmware, whole | 885 KB in 7 s, 24 s from the restart to the agent, MCUboot's swap included; confirmed |
| Firmware, as a delta | 19 KB (2.2%) rebuilt the 885 KB image in 8 s; confirmed |
| A firmware that cannot reach Flight Control | rolled back ([The way back](#the-way-back-on-the-esp32-s3-both-ports)) |
| Reflashed over USB | same device: its key is in the storage partition |
| Memory | the C heap with an app and metrics every 30 s: [RAM](#ram); a signed image with MQTT linked in is 1,038 KB |
| Downloads over HTTP/2 | a delta and the `mqttdemo` app: manifests, signatures and blobs, every request HTTP/2 in the registry's log |
| MQTT | [Apps on the boards](#apps-on-the-boards) |
| Without PSRAM | with one app, the C heap got down to 368 bytes while downloading an update, which failed (`HTTP -1`); the spec then stopped coming in while statuses went out, until a reset |
| N8R8 with PSRAM disabled in firmware | The signed image was 1,038,022 B, with 8,696 B of DRAM free at link time. A runtime check placed the C heap arena in internal DRAM. With the broker connected before starting `mqttdemo`, the app answered a QoS 1 command. At an export attempt only 32,012 B were free against the 61,440 B gate, and the gateway received no sample from that firmware. |
| The UI's Logs tab, metrics | the tab's commands answered (every filter, live mode); the device's metrics at the Telemetry Gateway ([CONSOLE.md](CONSOLE.md), [METRICS.md](METRICS.md)) |
| Field provisioning | from an iPhone with Espressif's app ([PROVISIONING.md](PROVISIONING.md#on-zephyr)) |

## Flight Control limitations

The gaps found in Flight Control for this class of device, each with a proposal, are in [FLIGHTCTL-LIMITATIONS.md](FLIGHTCTL-LIMITATIONS.md); this page cites them by number.

## Current limitations

- **Registry**: TLS only, with the CA the fleet gives; no public CA bundle, no redirects to another host ([REGISTRY.md](REGISTRY.md)).
- **Device key**: in flash in clear, except in the `secure` variant, where it is encrypted, or on the C5 in its ECDSA peripheral ([SECURITY.md](SECURITY.md)).
- **Provisioning**: credentials over USB; the WiFi network over USB or the device's own access point ([PROVISIONING.md](PROVISIONING.md)).
- **Enrollment certificates**: issued per batch, but Flight Control 1.4.0 cannot revoke one (limitation 8).
- **Offline longer than the certificate** (a year by default): the device enrolls again as a new device, with its apps, config and firmware (limitation 13).
- **Server unreachable**: without an accepted status for 10 minutes the device restarts, its apps with it, and again every 10 minutes; a write gives up after 30 s.
- **App format**: WASM OCI artifacts only (`application/vnd.wasm.content.layer.v1+wasm`), not a `.wasm` inside a container image.
- **JSON depth**: at most 32 levels; real documents use fewer than 10.
- **Not yet on a board**: the `secure` variant, registry tokens, GPIO interrupts and wired peripherals.
- **Zephyr**: an app's heap floor and the `.wasm` size limit are not checked yet.
- **MQTT with Zephyr** depends on internal RAM and available POSIX mutexes. On the N8R8 with both TLS records in internal RAM, `mqttdemo` left 32,364 B free and a 25,580 B largest block, below the 61,440 B telemetry threshold. With the records in PSRAM the board could export at cold boot, but a broker connection exhausted the pool of 28 mutexes during TLS client-certificate signing. With 32 mutexes, the gateway received telemetry both when MQTT connected before enabling exports and when it connected after a successful export on a fresh boot (64,648 and 64,644 B free). The four additional mutexes cost 84 B in the ELF's mutex tables. [Apps on the boards](#apps-on-the-boards).
- **Zephyr on the ESP32-S3 without PSRAM**: with one app it got down to 368 bytes of free C heap while trying to download an update, which did not go through, and the device then took no spec while its statuses went out ([Zephyr on the ESP32-S3](#zephyr-on-the-esp32-s3)).
- **Agent watchdog.** On ESP-IDF and Zephyr, the watchdog checks for 30 minutes without an accepted status or a received spec. On a physical Zephyr ESP32-S3 N8R8 with PSRAM disabled, the device restarted 1,826 s after its last spec while statuses continued. The last accepted status was 54 s before the restart. The same device came back online on the confirmed firmware and applied its pending spec. The ESP-IDF watchdog and the missing-status case have unit-test coverage.
- **Memory for apps.** A 64 KiB app memory requires a contiguous block plus enough free internal RAM for the agent. Classic ESP32 and C5 each completed a ten-swap trial. The C5 recovered one refusal on the same spec. With RTC FAST memory available for small TLS allocations, a repeat completed all ten swaps without a TLS write failure. [CoreMark](#coremark).

What comes next is in the [roadmap](ROADMAP.md).
