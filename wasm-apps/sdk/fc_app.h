/*
 * flightctl-mcu WASM app interface, version 1.
 *
 * docs/APPS.md says how to write an app and what each port has. Until the
 * v1.0.0 tag this interface can still change; from then on, functions are
 * only added within version 1. Buses, ADC, PWM, metrics and MQTT are not in
 * the firmware yet: they return FC_ENOTSUP once the name exists and is
 * granted.
 *
 * An app is a WebAssembly module (wasm32, MVP, no libc, no WASI) deployed by
 * Flight Control as a container application whose image is a WASM OCI
 * artifact. The device runs it with WAMR, one thread per app. Everything the
 * app can reach goes through the functions below (import module "fc").
 *
 * ---- What the app exports ----------------------------------------------------
 *
 *   fc_abi_version()      required, declared with FC_APP_ABI(). A device with
 *                         an older interface refuses the app with a clear error.
 *   init()                optional. Called once after the app is loaded, before
 *                         anything else. A non-zero return stops the app with
 *                         Error; it is restarted after a back-off, like a trap.
 *   tick()                optional. Called every envVars.TICK_MS milliseconds
 *                         (10 to 60000, default 100). The time is fc_time_ms().
 *   on_event(type, h, v)  optional. Called for each event (FC_EV_*), with the
 *                         handle it concerns and a value that depends on it.
 *
 *   The prototypes are declared below, so a definition with another signature
 *   does not compile. The device checks them too when it loads the app, and
 *   refuses an app that exports neither tick nor on_event, or that imports
 *   from module "env" (built for the interface before 1.0: rebuild it).
 *
 *   All of them run on the app's own thread, one at a time: the app needs no
 *   locks. A call may run a budget of instructions, about a second of work
 *   on the slowest chip, and spend at most FC_CALL_LIMIT_MS waiting in the
 *   device's functions (fc_sleep_ms...); past either, the app is stopped as
 *   if it had trapped. Events wait in a queue of FC_EVENT_QUEUE
 *   entries. A GPIO edge replaces the one still queued for the same GPIO; for
 *   the others, when the queue is full, the oldest are dropped and the next
 *   event is FC_EV_OVERFLOW with the number lost.
 *
 * ---- Hardware: names, not pin numbers --------------------------------------
 *
 * The app opens GPIOs, buses and channels by name ("led0", "i2c0",
 * "button0"). The names come from the board map: the firmware's defaults for
 * the board, which the fleet can override per device with the config file
 * /etc/fc/board.json (see docs/APPS.md). A name is unique in the whole map,
 * whatever its kind. The same app then runs on any board whose map has the
 * names it uses.
 *
 * An app reaches only what its fleet grants it, in envVars.FC_ALLOW: names from
 * the board map, plus "mqtt" and "metrics", separated by spaces. Several apps
 * may be granted a name, but only one holds it at a time: an app that opens a
 * name held by another gets FC_EBUSY. Without FC_ALLOW an app has log, state,
 * info, config, storage, time, timers and the chip's sensors, and no GPIO, bus
 * or network.
 *
 * A change to the app's envVars, or a new board map, restarts the app.
 *
 * ---- Calls -------------------------------------------------------------------
 *
 * Strings and buffers are a pointer and a length; strings need no NUL.
 * Functions that open something return a handle (>= 0), the others 0 or a
 * count of bytes; on failure all return a negative FC_E* code. A handle is
 * released with fc_close(): that stops a timer and ends a subscription.
 *
 * ---- When the app stops ---------------------------------------------------------
 *
 * A trap (out-of-bounds access, unreachable, a call past its limits)
 * stops the app; the device restarts it after a few seconds and counts it in
 * status.applications[].restarts. When the app stops for any reason (trap,
 * update, removal, restart), the device closes its handles and leaves the
 * hardware safe: a GPIO output goes to the board map's "safe" level, or back
 * to an input when the map gives none; a PWM goes to duty 0; an LED goes off.
 *
 * Memory: link with wasm-apps/build.sh, which keeps the module to a few KB of
 * linear memory (see the flags there).
 */
#pragma once

#include <stdint.h>

#define FC_APP_ABI_VERSION 1

#define FC_IMPORT(name) __attribute__((import_module("fc"), import_name(name)))
#define FC_EXPORT(name) __attribute__((export_name(name)))

