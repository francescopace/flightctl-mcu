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

Buses, analog, metrics and MQTT are not in the firmware yet: they return `FC_ENOTSUP` once the name exists and is granted, so an app written for them loads and can tell. What each port has is under [Ports](#ports).

## How an app runs

The app exports `fc_abi_version` (with `FC_APP_ABI()`), and then any of these:

- `init()`: called once after loading. It opens what the app needs. A non-zero return is an error.
- `tick()`: called every `TICK_MS` milliseconds (an env var of the app, from 10 to 60000, 100 by default). `fc_time_ms` gives the time.
- `on_event(type, handle, value)`: called for each GPIO edge, timer, UART data, MQTT message, new config, or change in the broker connection.

`fc_app.h` declares these prototypes, so a definition with another signature does not compile. The device refuses, when it downloads it, an app that exports neither `tick` nor `on_event`, that imports a function the firmware does not have, or that imports from the module `env`: that one was built with the interface of earlier firmware and must be rebuilt.

All of them run on the app's thread, one at a time, so the app needs no locks. Each call may run a budget of instructions and wait at most a second in the device's functions (`FC_CALL_LIMIT_MS`). A trap, or a call past either limit, stops the app; the device restarts it after a few seconds and counts it in `status.applications[].restarts`. An app in an endless loop is stopped that way too, so it can always be updated or removed.

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

The fleet's template can vary it per device, from labels, as with any config file. A name is unique in the whole map, whatever its kind: `FC_ALLOW` lists names without saying what they are. `active_low` makes levels logical (1 is "on"); `safe` is the level an output takes when its app stops; a plain LED has `active_low` too, a WS2812 its byte `order`. For a name the firmware already has, `"pin": -1` keeps the firmware's pin: a template can set `led0`'s colour order on every board without knowing where its LED is.

The device checks a new `board.json` before it keeps it, and refuses it, as an update error in the device's status, when it names a pin the firmware uses (flash, PSRAM, the console's UART and USB pins) or one the chip does not have, puts two names on one pin, or has an unknown section or field type. A new map restarts the apps that use the interface.

The same app then runs on every board whose map has the names it uses. A name it asks for and the map lacks gives `FC_ENOENT`.

## What an app may use

The fleet grants each app what it may open, in its env var `FC_ALLOW`: names from the board map, and `mqtt` and `metrics` for the network. Without `FC_ALLOW` an app has state, log, info, time, config, storage and the chip's sensors, and no GPIO, bus or network. Two apps may be granted the same name, but only one holds it at a time: the other gets `FC_EBUSY`. So two apps cannot drive the same relay, and an app given to a fleet by mistake reaches nothing it was not given. A whole I2C bus is one name, so the sensors on it belong to one app.

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

| | ESP-IDF | Zephyr | Host simulator |
|---|---|---|---|
| GPIO with interrupts | yes | not yet | simulated; nothing presses `button0` |
| `led0` | the board's LED (`CONFIG_FC_LED_GPIO`, WS2812 or plain) unless `board.json` moves it | simulated, printed on the console | simulated, printed |
| `die_temp` | the chip's sensor; `FC_ENOTSUP` on the ESP32, which has none | simulated: 30 to 46 °C over two minutes | simulated, the same |

## Size and limits

| | |
|---|---|
| Apps per device | 2 |
| Module | 64 KB |
| Linear memory | one 64 KB page, shrunk to what the module uses (see below) |
| Open handles per app (GPIOs, LEDs, timers...) | 12 |
| Timers per app | 4 |
| Storage per app | 4 KB |
| Queued events | 16 per app; past that the oldest go, and the app hears how many with `FC_EV_OVERFLOW` |
| One call of `init`, `tick` or `on_event` | 20 million instructions, and 1 s of waiting |
| One wait (`fc_sleep_ms`, `fc_uart_read`) | 100 ms |

The instruction budget is a first figure, to be measured on a C3, the slowest chip, so that it comes to about a second there.

What limits an app on a small chip is the heap, not the 64 KB the device accepts for a `.wasm`. Each app takes the module as WAMR's fast interpreter loads it (about 2.5 to 3.5 times the `.wasm`), its linear memory (static data and the 1 KB stack), about 3.5 KB for the instance and 8 KB for its thread ([RESULTS.md](RESULTS.md#ram)).

There is no libc: `fc_app.h` has a few helpers (`fc_fmt_int`, `fc_config_int`, `fc_config_double`, `fc_config_milli`, `FC_STR`). They are compiled into the app, so they can change without touching the interface. Build and push with `wasm-apps/build.sh <app> <version>`, and keep its link flags. Exporting `__heap_base` and `__data_end` lets WAMR shrink the WebAssembly memory page to what the module actually uses, without which an app does not fit on a C3. It also builds for the WebAssembly MVP (`-mcpu=mvp`): the firmware's WAMR has no reference types, and clang 20 and later otherwise encode indirect calls in a way it refuses.

## Examples

Neither has a `tick`: timers wake them, and they sleep in between.

- [alarm](../wasm-apps/alarm/alarm.c): the line temperature alarm. It reads `die_temp`, takes `threshold_c`, `hysteresis_c` and `sample_ms` from `/etc/fc-app/alarm.json`, and applies a new file without restarting. It drives `led0` when the fleet grants it (`FC_ALLOW: "led0"`); without the grant it reports only its state. The same source builds two images, to show an app updated without a reboot: `alarm:v1` lights the LED solid blue above the threshold, and `alarm:v2` adds a pre-alarm (dim blue, 2 °C below) and blinks with a second timer. Blue only, so boards with a single-colour LED look the same as RGB ones. About 2 KB of `.wasm`.
- [trend](../wasm-apps/trend/trend.c): no grants needed. It reports whether the temperature is rising or falling, and how fast. With `metrics` granted it will also send the temperature and the rate as metrics. About 2 KB.

On a chip without a temperature sensor both set their state to say so and stay idle, with no timer running.
