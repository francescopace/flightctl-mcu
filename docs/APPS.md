# Writing a WASM app

Package the `.wasm` module as a WASM OCI artifact. The agent runs one WAMR thread per app and can replace it without rebooting. Set `appType: container` in the fleet. The image must carry a WASM layer directly. A module inside a tar layer will fail to load.

For C apps, start with [alarm.c](../wasm-apps/alarm/alarm.c). Build and publish with `wasm-apps/build.sh <app> <version>`. The [SDK header](../wasm-apps/sdk/fc_app.h) defines the signatures, events and errors. For example, `fc_gpio_irq` imports `fc.gpio_irq`. ABI version 1 can change until v1.0.0. After that, changes within the version are additive.

| Group | Functions | Granted by |
|---|---|---|
| State, log and info | `fc_set_state`, `fc_log`, `fc_info` | always |
| Time and timers | `fc_time_ms`, `fc_time_unix_ms`, `fc_timer_start`, `fc_sleep_ms`, `fc_random` | always |
| Handles | `fc_close` | an open handle owned by the app |
| Config | `fc_config_get` | always |
| Storage | `fc_kv_get`, `fc_kv_set`, `fc_kv_del` | always |
| GPIO and LEDs | `fc_gpio_open`, `fc_gpio_read`, `fc_gpio_write`, `fc_gpio_irq`, `fc_led_open`, `fc_led_rgb` | a name in `FC_ALLOW` |
| I²C | `fc_i2c_open`, `fc_i2c_read`, `fc_i2c_write`, `fc_i2c_write_read` | a name in `FC_ALLOW` |
| SPI | `fc_spi_open`, `fc_spi_transfer` | a name in `FC_ALLOW` |
| UART | `fc_uart_open`, `fc_uart_read`, `fc_uart_write` | a name in `FC_ALLOW` |
| ADC and PWM | `fc_adc_open`, `fc_adc_read_mv`, `fc_pwm_open`, `fc_pwm_duty` | a name in `FC_ALLOW` |
| Sensors | `fc_sensor_read` (temperature or battery channels), `fc_sensor_subscribe` (temperature triggers) | board-map name in `FC_ALLOW`; built-in `die_temp` needs no grant |
| Metrics | `fc_metric` | `metrics` |
| MQTT | `fc_mqtt_publish`, `fc_mqtt_subscribe`, `fc_mqtt_topic`, `fc_event_data` | `mqtt` |
| C library (`env`) | Functions from `string.h`, `stdio.h`, `stdlib.h`, `ctype.h` and `math.h`. [Function list](#the-c-library) | always |
| Clock (`env`) | `clock_ms` | always |
| Arduino (`arduino`) | `pinMode`, `digitalWrite`, `getPinLED`, `delay`, `millis`, `print`, `getGreeting`. [Arduino interface](#the-blink-of-wasm3-arduino) | GPIO and LED access use names in `FC_ALLOW` |

MQTT functions return `FC_ENOTSUP` on firmware built without MQTT. See [port differences](#ports).

![Flight Control shows a running WASM app as a Single Container application](img/app-on-device.png)

## How an app runs

Declare the ABI with `FC_APP_ABI()`. Implement `tick`, `on_event`, or both. The SDK checks the signatures at compile time.

| Export | When it runs |
|---|---|
| `init()` | Once after loading. A nonzero return fails startup. |
| `tick()` | Every `TICK_MS` milliseconds (10–60,000, default 100). |
| `on_event(type, handle, value)` | On a sensor notification, GPIO edge, timer, UART or MQTT input, config change or broker connection change. |

Callbacks run one at a time. Use a timer for periodic work and `fc_close` to stop it. An event-only app sleeps between events. Callback [limits](#size-and-limits) also apply to module initialization, including C++ constructors.

Whenever an app stops, the agent closes its handles, restores GPIO outputs to their safe level (or input) and turns off LEDs. After a trap it retries in a few seconds. See `status.applications[].restarts` for the count.

**After a device restart**, saved apps start after the first accepted status, or after three minutes without one if the agent is ready. On ESP-IDF, Wi-Fi and clock synchronization must finish first.

A device crash counts against an app if it was loading, started less than a minute ago or has had no accepted status since startup. A restart for missing status below the agent's heap reserve also counts. Two consecutive attributed crashes leave the app in `Error` with `crashed the device as it started`. Changing the image, env vars or firmware, or removing the app, clears the block. A normal power cycle or update resets the consecutive crash count.

<a id="modules-built-for-no-device"></a>

## Standalone modules

Set `FC_ENTRY` to a no-argument export to run a standalone module once. A module with `_start` and no callbacks needs no setting. It may import `env.clock_ms` (i32 or i64 milliseconds since app start), the [C library](#the-c-library) and [Arduino functions](#the-blink-of-wasm3-arduino). Imports from `fc` require the callback interface.

The module allocates memory at startup and stays in `Preparing` until the agent has been idle for 2 s, or 30 s have elapsed. Its entry runs without an instruction budget. On ESP-IDF the interpreter checks every 4,096 operations whether to yield to the idle task. Removing or updating the app stops it.

On return, the app becomes `Completed` and records its result in `customInfo["app.<name>"]`. It runs again after an image or env change or a device restart. A trap triggers the usual retry.

Both catalog CoreMark modules export `run`. The wasm3 build needs 64 KiB of contiguous linear memory and the wasm-coremark build needs 128 KiB, plus space for the agent and app thread. A valid run returns iterations per second. [Scores by board](RESULTS.md#coremark)

```yaml
        - name: coremark
          appType: container
          image: <registry>/flightctl-mcu/coremark:v0.9.0-wasm3
          envVars:
            FC_ENTRY: run
```

## Hardware by name

Open peripherals by name. On ESP-IDF, the firmware supplies defaults such as `led0`. Add the board's wiring to `/etc/fc/board.json` through the fleet template. On Zephyr, use [devicetree names](ZEPHYR.md#hardware-for-apps).

```json
{
  "gpio": { "button0": { "pin": 0, "active_low": true }, "relay1": { "pin": 5, "safe": 0 } },
  "led":  { "led0": { "pin": 48, "ws2812": true, "order": "grb" } },
  "i2c":  { "i2c0": { "sda": 8, "scl": 9, "hz": 400000 } },
  "adc":  { "level": { "pin": 1 } },
  "pwm":  { "fan": { "pin": 6 } }
}
```

Names are unique across the map. A fleet template can use labels to select pins per device.

| Entry | Fields |
|---|---|
| GPIO, LED | `active_low` makes 1 mean on. `safe` sets an output's level when the app stops. WS2812 LEDs take a byte `order`. |
| I²C | `sda`, `scl`, optional `hz` (default 100 kHz). |
| SPI | `sck`, `mosi`, `miso` (one data pin may be omitted), `cs`, `mode` (0–3), `hz` (default 1 MHz). Devices with matching bus pins share the bus. |
| UART | `tx`, `rx` (one may be omitted), optional RS-485 driver-enable pin `de`. `baud` defaults to 115200 when the app requests 0. |
| Sensor | `"source": "die_temp"` selects the built-in temperature source. On Zephyr, `"dt"` selects a temperature probe declared in the app hardware devicetree. Battery sources reference an ADC channel or I²C bus by name. [Battery channels](#battery-channels) |
| PWM | `hz` defaults to 1 kHz when the app requests 0. |

For a firmware-defined name, `"pin": -1` (or omitted bus pins) keeps its original wiring. The device rejects invalid field types, unknown sections, reserved or nonexistent pins, and conflicting pin assignments. A valid map change restarts apps using the interface. Opening an unknown name returns `FC_ENOENT`.

## What an app may use

Set `FC_ALLOW` to space-separated hardware names and any `mqtt` or `metrics` grants. With no grants, the app can still use state, logging, info, clocks, config, storage and chip sensors.

Multiple apps can read the same sensor. Each needs its own grant for that name. Only one app can hold an open peripheral at a time. A second opener receives `FC_EBUSY`. An I²C grant covers the whole bus. SPI grants cover individual devices and the bus serializes their transfers. Holding chip select holds the bus until the callback returns.

For an alarm deployed across boards, set `"sensor": "line_temp"` in `/etc/fc-app/alarm.json` and grant `FC_ALLOW: "line_temp led0"`. The board map selects the probe:

| Board | `/etc/fc/board.json` |
|---|---|
| ESP-IDF with a die temperature sensor | `{"sensor":{"line_temp":{"source":"die_temp"}}}` |
| Zephyr ESP32-S3 DevKitC | `{"sensor":{"line_temp":{"dt":"core-temperature"}}}` |

A Zephyr board can expose an ambient probe by adding a child with `temperature-sensor = <&probe>;` to its `flightctl,app-hw` node and enabling the sensor driver. Set `die-temperature;` for a probe providing the die temperature channel. The alarm samples the selected probe every `sample_ms`. If the name exists but the app lacks its grant, Flight Control reports `sensor access denied`.

Use `fc_sensor_subscribe` to receive hardware notifications on a granted board-map name. The call returns a handle. `on_event(FC_EV_SENSOR, handle, mode)` tells the app to read the latest sample with `fc_sensor_read`. Pending events coalesce per subscription. One subscription owns the physical probe's trigger settings, and other granted apps can still read it. `fc_close(handle)`, app stop and grant revocation cancel the subscription and discard its queued events.

| Mode | Arguments and support |
|---|---|
| `FC_SENSOR_READY` | A new sample is available. Pass zero for both bounds. Requires a driver with a data-ready trigger. |
| `FC_SENSOR_THRESHOLD` | Supply a lower and upper bound in millidegrees Celsius. The driver signals its threshold condition. The ESP-IDF C6 die probe accepts whole degrees from −10 to 80 °C. Zephyr probes need threshold attributes and a trigger-capable driver. |

The physical Zephyr S3 die probe returns `FC_ENOTSUP` for both modes. The C6 die probe returns it for data-ready. The example alarm uses hardware notifications when available and keeps its sampling timer for pre-alarm and recovery. Install firmware providing `sensor_subscribe` before deploying a module that imports it. For rollback, restore the older module first to keep the app running through the firmware change.

Grants persist across reboots. Changing an app's environment restarts it.

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

## Battery channels

Use `fc_sensor_read(name, nlen, &value)` to poll a battery channel. Grant the app only the sensor names it reads. The firmware claims the underlying ADC channel or I²C bus for each reading and releases it before returning. A raw peripheral grant is unnecessary. An open handle on the same input gives `FC_EBUSY`. Battery channels return `FC_ENOTSUP` from `fc_sensor_subscribe`.

| Source in the board map | Input | Value |
|---|---|---|
| `battery_adc` | Named ADC channel wired to the pack through a known divider | Battery millivolts, rounded to the nearest mV after scaling |
| `sbs_voltage` | Named I²C bus connected to an SBS gauge | Pack voltage in mV |
| `sbs_percent` | Same bus | Relative state of charge, 0–100% |
| `sbs_charging` | Same bus | 1 for positive current into the pack, 0 for idle or discharge |

For a voltage divider, first declare the wired ADC channel in the board map. On ESP-IDF use a supported ADC1 pin. On Zephyr use an ADC channel exposed by the board's app hardware devicetree. This map uses a channel named `vbat_adc` whose pin sees half the pack voltage.

```json
{"sensor":{"battery_mv":{"source":"battery_adc","input":"vbat_adc","divider_num":2,"divider_den":1}}}
```

The multiplier is `divider_num / divider_den`, both positive integers with the numerator at least as large as the denominator. Use the actual divider ratio and keep the input within the board's ADC range. Accuracy depends on the ADC calibration and resistor tolerances. Configure this source only where the divider is wired. A floating pin can return a plausible voltage.

For a Smart Battery gauge on an existing bus named `i2c0`, use this map. The firmware reads standard two-byte little-endian words at the 7-bit address `0x0b` for Voltage (0x09), RelativeStateOfCharge (0x0d) and signed Current (0x0a). Omit channels the gauge does not implement.

```json
{
  "sensor": {
    "battery_mv":       {"source":"sbs_voltage",  "input":"i2c0"},
    "battery_pct":      {"source":"sbs_percent",  "input":"i2c0"},
    "battery_charging": {"source":"sbs_charging", "input":"i2c0"}
  }
}
```

A successful read returns 0 and writes the value. On failure the output stays unchanged. An unmapped name returns `FC_ENOENT`, a missing grant gives `FC_EPERM`, and a failed bus read returns the driver's error. Charge above 100% is rejected with `FC_EIO`. A divider supplies voltage only.

The example `battery` app uses the names above. Set `FC_ALLOW` to `battery_mv battery_pct battery_charging metrics` and configure the threshold for your pack in `/etc/fc-app/battery.json`, for example `{"low_mv":3300,"sample_ms":30000}`. It samples at startup, on config change and every 30 seconds by default (minimum 1 second). It reports `app.battery.low_battery` as 1 below a positive threshold and 0 at or above it. Without a configured threshold it skips that gauge. Other gauges are `voltage_mv`, `charge_pct` and `charging` under the same prefix. Each updates only on a valid reading. The telemetry gateway may continue to receive the last good value after a read error. Flight Control shows `battery unavailable` when the voltage channel is absent or unsupported, `battery access denied` for a missing grant, and `battery read failed` for a transient error.

## Config and storage

Put app settings in `/etc/fc-app/<app name>.json` as a flat JSON object. `fc_config_get` reads a value as text. Use `fc_config_int`, `fc_config_double` or `fc_config_milli` for numbers (`"40.5"` becomes 40500 thousandths). A changed file raises `FC_EV_CONFIG`. The app can reload settings without restarting.

Use `fc_kv_*` for infrequent writes such as calibration. Each app has 4 KB for keys and values, with up to 31 characters per key. Each write replaces the whole flash record. Data survives reboots and upgrades until the app is removed.

During an update, flash must hold the replacement alongside resident modules and the catalog. The agent verifies the new module, commits its catalog entry, then reclaims the old copy. A failed write leaves the old version active. Three maximum-size modules exceed the Zephyr S3's 176 KiB storage partition. Older firmware needs registry access to reload apps updated to the new catalog format.

`fc_info` returns device and app names, firmware version and board. Before enrollment the device name returns `FC_EAGAIN`.

## Where the data goes

- Use `fc_set_state` for occasional changes such as `calibrating` or `alarm`. Flight Control displays the text under `customInfo["app.<name>"]` and records changes as events.
- Use `fc_metric` for numeric gauges and counters. Grant `metrics` and [configure the Telemetry Gateway](METRICS.md) to export them.
- Use MQTT for payloads and commands. Grant `mqtt` and configure a broker as below.

### MQTT

All apps share one broker connection. Supply its settings and CA through the fleet.

```yaml
spec:
  template:
    spec:
      config:
        - name: mqtt
          inline:
            - path: /etc/fc/mqtt.json
              content: '{"broker": "mqtts://broker.example.com:8883", "username": "pumps", "password": "...",
                         "fleetPrefix": "flightctl/fleet/pumps/{app}/"}'
            - path: /etc/fc/mqtt-ca.crt
              content: |
                -----BEGIN CERTIFICATE-----
                ...
```

| Broker setting | Meaning |
|---|---|
| `broker` | Required. `mqtts://host[:port]` or `host:port` uses TLS and verifies the hostname against `mqtt-ca.crt` (default port 8883). `mqtt://host[:port]` uses plain TCP (default 1883). |
| `username`, `password` | Optional broker credentials. Anyone who can read the inline fleet spec can read them. Restrict the broker account to the fleet’s prefixes. |
| `clientId` | `{device}` by default, the device's name in Flight Control |
| `keepAliveSeconds` | 60 by default, 10 to 3600 |
| `devicePrefix` | where an app publishes and subscribes, `flightctl/{device}/{app}/` by default |
| `fleetPrefix` | where an app may subscribe too, for commands to every device of the fleet; none by default |

The device substitutes `{device}` and `{app}` in each prefix and adds a trailing slash if needed. Prefixes cannot contain wildcards. App topics are relative. For example, `pump` on device `abc` publishes `temp` to `flightctl/abc/pump/temp`. Subscription filters accept `+` and `#`.

| Behavior | Limit |
|---|---|
| Delivery | QoS 0 or 1. An unacknowledged publish is lost on disconnect. No persistent sessions, QoS 2, will messages or client certificates. |
| Message | 512-byte payload, 64-character relative topic. Oversized or unqueueable messages are dropped, counted and acknowledged. |
| Incoming queue | 4 KB shared. One app can hold half. Up to 16 subscriptions across all apps. |
| Reconnect | First retry after 1–2 s, backing off to two minutes. Subscriptions are restored. Apps receive `FC_EV_NET` on connection (1) or loss (0). |

After enrollment the agent starts the client before the apps. Changing either config file restarts it. The handshake waits for an accepted status, idle management traffic, a contiguous 24 KiB block and 12 KiB free above the agent's reserve. Once connected, MQTT stays up during downloads. [Board measurements](RESULTS.md#apps-on-the-boards)

For a local broker, `tools/mqtt/up.sh` starts Mosquitto at `mqtt.<LAN IP>.nip.io:8883`. It creates a CA and a user restricted to `flightctl/#`. Use `tools/fleet/mqtt-config.py` to add the two config files to a fleet.

## Ports

On Zephyr, the fleet can alias a hardware node in the board map. Grant the alias in `FC_ALLOW`. The S3 overlay exposes the physical die probe as `core-temperature`. The legacy `die_temp` reading and LED remain simulated on Zephyr. [Zephyr hardware configuration](ZEPHYR.md#hardware-for-apps)

| | ESP-IDF | Zephyr | Host simulator |
|---|---|---|---|
| GPIO with interrupts | yes | yes, the devicetree's names | simulated; nothing presses `button0` |
| `led0` | the board's LED (`CONFIG_FC_LED_GPIO`, WS2812 or plain) unless `board.json` moves it | simulated, printed on the console | simulated, printed |
| `die_temp` | the chip's sensor; `FC_ENOTSUP` on the ESP32, which has none | simulated: 30 to 46 °C over two minutes | simulated, the same |
| I2C | master, any two pins; up to two buses on the ESP32 and S3, one on the C chips | the devicetree's controllers; a transfer lasts as long as the driver makes it | `i2c0`, with one device at 0x48: register 0 the simulated temperature in hundredths of a degree, 2 and 3 writable |
| ADC | ADC1's pins, 0 to about 3.1 V, calibrated from the chip's eFuse; up to 8 channels | the devicetree's channels, in millivolts by their reference | 1650 mV on any pin |
| PWM | LEDC, up to 4, each at its own frequency, duty to 14 bits | the devicetree's channels | printed when it changes |
| SPI | master, up to 4 devices on one bus (two buses on the ESP32 and S3), chip select driven by the firmware; transfers in pieces of 64 bytes, without DMA | the devicetree's devices, chip select driven by the firmware; a transfer lasts as long as the driver makes it | MISO wired to MOSI: what is sent comes back |
| UART | UART1, and UART2 on the ESP32 and S3 (UART0 is the console's); RS-485 half duplex with `de`; 512 bytes received ahead | interrupt-driven, 512 bytes received ahead, excluding the console. On ESP32-S3, enable runtime UART configuration and set `de-gpios` for RS-485. The port holds DE through the last stop bit, including after a timed-out write or close. Other controllers with `de-gpios` return `FC_ENOTSUP` | RX wired to TX: what is written comes back, with `FC_EV_UART` |

## Size and limits

| Resource | Limit |
|---|---|
| Apps per device | 2 |
| WASM file | Up to 64 KiB per download. With the app heap check enabled, the limit is the smaller of 64 KiB and one quarter of the free heap above the agent’s 40 KiB reserve. |
| Linear memory | The SDK build declares one 64 KiB page, including the app’s stack and any `malloc` arena. Other modules can declare more. WAMR can shrink the allocation when the module exports `__heap_base` and `__data_end` and the 16-byte-aligned data end fits below or at the heap base. |
| Open handles per app (GPIOs, LEDs, timers...) | 12 |
| Timers per app | 4 |
| Storage per app | 4 KB |
| Queued events | 16 per app; past that the oldest go, and the app hears how many with `FC_EV_OVERFLOW` |
| One call of `init`, `tick` or `on_event` | 5 million instructions, and 1 s in the device's functions (waits, and the C library) |
| The export in `FC_ENTRY`, or `_start` | no limit, stopped when the app is removed or updated |
| One wait (`fc_sleep_ms`, `fc_uart_read`), one I2C transfer, one UART write, or waiting for another app's SPI device | 100 ms |
| One I2C transfer | 1 KB each way (`FC_I2C_MAX`) |
| One SPI transfer | 4 KB (`FC_SPI_MAX`) |

**Memory at startup.** On ESP-IDF the decoded module typically needs 2.5–3.5 times its file size. Startup also needs contiguous linear memory, about 3.5 KiB for the instance and an 8 KiB thread stack. An update can briefly hold both module versions. After instantiation the agent requires 40 KiB of free internal RAM to reconnect. In `customInfo.heapAgentBytes`, compare `after` (free bytes after startup), `low` (subsequent minimum) and `floor` (reserve). On Zephyr the equivalent check (`CONFIG_FC_APP_HEAP_CHECK`) is off by default and unverified on hardware. [Measured RAM use](RESULTS.md#ram)

**Two apps.** Set `FC_PRIMARY: "true"` on one app to load it first. A secondary can start once the primary has its fixed memory and sufficient RAM remains. If the primary can still grow its linear memory, the secondary waits. Updating or restarting the primary can park the secondary, releasing its hardware, thread and module. Its stored code and data survive. A completed one-shot keeps its result.

**When allocation fails**, status distinguishes a lack of contiguous space from insufficient free heap. The agent checks for enough memory every 30 s. For a primary it can release idle management TLS and reload the committed module from flash. On C5 and C6 without PSRAM, the loader tries to reserve a contiguous block early for eligible linear memories of 16 KiB or more. [CoreMark measurements](RESULTS.md#coremark)

**Build flags.** Keep the flags from `wasm-apps/build.sh`, including `-mcpu=mvp` and exports for `__heap_base` and `__data_end`. WAMR supports tail calls but lacks reference types. If an app needs a new firmware import, deploy the firmware and app in the same spec. The agent installs and confirms firmware first. An unsupported import rejects the app update and preserves the installed version. Rebuild legacy apps that import `env.set_state`, `env.log` or `env.led`.

Split long computations across callbacks. The instruction limit counts interpreter operations. Native calls and waits share a separate time allowance per callback. Hitting either limit stops the app. [Time per board](RESULTS.md#timings)

### The C library

Apps import these native functions from `env`. Their declarations are in the [SDK libc headers](../wasm-apps/sdk/libc/).

| Header | Functions |
|---|---|
| `string.h` | `memcmp`, `memcpy`, `memmove`, `memset`, `memchr`, `strlen`, `strcmp`, `strncmp`, `strncasecmp`, `strcpy`, `strncpy`, `strchr`, `strrchr`, `strstr`, `strspn`, `strcspn` |
| `stdio.h` | `snprintf`, `sprintf`, `printf`, `puts`, `putchar` |
| `stdlib.h` | `atoi`, `strtol`, `strtoul`, `abort` |
| `ctype.h` | `isalnum`, `isalpha`, `isdigit`, `isgraph`, `isprint`, `isspace`, `isupper`, `isxdigit`, `tolower`, `toupper` |
| `math.h` | `expf`, `logf`, `tanhf` |

Native calls count toward the callback's time limit. An out-of-bounds pointer, invalid buffer length or `abort()` traps the app.

`printf`, `puts` and `putchar` write INFO-level app logs. Lines split at 96 characters. A single `printf` writes up to 127 characters. Pending text is flushed when the callback returns. The port's libc formats conversions, including widths, precisions and `%f`.

For `malloc`, `calloc`, `realloc` and `free`, declare an arena at file scope, e.g. `FC_HEAP(4096);`. It counts toward the module's fixed linear memory. Each allocation adds four bytes and rounds up to eight. `malloc` returns NULL if no free block fits.

The SDK also maps common math operations such as `sqrtf` and `fabsf` directly to WASM instructions. The linker permits only the libc imports listed in [libc.syms](../wasm-apps/sdk/libc/libc.syms). Files and `vsnprintf` are unavailable.

## Examples

| App | What to try |
|---|---|
| [alarm](../wasm-apps/alarm/alarm.c) | Set `sensor` (default `die_temp`), `threshold_c`, `hysteresis_c` and `sample_ms` in `/etc/fc-app/alarm.json`. Grant `led0` for a blue warning 2 °C below the threshold and a blinking alarm. |
| [battery](../wasm-apps/battery/battery.c) | Polls named battery channels and publishes voltage, low-battery, relative charge and charging gauges. [Configuration](#battery-channels) |
| [trend](../wasm-apps/trend/trend.c) | Reports temperature and rate of change. Grant `metrics` to export both. Like `alarm`, it stays idle on chips without a temperature sensor. |
| [libccheck](../wasm-apps/libccheck/libccheck.c) | Exercises libc. A passing run reports `bad=0`. |
| [hwcheck](../wasm-apps/hwcheck/hwcheck.c) | Grant `i2c0 spi0 uart0 adc0 pwm0` to inspect bus responses, UART loopback, ADC voltage and PWM output. |
| [mqttdemo](../wasm-apps/mqttdemo/mqttdemo.c) | Grant `mqtt` to publish temperature on `temp` and receive commands on device `cmd/#` and fleet `cmd`. Replies appear on `ack`. Grant `led0` for LED commands. |
| [CoreMark](../wasm-apps/coremark/fetch.sh) | Fetch both verified modules. Publish the smaller one with `wasm-apps/push.sh coremark wasm3 v0.9.0-wasm3`. See [one-shot deployment](#modules-built-for-no-device). |

### TinyML with emlearn

[tinyml](../wasm-apps/tinyml/tinyml.c) classifies 12 temperature samples as steady, warming, cooling or unstable. The random forest has five trees and 193 nodes. It uses 1.2 KB of linear memory in a 4.1 KB module. [tinymlp](../wasm-apps/tinymlp/tinymlp.c) uses the same features with a 236-parameter neural network and also reports confidence. It imports `expf`, `logf` and `tanhf` and uses 2.4 KB of linear memory in a 6.8 KB module. Neither needs grants.

The models assume 5 s between samples. Setting `sample_ms` changes the sampling interval without retraining. To reproduce the forest, use the pinned [training requirements](../wasm-apps/tinyml/requirements.txt) and [training script](../wasm-apps/tinyml/train.py). For your own forest, `dtype="float"` preserves fractional thresholds. The [feature adapter](../wasm-apps/tinyml/features.h) accounts for the difference between strict and inclusive thresholds on integer inputs. emlearn uses majority votes, so close calls can differ from scikit-learn's averaged probabilities.

### The blink of wasm3-arduino

The device runs the [wasm3-arduino blink](https://github.com/wasm3/wasm3-arduino) as published in Rust (283 B), C++ (369 B), AssemblyScript (2.2 KB) and TinyGo (5.2 KB). Grant `FC_ALLOW: "led0"`. To fetch the modules, verify their SHA-256 and publish the Rust build:

```bash
wasm-apps/arduino/fetch.sh
wasm-apps/push.sh arduino rust v0.9.0-rust
```

| Function | On the device |
|---|---|
| `pinMode(pin, mode)`, `digitalWrite(pin, value)` | pin n is the n-th name of `FC_ALLOW`, counted from 0: a GPIO opens as `fc_gpio_open` opens it, an LED lights white. One app holds a name at a time, and an output goes back to its safe level when the app stops |
| `getPinLED()` | the pin of `led0` in `FC_ALLOW` |
| `delay(ms)` | any length, in steps of 10 ms (`delay(0)` and `delay(1)` wait 10 ms), each one letting the device's other threads run; removing or updating the app stops it within 10 ms |
| `millis()` | milliseconds since the app started |
| `print(s, len)` | the app's log, a line at a time, as `printf` |
| `getGreeting(buf, len)` | `Hello from Flight Control` |

Only `getPinLED()` and `millis()` return values. Each blink exports a non-returning `_start()` and stays `Running` until stopped. In a callback, `delay` counts toward its native-call limit. Each build needs 64 KiB of contiguous memory, which can prevent a second app from starting. [Board results](RESULTS.md#apps-on-the-boards)