/* Declares the interface version the app was built against. */
#define FC_APP_ABI()                                                                                         \
    FC_EXPORT("fc_abi_version") int32_t fc_abi_version(void) { return FC_APP_ABI_VERSION; }

/* What the app may define (see above). */
FC_EXPORT("init") int32_t init(void);
FC_EXPORT("tick") void tick(void);
FC_EXPORT("on_event") void on_event(int32_t type, int32_t handle, int32_t value);

#define FC_CALL_LIMIT_MS 1000 /* longest wait of one init, tick or on_event */

/* ---- Errors ------------------------------------------------------------------ */

#define FC_EINVAL    (-1)  /* a bad argument */
#define FC_ENOENT    (-2)  /* no such name in the board map, key, or config */
#define FC_EPERM     (-3)  /* not granted in FC_ALLOW */
#define FC_EBUSY     (-4)  /* held by another app, or by the firmware */
#define FC_EIO       (-5)  /* the bus or the device failed (I2C NACK, ...) */
#define FC_ETIMEDOUT (-6)
#define FC_ENOSPC    (-7)  /* a buffer too small, or the app's storage full */
#define FC_ENOTSUP   (-8)  /* this board or firmware does not have it */
#define FC_EAGAIN    (-9)  /* not now: no data yet, not connected yet */

/* ---- Events (on_event type) ---------------------------------------------------
 *                         handle            value */
#define FC_EV_GPIO     1 /* the GPIO          its level after the last edge */
#define FC_EV_TIMER    2 /* the timer         0 */
#define FC_EV_UART     3 /* the UART          bytes waiting to be read */
#define FC_EV_MQTT     4 /* the subscription  payload length: fc_event_data, fc_mqtt_topic */
#define FC_EV_CONFIG   5 /* -1                0: the app's config changed */
#define FC_EV_NET      6 /* -1                1 connected to the broker, 0 lost */
#define FC_EV_OVERFLOW 7 /* -1                events dropped */

#define FC_EVENT_QUEUE 16

/* ---- Device: state, log, info, time -------------------------------------------- */

/* Free text shown in Flight Control as customInfo["app.<name>"], at most 63
 * characters. Each change is an event on the server: send it when the
 * app's state changes, not values that move all the time (use fc_metric). */
FC_IMPORT("set_state") void fc_set_state(const char *s, int32_t len);

#define FC_LOG_ERROR 0
#define FC_LOG_WARN  1
#define FC_LOG_INFO  2
#define FC_LOG_DEBUG 3
/* A line in the device log, prefixed with the app's name. */
FC_IMPORT("log") void fc_log(int32_t level, const char *s, int32_t len);

/* Facts about where the app runs, as text: "device" (its name in Flight
 * Control), "app" (the app's name), "firmware" (its version), "board" (the
 * board map's board). Returns the length, FC_ENOENT for another key,
 * FC_EAGAIN for "device" before the device has enrolled, or FC_ENOSPC with
 * nothing written if cap is too small. */
FC_IMPORT("info") int32_t fc_info(const char *key, int32_t klen, char *buf, int32_t cap);

/* Milliseconds since boot (does not wrap). */
FC_IMPORT("time_ms") int64_t fc_time_ms(void);
/* Wall clock in milliseconds since 1970 (UTC), or FC_EAGAIN before the device
 * has set its clock (SNTP). */
FC_IMPORT("time_unix_ms") int64_t fc_time_unix_ms(void);
/* Blocks the app for up to FC_WAIT_MAX_MS: the pause a sensor needs between a
 * command and its reading. Longer waits are timers. Timing finer than a
 * millisecond (bit-banged 1-Wire, DHT) is not possible from an app. */
#define FC_WAIT_MAX_MS 100
FC_IMPORT("sleep_ms") int32_t fc_sleep_ms(int32_t ms);
/* Fills buf with random bytes from the hardware generator. */
FC_IMPORT("random") int32_t fc_random(void *buf, int32_t len);

/* ---- Timers -------------------------------------------------------------------
 * A timer raises FC_EV_TIMER after ms, once or every ms. Returns a handle;
 * FC_ENOSPC past FC_MAX_TIMERS per app. fc_close stops it; a one-shot timer
 * that has fired still holds its slot until it is closed. */
#define FC_MAX_TIMERS 4
FC_IMPORT("timer_start") int32_t fc_timer_start(int32_t ms, int32_t periodic);

