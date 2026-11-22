# WebAssembly runtime

The three MCU ports run signed WASM modules with the WAMR 2.4.6 fast interpreter, instruction metering and tail calls. Apps target the WASM MVP instruction set. AOT, JIT, WASI, SIMD and reference types are disabled. [App ABI and build flags](APPS.md#size-and-limits) · [Board benchmarks](BENCHMARKS.md)

## Runtime configuration

Flight Control MCU v0.9.7 applies 12 patches to WAMR 2.4.6. Each patch acts on the port or interpreter configuration shown below.

| WAMR patch | Scope and effect |
|---|---|
| [espidf-stack-info](../third_party/patches/wamr-espidf-stack-info.patch) | Reads the FreeRTOS task stack base without scanning for the high-water mark. Callback time fell 21.1–43.4% across five ESP-IDF boards with identical linked IRAM and static data sizes. CoreMark showed no repeatable gain. [Callback times by board](BENCHMARKS.md#timer-callbacks) |
| [espidf-psram-interp](../third_party/patches/wamr-espidf-psram-interp.patch) | Restricts the ESP32-S3 PSRAM mapping option to AOT builds. An interpreter-only image built with PSRAM support can start apps on a board without PSRAM. |
| [wasm32-compact-dispatch](../third_party/patches/wamr-wasm32-compact-dispatch.patch) | On selected 32-bit targets, the fast interpreter specializes common instructions and dispatches frequent pairs directly. Using 16-bit handler offsets saves two bytes per handler reference in decoded code. [Dispatch comparisons](#interpreter-dispatch) |
| [const-natives](../third_party/patches/wamr-const-natives.patch) | Leaves sorted native symbol tables in flash and lets the agent register the libc functions available to apps. |
| [libc-subset](../third_party/patches/wamr-libc-subset.patch) | Keeps the 15 builtin libc wrappers used by the agent so the linker can discard unused wrappers. |
| [espidf-realloc](../third_party/patches/wamr-espidf-realloc.patch) | Preserves the contents of an ESP-IDF allocation when moving it changes the padding required for alignment. |
| [thumb-stack-align](../third_party/patches/wamr-thumb-stack-align.patch) | Keeps the stack aligned to 8 bytes on Cortex-M without an FPU when a call to a native function passes arguments on the stack. |
| [unaligned-select](../third_party/patches/wamr-unaligned-select.patch) | Uses alignment-safe writes when the loader or mini-loader rewrites a 64-bit select. This lets affected modules load on Cortex-M0+. |
| [instantiation-hook](../third_party/patches/wamr-instantiation-hook.patch) | Gives the agent access to the interpreter execution environment during instantiation so the app's start function and constructors run with its instruction budget and native-call context. |
| [metering-stop](../third_party/patches/wamr-metering-stop.patch) | Checks for a stop request at branches and calls so the agent can interrupt a running app with instruction metering enabled and the WAMR thread manager disabled. |
| [unbounded-checkpoint](../third_party/patches/wamr-unbounded-checkpoint.patch) | Gives unlimited interpreter calls periodic checkpoints where the ESP-IDF scheduler can yield CPU time to other runners. |
| [bytecode-placement](../third_party/patches/wamr-bytecode-placement.patch) | Lets the ESP32-C5/C6 firmware resize its contiguous heap reservation after reading the module's memory declaration and before allocating decoded functions in main SRAM. |

The ESP-IDF port prefers internal RAM for linear memory, then PSRAM. Eligible SDK modules export `__heap_base` and `__data_end` so WAMR can shrink the allocation. [WAMR version and patch selection](../third_party/fetch-wamr.sh)

## Comparison with Toit

One ESP32-D0WD rev. 1.0 at 240 MHz ran Flight Control MCU v0.9.4 (C compiled to WASM), Toit 2.0.0 and MicroPython 1.29.0. Medians cover three runs per runtime in the initial comparison. Kernel rates are higher-is-faster, times are lower-is-faster. The WAMR DeltaBlue figure is the free-list median from the [allocator comparison](#sdk-allocator). The initial comparison measured 3,417.3 ms/iteration with the first-fit allocator.

| Runtime | 2,000-byte kernels (iterations/s) | Richards (ms/iteration) | DeltaBlue (ms/iteration) |
|---|---:|---:|---:|
| WAMR | 18.950 | 969.2 | 900.3 |
| Toit `-O2` | 5.653 | 1,298.97 | 859.91 |
| MicroPython | — | 9,990.9 | 13,351.6 |

The catalog wasm3 module executed 3.35× as many iterations/s as the local Toit translation of the CoreMark kernels. Each Toit batch ran 320 iterations and returned the reference CRC. This is an experimental translation. Official CoreMark scores require the original kernels. [Toit source](../wasm-apps/runtime-bench/kernels.toit) · [Method and validation](TESTING.md#comparing-the-2000-byte-workload-with-toit)

## SDK allocator

Two allocators appear on this page. **First-fit** is the `malloc` of SDK v0.9.4. **Free list** is the `malloc` from SDK v0.9.5-rc.1, which visits free blocks in an address-ordered list and coalesces adjacent blocks on release.

DeltaBlue spent about 78.7% of its time in the first-fit `malloc`. On the same ESP32, two WASM installations per allocator used the same 40 KiB arena and inputs.

| DeltaBlue runtime and allocator | Median (ms/iteration) | Fastest batch (ms/iteration) | Slowest batch (ms/iteration) |
|---|---:|---:|---:|
| WASM, first-fit | 3,420.6 | 3,415.7 | 3,425.3 |
| WASM, free list | 900.3 | 898.7 | 902.3 |
| Toit 2.0.0 `-O2` | 859.91 | 858.85 | 862.44 |

With the free list, WASM took 4.7% longer per iteration than the earlier Toit measurement. The module grew from 11,923 to 12,758 B. Across six boards the [paired run](BENCHMARKS.md#sdk-release-comparison) sped up DeltaBlue by 3.35–4.08×. Richards changed by at most 0.49%. · [Allocation profile method](TESTING.md#deltablue-allocation-profile) · [Allocator](../wasm-apps/runtime-bench/freelist.c)

<details>
<summary>DeltaBlue allocation profile with the first-fit allocator</summary>

First-fit allocator on the classic ESP32. The phase timer and candidate-block counter used separate builds.

| Phase | Elapsed (ms/iteration) | Calls to `malloc`/iteration | Candidate blocks visited/iteration |
|---|---:|---:|---:|
| Build graph and add edit constraints | 2,583.3 | 11,420 | 2,079,590 |
| Extract plans | 66.1 | 100 | 37,450 |
| Execute plans and check the chain | 257.0 | 0 | 0 |
| Reset, remove constraints, clean up and check projections | 511.7 | 1,280 | 316,900 |

</details>

### Fragmentation on classic ESP32

Both allocators recovered all 40,948 usable bytes after teardown. Time per free is a batch estimate. The largest single reading includes scheduling and a 1 ms clock. [Workload and timing](TESTING.md#fragmentation-and-time-spent-in-free) · [Batch timings](benchmarks/esp32-fragmentation-v0.9.5-rc.1.json)

| Free blocks before target | Neighbors merged by the free list | First-fit, time per free (µs) | Free list, time per free (µs) | First-fit, largest single reading (ms) | Free list, largest single reading (ms) |
|---:|---|---:|---:|---:|---:|
| 0 | None | 0.89 | 2.32 | 1 | 1 |
| 128 | None | 0.90 | 129.88 | 1 | 1 |
| 1,024 | None | 0.92 | 1,011.72 | 1 | 2 |
| 2,559 | Left | 0.95 | 2,492.19 | 1 | 33 |
| 2,558 | Both | 0.95 | 2,531.25 | 1 | 32 |

## Interpreter dispatch

Classic ESP32 at 240 MHz, ESP-IDF 6.1.1. Each build ran the same signed modules across three installations on two boots. Experiment 2 is the full patch with 16-bit and with 32-bit offsets. Experiment 1 is a separate experiment that kept standard 32-bit addresses and added direct jumps for 35 instruction pairs. DeltaBlue here measures only the allocation-free plan execution phase. [Interpreter patch](../third_party/patches/wamr-wasm32-compact-dispatch.patch)

| Experiment | Build | CoreMark, 64 KiB (iterations/s) | Richards (ms/iteration) | DeltaBlue plan execution (ms/iteration) | Image (B) |
|---:|---|---:|---:|---:|---:|
| 1 | Standard dispatch | 14.786 | 1,283.0 | 330.9 | — |
| 1 | Pairs: standard 32-bit addresses with direct jumps for 35 instruction pairs | 15.752 | 1,222.3 | 322.0 | — |
| 2 | Full patch, 32-bit offsets | 17.004 | 1,024.1 | 270.3 | 1,193,488 |
| 2 | Full patch, 16-bit offsets (the [released configuration](#runtime-configuration)) | 18.991 | 983.1 | 262.4 | 1,193,040 |

Values are medians. In experiment 1 the pairs grew the firmware by 3,728 B. [Experiment 1 runs and image sizes](benchmarks/esp32-pairs32.json)

<details>
<summary>Range over the three installations</summary>

| Build | Workload | Lowest | Highest |
|---|---|---:|---:|
| Standard dispatch | CoreMark (iterations/s) | 14.780 | 14.877 |
| Standard dispatch | Richards (ms/iteration) | 1,274.0 | 1,283.7 |
| Standard dispatch | DeltaBlue plan execution (ms/iteration) | 321.3 | 336.7 |
| Pairs | CoreMark (iterations/s) | 15.741 | 15.813 |
| Pairs | Richards (ms/iteration) | 1,212.9 | 1,224.2 |
| Pairs | DeltaBlue plan execution (ms/iteration) | 321.0 | 325.2 |
| Full patch, 32-bit offsets | CoreMark (iterations/s) | 16.989 | 17.015 |
| Full patch, 32-bit offsets | Richards (ms/iteration) | 1,023.3 | 1,024.5 |
| Full patch, 32-bit offsets | DeltaBlue plan execution (ms/iteration) | 268.8 | 272.3 |
| Full patch, 16-bit offsets | CoreMark (iterations/s) | 18.918 | 18.993 |
| Full patch, 16-bit offsets | Richards (ms/iteration) | 981.8 | 984.6 |
| Full patch, 16-bit offsets | DeltaBlue plan execution (ms/iteration) | 260.9 | 264.2 |

</details>
