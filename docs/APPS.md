# Writing a WASM app

An app is a WebAssembly module that Flight Control deploys as a container application, with a WASM OCI artifact as its image. The device runs it with WAMR, one thread per app, and replaces it without a reboot. The app reaches the device only through the functions in [wasm-apps/sdk/fc_app.h](../wasm-apps/sdk/fc_app.h) (import module `fc`), version 1 of the interface. Until the v1.0.0 tag it can still change; after that, functions are only added.

| Group | Functions | Granted by |
|---|---|---|
| State, log and info | `fc_set_state`, `fc_log`, `fc_info` | always |
| Time | `fc_time_ms`, `fc_time_unix_ms`, `fc_timer_start`, `fc_sleep_ms`, `fc_random` | always |
| Config | `fc_config_get` | always |
| Storage | `fc_kv_get`, `fc_kv_set`, `fc_kv_del` | always |
| GPIO and LEDs | `fc_gpio_open/read/write/irq`, `fc_led_open/rgb` | a name in `FC_ALLOW` |
| Buses | `fc_i2c_*`, `fc_spi_*`, `fc_uart_*` | a name in `FC_ALLOW` |
| Analog | `fc_adc_*`, `fc_pwm_*` | a name in `FC_ALLOW` |
| Chip sensors | `fc_sensor_read` (`die_temp`) | always |
| Metrics | `fc_metric` | `metrics` |
| MQTT | `fc_mqtt_publish/subscribe/topic`, `fc_event_data` | `mqtt` |