/* ---- Config from the fleet ----------------------------------------------------
 * A key of the config file /etc/fc-app/<app name>.json from the fleet (a flat
 * JSON object: strings, numbers and booleans, given back as text). The app's
 * envVars are the device's (FC_ALLOW, TICK_MS) and are not read here. Returns
 * the value's length, or FC_ENOENT; FC_ENOSPC with nothing written if cap is
 * too small. A new file raises FC_EV_CONFIG. */
FC_IMPORT("config_get") int32_t fc_config_get(const char *key, int32_t klen, char *buf, int32_t cap);

/* ---- Storage that survives a reboot --------------------------------------------
 * Each app has its own keys, kept until the app is removed from the device or
 * its name changes; a new version of the app keeps them. They are stored as
 * one record per app, so each fc_kv_set or fc_kv_del rewrites the whole
 * record: write seldom, flash wears out. At most FC_KV_QUOTA bytes per app,
 * keys and values together; keys up to FC_KV_KEY_MAX characters. */
#define FC_KV_QUOTA   4096
#define FC_KV_KEY_MAX 31
FC_IMPORT("kv_get") int32_t fc_kv_get(const char *key, int32_t klen, void *buf, int32_t cap);
FC_IMPORT("kv_set") int32_t fc_kv_set(const char *key, int32_t klen, const void *data, int32_t len);
FC_IMPORT("kv_del") int32_t fc_kv_del(const char *key, int32_t klen);

/* ---- Handles -------------------------------------------------------------------- */

/* At most FC_MAX_HANDLES open at once per app: FC_ENOSPC past that. */
#define FC_MAX_HANDLES 12
FC_IMPORT("close") int32_t fc_close(int32_t h);

/* ---- GPIO ------------------------------------------------------------------------- */

#define FC_GPIO_IN          0
#define FC_GPIO_IN_PULLUP   1
#define FC_GPIO_IN_PULLDOWN 2
#define FC_GPIO_OUT         3
#define FC_GPIO_OUT_OD      4 /* open drain */

#define FC_EDGE_NONE    0
#define FC_EDGE_RISING  1
#define FC_EDGE_FALLING 2
#define FC_EDGE_BOTH    3

FC_IMPORT("gpio_open") int32_t fc_gpio_open(const char *name, int32_t nlen, int32_t mode);
/* 0 or 1. The board map can mark a pin active low; levels are then logical. */
FC_IMPORT("gpio_read") int32_t fc_gpio_read(int32_t h);
FC_IMPORT("gpio_write") int32_t fc_gpio_write(int32_t h, int32_t level);
/* Raises FC_EV_GPIO on the edge, ignoring further edges for debounce_ms. */
FC_IMPORT("gpio_irq") int32_t fc_gpio_irq(int32_t h, int32_t edge, int32_t debounce_ms);

/* ---- LED -------------------------------------------------------------------------
 * The board's LEDs by name ("led0"), plain or addressable (WS2812). A plain LED
 * lights for any non-zero colour. */
FC_IMPORT("led_open") int32_t fc_led_open(const char *name, int32_t nlen);
FC_IMPORT("led_rgb") int32_t fc_led_rgb(int32_t h, int32_t r, int32_t g, int32_t b);

/* ---- I2C ---------------------------------------------------------------------------
 * A bus by name; pins and speed are in the board map. addr is 7-bit. */
FC_IMPORT("i2c_open") int32_t fc_i2c_open(const char *bus, int32_t blen);
FC_IMPORT("i2c_write") int32_t fc_i2c_write(int32_t h, int32_t addr, const void *data, int32_t len);
FC_IMPORT("i2c_read") int32_t fc_i2c_read(int32_t h, int32_t addr, void *buf, int32_t len);
/* Write then read without a stop in between (a register read). */
FC_IMPORT("i2c_write_read")
int32_t fc_i2c_write_read(int32_t h, int32_t addr, const void *w, int32_t wlen, void *r, int32_t rlen);

/* ---- SPI ----------------------------------------------------------------------------
 * A device on a bus by name: the board map gives the bus, chip select, mode and
 * clock. tx or rx may be 0 for a one-way transfer. With FC_SPI_KEEP_CS the chip
 * select stays asserted for the next transfer (a command, then its reply); the
 * bus stays the app's until a transfer without it, or the end of the call. */
#define FC_SPI_KEEP_CS 1
FC_IMPORT("spi_open") int32_t fc_spi_open(const char *name, int32_t nlen);
FC_IMPORT("spi_transfer")
int32_t fc_spi_transfer(int32_t h, const void *tx, void *rx, int32_t len, int32_t flags);

