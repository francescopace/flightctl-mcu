/*
 * Module "arduino": the functions the examples of wasm3-arduino import
 * (github.com/wasm3/wasm3-arduino, examples_pio/Wasm_Advanced/wasm_apps), so
 * that their blink runs as it is published (docs/APPS.md).
 *
 *   millis()                  ms since the app started
 *   delay(ms)                 any length, in steps of 10 ms; removing or
 *                             updating the app stops it
 *   pinMode(pin, mode)        INPUT 0, OUTPUT 1, INPUT_PULLUP 2
 *   digitalWrite(pin, value)  LOW 0, HIGH 1
 *   getPinLED()               the pin of led0
 *   print(s, len)             to the app's log, a line at a time
 *   getGreeting(buf, len)     "Hello from Flight Control"
 *
 * A pin is a name of the board map: pin n is the n-th name of the app's
 * FC_ALLOW, counted from 0, so an app reaches only what the fleet grants it.
 * With FC_ALLOW "led0 relay1", getPinLED() is 0 and pin 1 is relay1. A name
 * that is a GPIO opens as one; an LED (led0 on most boards) lights white.
 * They are opened as fc_gpio_open and fc_led_open open them: one app holds a
 * name at a time, and when the app stops an output goes back to its safe
 * level. These functions return nothing, as Arduino's: what fails is logged
 * once per pin.
 *
 * The published blinks export _start(), which never returns: the device runs
 * it once (FC_ENTRY), with no budget and no limit on its waits. In an app
 * with tick() or on_event(), a delay counts against the call's second in
 * the device's functions.
 */
#include <stdint.h>
#include <string.h>

#include "fc_apps_int.h"
#include "fc_port.h"

#define PINS   16
#define LED_ON 32 /* white, dim: as bright as a plain LED on most boards */

static const char GREETING[] = "Hello from Flight Control";

/* Each function does nothing when called as the module is instantiated, before
 * the app's context exists (a NULL app). */

/* Per app slot, for its current instance: the handle each pin opened as. */
typedef struct {
    wasm_module_inst_t inst;
    int64_t started_ms;
    int8_t h[PINS];
    uint32_t serial[PINS];
    uint32_t warned; /* pins already logged, bit PINS for any pin past them */
} pins_t;

static pins_t s_pins[FC_MAX_APPS];

static pins_t *pins_of(wasm_exec_env_t env, app_t *a)
{
    pins_t *p = &s_pins[a->slot];
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (p->inst != inst || p->started_ms != a->started_ms) { /* a new instance */
        memset(p, 0, sizeof(*p));
        memset(p->h, -1, sizeof(p->h));
        p->inst = inst;
        p->started_ms = a->started_ms;
    }
    return p;
}

/* The pin-th name of FC_ALLOW, or 0. */
static int pin_name(const app_t *a, uint32_t pin, char *out, size_t cap)
{
    const char *s = a->allow;
    for (uint32_t i = 0;; i++) {
        while (*s == ' ') {
            s++;
        }
        if (*s == '\0') {
            return 0;
        }
        size_t n = strcspn(s, " ");
        if (i == pin) {
            if (n >= cap) {
                return 0;
            }
            memcpy(out, s, n);
            out[n] = '\0';
            return 1;
        }
        s += n;
    }
}

/* Once per pin and instance: a blink writes its pin twice a second, and the
 * log keeps 4 KB. The pins past FC_ALLOW's names (getPinLED() without led0)
 * share one bit. */
static void warn(app_t *a, pins_t *p, uint32_t pin, const char *what, int rc)
{
    uint32_t bit = 1u << (pin < PINS ? pin : PINS);
    if (p->warned & bit) {
        return;
    }
    p->warned |= bit;
    fc_port_log(FC_LOG_WARN, "app %s: arduino pin %u: %s (%d)", a->name, (unsigned)pin, what, rc);
}

/* The open handle of a pin, or -1. */
static int pin_handle(app_t *a, pins_t *p, uint32_t pin)
{
    if (pin >= PINS || p->h[pin] < 0) {
        return -1;
    }
    const app_handle_t *h = &a->io->h[p->h[pin]];
    return (h->kind == H_GPIO || h->kind == H_LED) && h->serial == p->serial[pin] ? p->h[pin] : -1;
}