Metrics and MQTT are not in the firmware yet: they return `FC_ENOTSUP` once the name exists and is granted, so an app written for them loads and can tell. What each port has is under [Ports](#ports).

## How an app runs

The app exports `fc_abi_version` (with `FC_APP_ABI()`), and then any of these:

- `init()`: called once after loading. It opens what the app needs. A non-zero return is an error.
- `tick()`: called every `TICK_MS` milliseconds (an env var of the app, from 10 to 60000, 100 by default). `fc_time_ms` gives the time.
- `on_event(type, handle, value)`: called for each GPIO edge, timer, UART data, MQTT message, new config, or change in the broker connection.

`fc_app.h` declares these prototypes, so a definition with another signature does not compile. The device refuses, when it downloads it, an app that exports neither `tick` nor `on_event` (nor runs once, below), or that imports a function the firmware does not have. One that imports `env.set_state`, `env.log` or `env.led` was built with the interface of earlier firmware and must be rebuilt.

All of them run on the app's thread, one at a time, so the app needs no locks. Each call may run a budget of instructions and wait at most a second in the device's functions (`FC_CALL_LIMIT_MS`). A trap, or a call past either limit, stops the app; the device restarts it after a few seconds and counts it in `status.applications[].restarts`. An app in an endless loop is stopped that way too, so it can always be updated or removed; the exception is a loop in code the module runs as it is instantiated ([ROADMAP.md](ROADMAP.md#next)).

A trap is the app's; a crash of the firmware is the device's, and it restarts. The device starts its saved apps at boot, before it reaches the server, so an app whose start crashes the firmware (a fault in the runtime as it loads the module, or in a driver the app opens) would restart it for good. The device counts such restarts: one that comes while an app's module is loading is that app's, otherwise it is every app's still in its first minute. An app counted twice in a row is not started again. Its status is `Error`, "crashed the device as it started", the spec is not retried, and the device goes on to the server, where the fleet can change it. It is tried again when its image or its env vars change, when the firmware changes, or after it leaves the spec. A restart that was not a crash (power, an update) resets the count; a crash is a fault, an abort or a watchdog on ESP-IDF, a fatal error on Zephyr.

## Modules built for no device

A module built for no device in particular, such as a benchmark, runs as it was published. The fleet names the export to call in the app's env var `FC_ENTRY`; a module with neither `tick` nor `on_event` that exports `_start` needs none. The device calls that export once, with no arguments:

- once the agent has had nothing on hand for 2 s (no status update due or being sent, no spec being applied), and at most 30 s after the app started; until then the app is `Preparing`. Its memory is taken first, as soon as the app starts, before the agent opens its connection again after the download; if it does not fit while the agent still holds the spec, it is tried once more when the agent is idle. Right after a change the agent applies the spec and sends two status updates, each a TLS exchange: on a chip with one core they would take CPU from the first seconds of the run, where a benchmark such as CoreMark calibrates itself. At boot it also waits for the agent to connect;
- with no instruction budget: it may run for as long as it needs. Removing or updating the app stops it within a few instructions, at its next branch or call;
- on the app's thread, at the priority of the agent's threads, so they take turns on the CPU;
- when it returns, the app is `Completed` and not run again until its image or env vars change, or the device restarts. What it returned is its state, in `customInfo["app.<name>"]`, such as `run() returned 12.500`; a float is given to the thousandth. A trap is an `Error`, and it is run again after a few seconds, as any app.

Such a module may import `env.clock_ms`, as CoreMark does: the milliseconds since the app started, as an i32 or an i64. That is the only function the device has in `env`, the module that clang's `wasm-ld --allow-undefined` and Emscripten import from by default; any other is refused when downloaded. It may also use tail calls (`return_call`). It may not yet import from `fc`: its call has none of an app's handles, events or limits on waits around it.

[CoreMark](../wasm-apps/coremark/fetch.sh) runs that way, from either published build: wasm3's, 7.7 KB with 64 KB of memory, or wasm-coremark's, 12.5 KB with 128 KB. Neither exports `__heap_base`, so the device holds all their memory, in one block (see below): 64 KB takes 68 KB of ESP-IDF's heap, which rounds a block up to its size class. `run()` runs for at least 10 seconds and returns the score, in iterations per second (0 when the run was not valid); the scores measured are in [RESULTS.md](RESULTS.md#coremark):

```yaml
        - name: coremark
          appType: container
          image: <registry>/flightctl-mcu/coremark:wasm3
          envVars:
            FC_ENTRY: run
```

An app that polls a sensor every second needs only `tick`. An app that waits for a button or a command sleeps between events and costs no CPU. Other periods are timers (`fc_timer_start`), stopped with `fc_close`.

When the app stops, for whatever reason (a trap, an update, its removal), the device closes what it opened and leaves the hardware safe: a GPIO output goes to the `safe` level the board map gives it, or back to an input without one, and an LED goes off. A pump stays off while its app is replaced.

## Hardware by name

The app opens `"relay1"` or `"button0"`, never a pin number. The names come from the **board map**: the firmware has its own (on ESP-IDF, `led0`, the board's LED), and the fleet adds what is wired to the board with the config file `/etc/fc/board.json`, entry by entry:

```json
{
  "gpio": { "button0": { "pin": 0, "active_low": true }, "relay1": { "pin": 5, "safe": 0 } },
  "led":  { "led0": { "pin": 48, "ws2812": true, "order": "grb" } },
  "i2c":  { "i2c0": { "sda": 8, "scl": 9, "hz": 400000 } },
  "adc":  { "level": { "pin": 1 } },
  "pwm":  { "fan": { "pin": 6 } }
}
```

The fleet's template can vary it per device, from labels, as with any config file. A name is unique in the whole map, whatever its kind: `FC_ALLOW` lists names without saying what they are. `active_low` makes levels logical (1 is "on"); `safe` is the level an output takes when its app stops; a plain LED has `active_low` too, a WS2812 its byte `order`. An I2C bus has `sda`, `scl` and its clock in `hz` (100 kHz without it). An SPI device has its bus's `sck`, `mosi` and `miso` (one of the last two may be left out), its chip select `cs`, its `mode` (0 to 3) and its clock in `hz` (1 MHz without it); devices with the same three bus pins share the bus. A UART has `tx`, `rx` (one may be left out), `de`, the pin that enables an RS-485 transceiver while it sends, and the `baud` used when the app asks for 0 (115200 without it). A PWM may have its frequency in `hz`, used when the app asks for 0 (1 kHz without it). For a name the firmware already has, `"pin": -1` (or no pins, for a bus) keeps the firmware's pins: a template can set `led0`'s colour order on every board without knowing where its LED is.

The device checks a new `board.json` before it keeps it, and refuses it, as an update error in the device's status, when it names a pin the firmware uses (flash, PSRAM, the console's UART and USB pins) or one the chip does not have, puts two names on one pin (a bus's pins included), or has an unknown section or field type. A new map restarts the apps that use the interface.

The same app then runs on every board whose map has the names it uses. A name it asks for and the map lacks gives `FC_ENOENT`.

## What an app may use

The fleet grants each app what it may open, in its env var `FC_ALLOW`: names from the board map, and `mqtt` and `metrics` for the network. Without `FC_ALLOW` an app has state, log, info, time, config, storage and the chip's sensors, and no GPIO, bus or network. Two apps may be granted the same name, but only one holds it at a time: the other gets `FC_EBUSY`. So two apps cannot drive the same relay, and an app given to a fleet by mistake reaches nothing it was not given. A whole I2C bus is one name, so the sensors on it belong to one app; an SPI bus is shared, each device a name, so two apps may each hold a device on it, taking turns (a device that keeps its chip select between transfers holds the bus until the end of the app's call).

The device keeps an app's env vars with it, so an app started at boot, before the device reaches the server, has its grants. A change to them restarts the app.

```yaml
spec:
  template:
    spec:
      config:
        - name: board
          inline:
            - path: /etc/fc/board.json
              content: '{"gpio": {"relay1": {"pin": 5, "safe": 0}, "button0": {"pin": 0, "active_low": true}}}'
        - name: pump
          inline:
            - path: /etc/fc-app/pump.json
              content: '{"period_ms": 5000, "max_level_mv": 2400}'
      applications:
        - name: pump
          appType: container
          image: registry.example.com/flightctl-mcu/pump:v1
          envVars:
            FC_ALLOW: "relay1 button0 led0"
```

## Config and storage

The app's `envVars` are for the device (`FC_ALLOW`, `TICK_MS`). The app's own settings are in `/etc/fc-app/<app name>.json`, a flat JSON object that `fc_config_get` reads key by key, as text. The header turns a value into a number: `fc_config_int` an integer, `fc_config_double` any number, `fc_config_milli` a number into rounded thousandths (`"40.5"` into 40500, to compare with milli-degrees or millivolts). A new file raises `FC_EV_CONFIG`, so the app can apply it without restarting; a file that comes again unchanged with a new version of the device's spec does not.

`fc_kv_*` keeps up to 4 KB per app, keys and values together, across reboots and new versions of the app: a calibration, a counter, the last state. Keys are up to 31 characters. An app's keys are one record in flash, rewritten whole at each `fc_kv_set`, so they are for what changes seldom. They are removed with the app.

`fc_info` gives the device's name in Flight Control (`FC_EAGAIN` until it has enrolled), the app's, the firmware's version and the board, for payloads and log lines.

## Where the data goes

Today an app reports its **state** (`fc_set_state`): a short text in Flight Control, `customInfo["app.<name>"]`, such as `ok`, `alarm` or `calibrating`. Flight Control records each change as an event, so it is for what changes seldom.

Two more ways are part of the interface, for when the firmware has them ([ROADMAP.md](ROADMAP.md)):

- **Metrics** (`fc_metric`): numbers for Flight Control's Telemetry Gateway, which adds the device's ID and exports them to Prometheus, sent in one batch per interval over TLS with the device's management certificate.
- **MQTT** (`fc_mqtt_*`): the application's own data and commands, with a broker the fleet gives in `/etc/fc/mqtt.json` (address, user, topic prefixes) and its CA. The device keeps one connection for all the apps. Each app publishes and subscribes under its own prefix, by default `flightctl/<device>/<app>/`, and can subscribe under the fleet's, to receive a command sent once to every device. QoS is 0 or 1; topics are at most 64 characters and payloads 512 bytes.

## Ports

On Zephyr the board map's names come from the devicetree, not from pins: the children of a node with compatible `flightctl,app-hw` ([ZEPHYR.md](ZEPHYR.md#hardware-for-apps)). The fleet's `board.json` may give those nodes other names (`"dt": "<node>"`), never pins. A board without that node has only the simulated `led0` and `die_temp`.

| | ESP-IDF | Zephyr | Host simulator |
|---|---|---|---|
| GPIO with interrupts | yes | yes, the devicetree's names | simulated; nothing presses `button0` |
| `led0` | the board's LED (`CONFIG_FC_LED_GPIO`, WS2812 or plain) unless `board.json` moves it | simulated, printed on the console | simulated, printed |
| `die_temp` | the chip's sensor; `FC_ENOTSUP` on the ESP32, which has none | simulated: 30 to 46 °C over two minutes | simulated, the same |
| I2C | master, any two pins; up to two buses on the ESP32 and S3, one on the C chips | the devicetree's controllers; a transfer lasts as long as the driver makes it | `i2c0`, with one device at 0x48: register 0 the simulated temperature in hundredths of a degree, 2 and 3 writable |
| ADC | ADC1's pins, 0 to about 3.1 V, calibrated from the chip's eFuse; up to 8 channels | the devicetree's channels, in millivolts by their reference | 1650 mV on any pin |
| PWM | LEDC, up to 4, each at its own frequency, duty to 14 bits | the devicetree's channels | printed when it changes |
| SPI | master, up to 4 devices on one bus (two buses on the ESP32 and S3), chip select driven by the firmware; transfers in pieces of 64 bytes, without DMA | the devicetree's devices, chip select driven by the firmware; a transfer lasts as long as the driver makes it | MISO wired to MOSI: what is sent comes back |
| UART | UART1, and UART2 on the ESP32 and S3 (UART0 is the console's); RS-485 half duplex with `de`; 512 bytes received ahead | interrupt-driven, 512 bytes received ahead; never the console's; no RS-485 yet (`FC_ENOTSUP` for a node with `de-gpios`) | RX wired to TX: what is written comes back, with `FC_EV_UART` |

## Size and limits

| | |
|---|---|
| Apps per device | 2 |
| Module | 64 KB |
| Linear memory | one 64 KB page, shrunk to what the module uses (see below); a module that does not export `__heap_base` and `__data_end` keeps all it declares |
| Open handles per app (GPIOs, LEDs, timers...) | 12 |
| Timers per app | 4 |
| Storage per app | 4 KB |
| Queued events | 16 per app; past that the oldest go, and the app hears how many with `FC_EV_OVERFLOW` |
| One call of `init`, `tick` or `on_event` | 20 million instructions, and 1 s of waiting |
| The export in `FC_ENTRY`, or `_start` | no limit, stopped when the app is removed or updated |
| One wait (`fc_sleep_ms`, `fc_uart_read`), one I2C transfer, one UART write, or waiting for another app's SPI device | 100 ms |
| One I2C transfer | 1 KB each way (`FC_I2C_MAX`) |
| One SPI transfer | 4 KB (`FC_SPI_MAX`) |

The instruction budget is a first figure, to be measured on a C3, the slowest chip, so that it comes to about a second there.

What limits an app on a small chip is the heap, not the 64 KB the device accepts for a `.wasm`. Each app takes the module as WAMR's fast interpreter loads it (about 2.5 to 3.5 times the `.wasm`), its linear memory (static data and the 1 KB stack), about 3.5 KB for the instance and 8 KB for its thread ([RESULTS.md](RESULTS.md#ram)).

There is no libc: `fc_app.h` has a few helpers (`fc_fmt_int`, `fc_config_int`, `fc_config_double`, `fc_config_milli`, `FC_STR`). They are compiled into the app, so they can change without touching the interface. Build and push with `wasm-apps/build.sh <app> <version>`, and keep its link flags. Exporting `__heap_base` and `__data_end` lets WAMR shrink the WebAssembly memory page to what the module actually uses, without which an app does not fit on a C3. It also builds for the WebAssembly MVP (`-mcpu=mvp`): the firmware's WAMR has no reference types, and clang 20 and later otherwise encode indirect calls in a way it refuses. Tail calls are the one feature beyond the MVP it has.

## Examples

Neither has a `tick`: timers wake them, and they sleep in between.

- [alarm](../wasm-apps/alarm/alarm.c): the line temperature alarm. It reads `die_temp`, takes `threshold_c`, `hysteresis_c` and `sample_ms` from `/etc/fc-app/alarm.json`, and applies a new file without restarting. It drives `led0` when the fleet grants it (`FC_ALLOW: "led0"`); without the grant it reports only its state. The same source builds two images, to show an app updated without a reboot: `alarm:v1` lights the LED solid blue above the threshold, and `alarm:v2` adds a pre-alarm (dim blue, 2 °C below) and blinks with a second timer. Blue only, so boards with a single-colour LED look the same as RGB ones. About 2 KB of `.wasm`.
- [trend](../wasm-apps/trend/trend.c): no grants needed. It reports whether the temperature is rising or falling, and how fast. With `metrics` granted it will also send the temperature and the rate as metrics. About 2 KB.

On a chip without a temperature sensor both set their state to say so and stay idle, with no timer running.

- [CoreMark](../wasm-apps/coremark/fetch.sh), as published, not rebuilt: `wasm-apps/coremark/fetch.sh` downloads both builds and checks them against their SHA-256, then `wasm-apps/push.sh coremark wasm3` pushes one. See [Modules built for no device](#modules-built-for-no-device).