/* ---- UART -----------------------------------------------------------------------------
 * baud 0 takes the board map's. format: FC_UART_8N1 and the like. For RS-485
 * the board map names the pin that enables the driver; the device drives it.
 * Reads wait at most timeout_ms, itself at most FC_WAIT_MAX_MS; FC_EV_UART
 * says data is waiting. 8E1 and 8N2 are Modbus RTU's two framings. */
#define FC_UART_8N1 0
#define FC_UART_8E1 1
#define FC_UART_8N2 2
FC_IMPORT("uart_open") int32_t fc_uart_open(const char *name, int32_t nlen, int32_t baud, int32_t format);
FC_IMPORT("uart_write") int32_t fc_uart_write(int32_t h, const void *data, int32_t len);
FC_IMPORT("uart_read") int32_t fc_uart_read(int32_t h, void *buf, int32_t cap, int32_t timeout_ms);

/* ---- ADC and PWM ------------------------------------------------------------------------ */

/* Millivolts at the pin, calibrated where the chip allows it. */
FC_IMPORT("adc_open") int32_t fc_adc_open(const char *name, int32_t nlen);
FC_IMPORT("adc_read_mv") int32_t fc_adc_read_mv(int32_t h);

/* duty from 0 (off) to 65535 (always on). */
FC_IMPORT("pwm_open") int32_t fc_pwm_open(const char *name, int32_t nlen, int32_t freq_hz);
FC_IMPORT("pwm_duty") int32_t fc_pwm_duty(int32_t h, int32_t duty);

/* ---- Sensors the chip has ------------------------------------------------------------------
 * What is inside the chip or on the board and has no bus of its own: "die_temp"
 * (milli-degrees C) on the chips that have one. External sensors are read by
 * the app over I2C or SPI. Needs no grant. */
FC_IMPORT("sensor_read") int32_t fc_sensor_read(const char *name, int32_t nlen, int32_t *value);

/* ---- Metrics (granted by "metrics") ----------------------------------------------------------
 * A number for Flight Control's Telemetry Gateway (OpenTelemetry), which adds
 * the device's ID and exports it to Prometheus. The metric's name is
 * app.<app name>.<name> (a proposal: the exporter decides how it looks in
 * Prometheus). The device sends the last value of each metric every
 * metrics interval, in one batch. At most FC_MAX_METRICS per app. */
#define FC_MAX_METRICS 8
#define FC_METRIC_GAUGE   0 /* the value as it is */
#define FC_METRIC_COUNTER 1 /* fc_metric adds to it */
FC_IMPORT("metric") int32_t fc_metric(const char *name, int32_t nlen, int32_t kind, double value);

/* ---- MQTT (granted by "mqtt") ------------------------------------------------------------------
 * One connection to the broker the fleet gives (/etc/fc/mqtt.json and its CA),
 * shared by all the apps, kept open and reconnected by the device. Topics are
 * relative, under one of two prefixes from mqtt.json:
 *   FC_MQTT_DEVICE  the device's, by default "flightctl/<device name>/<app name>/":
 *                   the app publishes and subscribes there.
 *   FC_MQTT_FLEET   the fleet's, which the fleet's template writes with its
 *                   name, such as "flightctl/fleet/pumps/<app name>/": subscribe
 *                   only, for commands to every device of the fleet. Without
 *                   it in mqtt.json, subscribing there gives FC_ENOENT.
 * An app reaches nothing outside them. QoS is 0 or 1. Topics are at most
 * FC_MQTT_MAX_TOPIC characters and payloads FC_MQTT_MAX_PAYLOAD bytes, both
 * ways: a longer message received is dropped and counted in the device log.
 * Publishing while disconnected returns FC_EAGAIN; FC_EV_NET says when the link
 * comes and goes. fc_mqtt_subscribe returns a handle, closed with fc_close. A
 * message on a subscription raises FC_EV_MQTT with that handle; during that
 * call, fc_event_data gives the payload and fc_mqtt_topic the relative topic. */
#define FC_MQTT_DEVICE      0
#define FC_MQTT_FLEET       1
#define FC_MQTT_MAX_TOPIC   64
#define FC_MQTT_MAX_PAYLOAD 512
FC_IMPORT("mqtt_publish")
int32_t fc_mqtt_publish(const char *topic, int32_t tlen, const void *payload, int32_t plen, int32_t qos,
                        int32_t retain);