static void ard_pinMode(wasm_exec_env_t env, uint32_t pin, uint32_t mode)
{
    app_t *a = wasm_runtime_get_user_data(env);
    if (a == NULL) {
        return;
    }
    pins_t *p = pins_of(env, a);
    char name[32];
    if (pin >= PINS || !pin_name(a, pin, name, sizeof(name))) {
        warn(a, p, pin, "no such name in FC_ALLOW", FC_HW_EPERM);
        return;
    }
    int h = pin_handle(a, p, pin);
    if (h >= 0) {
        fc_appapi_close(env, h);
        p->h[pin] = -1;
    }
    int gm = mode == 1 ? FC_HW_GPIO_OUT : mode == 2 ? FC_HW_GPIO_IN_PULLUP : FC_HW_GPIO_IN;
    h = fc_appapi_gpio_open(env, name, gm);
    if (h == FC_HW_ENOENT && mode == 1) {
        h = fc_appapi_led_open(env, name); /* not a GPIO: an LED? */
    }
    if (h < 0) {
        warn(a, p, pin, "cannot open it", h);
        return;
    }
    p->h[pin] = (int8_t)h;
    p->serial[pin] = a->io->h[h].serial;
}

static void ard_digitalWrite(wasm_exec_env_t env, uint32_t pin, uint32_t value)
{
    app_t *a = wasm_runtime_get_user_data(env);
    if (a == NULL) {
        return;
    }
    pins_t *p = pins_of(env, a);
    int h = pin_handle(a, p, pin);
    int rc = FC_HW_EINVAL;
    if (h >= 0 && a->io->h[h].kind == H_LED) {
        int v = value ? LED_ON : 0;
        rc = fc_appapi_led_rgb(env, h, v, v, v);
    } else if (h >= 0) {
        rc = fc_appapi_gpio_write(env, h, value != 0);
    }
    if (rc < 0) {
        warn(a, p, pin, h < 0 ? "written before pinMode(OUTPUT)" : "cannot write it", rc);
    }
}

static uint32_t ard_getPinLED(wasm_exec_env_t env)
{
    app_t *a = wasm_runtime_get_user_data(env);
    char name[32];
    for (uint32_t pin = 0; a != NULL && pin < PINS && pin_name(a, pin, name, sizeof(name)); pin++) {
        if (strcmp(name, "led0") == 0) {
            return pin;
        }
    }
    return PINS; /* not granted: pinMode() says so */
}

static uint32_t ard_millis(wasm_exec_env_t env)
{
    app_t *a = wasm_runtime_get_user_data(env);
    return (uint32_t)(fc_apps_now_ms() - (a ? a->started_ms : 0));
}

/* In steps of a tick, at least one (fc_appapi_wait): delay(1) or delay(0) in
 * a loop still lets the other threads run, and a long delay in a call with
 * limits stops at the call's second. */
static void ard_delay(wasm_exec_env_t env, uint32_t ms)
{
    app_t *a = wasm_runtime_get_user_data(env);
    if (a != NULL) {
        fc_appapi_wait(env, a, ms);
    }
}

/* Into the app's line (a->io->out, as printf's): logged at each newline. */
static void ard_print(wasm_exec_env_t env, const char *s, uint32_t len)
{
    app_t *a = wasm_runtime_get_user_data(env);
    for (uint32_t i = 0; a != NULL && i < len; i++) {
        if (s[i] != '\n') {
            a->io->out[a->out_len++] = s[i];
        }
        if (s[i] == '\n' || a->out_len == (int)sizeof(a->io->out)) {
            fc_port_log(FC_LOG_INFO, "app %s: %.*s", a->name, a->out_len, a->io->out);
            a->out_len = 0;
        }
    }
}

static void ard_getGreeting(wasm_exec_env_t env, char *buf, uint32_t len)
{
    (void)env;
    if (len > 0) {
        size_t n = sizeof(GREETING) - 1 < len - 1 ? sizeof(GREETING) - 1 : len - 1;
        memcpy(buf, GREETING, n);
        buf[n] = '\0';
    }
}

/* Sorted by name, as WAMR looks them up. */
static const NativeSymbol s_arduino[] = {
    {"delay", (void *)ard_delay, "(i)", NULL},
    {"digitalWrite", (void *)ard_digitalWrite, "(ii)", NULL},
    {"getGreeting", (void *)ard_getGreeting, "(*~)", NULL},
    {"getPinLED", (void *)ard_getPinLED, "()i", NULL},
    {"millis", (void *)ard_millis, "()i", NULL},
    {"pinMode", (void *)ard_pinMode, "(ii)", NULL},
    {"print", (void *)ard_print, "(*~)", NULL},
};

NativeSymbol *fc_arduino_natives(int *count)
{
    *count = (int)(sizeof(s_arduino) / sizeof(s_arduino[0]));
    return (NativeSymbol *)s_arduino;
}
