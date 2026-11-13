# Writing a WASM app

Apps include [wasm-apps/sdk/fc_app.h](../wasm-apps/sdk/fc_app.h), which is the interface between an app and the device, with a version number. An app exports `tick(now_ms)`, which the device calls every `TICK_MS` milliseconds. That value comes from the app's `envVars` in the fleet and defaults to 100. The app reaches the hardware only through host functions:

- from the agent, on every device: `fc_set_state()`, shown in Flight Control as `customInfo["app.<name>"]`, and `fc_log()`;
- from the firmware: `fc_temp_mc()`, `fc_threshold_mc()` (from the fleet config) and `fc_led(r, g, b)`.

`FC_APP_ABI()` declares the interface version the app was built for, so an older device refuses it with a clear error instead of failing later.

There are two examples:

- [alarm](../wasm-apps/alarm/alarm.c): the LED logic of the temperature alarm, a few hundred bytes. It uses only blue, dim for the pre-alarm and blinking for the alarm, so boards with a single-colour LED look the same as RGB ones.
- [trend](../wasm-apps/trend/trend.c): no LED. It smooths the temperature and reports whether it is rising or falling, and how fast.

Build and push with `wasm-apps/build.sh <app> <version>`. Keep its link flags: exporting `__heap_base` and `__data_end` lets WAMR shrink the 64 KB WebAssembly memory page to what the module actually uses. Without that, an app does not fit on a C3. It also builds for the WebAssembly MVP (`-mcpu=mvp`): the firmware's WAMR has no reference types, and clang 20 and later otherwise encode indirect calls in a way it refuses, so an app with a function pointer would not load.

What limits an app on a small chip is the heap, not the 64 KB the device accepts for a `.wasm`. Each app takes the module as WAMR's fast interpreter loads it (about 2.5 to 3.5 times the `.wasm`), its linear memory (static data and the 1 KB stack), about 3.5 KB for the instance and 8 KB for its thread. `trend`, 1.9 KB of `.wasm`, takes 17.7 KB on a C3 ([RESULTS.md](RESULTS.md#ram)).