FC_IMPORT("mqtt_subscribe")
int32_t fc_mqtt_subscribe(int32_t scope, const char *topic, int32_t tlen, int32_t qos);
FC_IMPORT("mqtt_topic") int32_t fc_mqtt_topic(char *buf, int32_t cap);

/* The data of the event being handled (an MQTT payload), during on_event. */
FC_IMPORT("event_data") int32_t fc_event_data(void *buf, int32_t cap);

/* ---- Helpers (no libc) ------------------------------------------------------------------------ */

#define FC_STATE(lit)       fc_set_state(lit, sizeof(lit) - 1)
#define FC_LOG(level, lit)  fc_log(level, lit, sizeof(lit) - 1)
#define FC_STR(lit)         lit, (int32_t)(sizeof(lit) - 1)

/* Small integer formatting, for states and log lines without printf.
 * Writes at most 12 characters; returns the number written. */
static inline int32_t fc_fmt_int(char *out, int32_t v)
{
    char tmp[12];
    int32_t n = 0, len = 0;
    uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && n < 11);
    if (v < 0) {
        out[len++] = '-';
    }
    while (n) {
        out[len++] = tmp[--n];
    }
    return len;
}

/* An integer from the config, or def when the key is missing, not a whole
 * number, or out of the int32 range. */
static inline int32_t fc_config_int(const char *key, int32_t klen, int32_t def)
{
    char b[16];
    int32_t n = fc_config_get(key, klen, b, sizeof(b)), i = 0, neg = 0;
    int64_t v = 0;
    if (n <= 0) {
        return def;
    }
    if (b[0] == '-') {
        neg = 1;
        i = 1;
    }
    if (i == n) {
        return def;
    }
    for (; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') {
            return def;
        }
        v = v * 10 + (b[i] - '0');
        if (v > (int64_t)INT32_MAX + neg) {
            return def;
        }
    }
    return (int32_t)(neg ? -v : v);
}

/* A number from the config ("40.5", "-3", "1.5e-4"), or def when the key is
 * missing, not a number, or too large for a double. Precise to about 15
 * significant digits, which is what a JSON config can carry. */
static inline double fc_config_double(const char *key, int32_t klen, double def)
{
    char b[32];
    int32_t n = fc_config_get(key, klen, b, sizeof(b)), i = 0, digits = 0, frac = 0, dot = 0, exp = 0;
    int32_t eneg = 0, edigits = 0;
    double v = 0, p = 1;
    if (n <= 0) {
        return def;
    }
    int32_t neg = b[0] == '-';
    for (i = neg; i < n && ((b[i] >= '0' && b[i] <= '9') || (b[i] == '.' && !dot)); i++) {
        if (b[i] == '.') {
            dot = 1;
            continue;
        }
        v = v * 10 + (b[i] - '0');
        digits++;
        frac += dot;
    }
    if (digits == 0) {
        return def;
    }
    if (i < n && (b[i] == 'e' || b[i] == 'E')) {
        i++;
        if (i < n && (b[i] == '-' || b[i] == '+')) {
            eneg = b[i] == '-';
            i++;
        }
        for (; i < n && b[i] >= '0' && b[i] <= '9'; i++) {
            exp = exp < 1000 ? exp * 10 + (b[i] - '0') : exp;
            edigits++;
        }
        if (edigits == 0) {
            return def;
        }
    }
    if (i != n) {
        return def;
    }
    exp = (eneg ? -exp : exp) - frac;
    for (int32_t k = exp < 0 ? -exp : exp; k > 0 && p < 1e308; k--) {
        p *= 10;
    }
    v = exp < 0 ? v / p : v * p;
    if (v - v != 0) { /* infinite */
        return def;
    }
    return neg ? -v : v;
}

/* A number from the config in thousandths, rounded ("40.5" gives 40500, "40"
 * gives 40000), or def when the key is missing, not a number, or out of the
 * int32 range. For degrees into milli-degrees, volts into millivolts, and the
 * like, to compare with what the device's functions return. */
static inline int32_t fc_config_milli(const char *key, int32_t klen, int32_t def)
{
    double v = fc_config_double(key, klen, __builtin_nan(""));
    if (v != v) {
        return def;
    }
    v = v * 1000 + (v < 0 ? -0.5 : 0.5);
    if (v >= 2147483648.0 || v <= -2147483649.0) {
        return def;
    }
    return (int32_t)v;
}
