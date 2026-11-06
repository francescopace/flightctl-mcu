/*
 * "alarm" WASM app: the LED logic of the line temperature alarm, deployed by
 * Flight Control as a container application (OCI image with a .wasm layer)
 * and run by the firmware with WAMR, without reboot.
 *
 * The firmware calls tick(now_ms) every TICK_MS (app env var, default 100).
 * Hardware and config come through host functions (module "env").
 *   v1: LED solid blue at or above the fleet threshold.
 *   v2: 1 C hysteresis, dim solid blue pre-alarm 2 C below, blinking blue alarm.
 * Blue only, so boards with a single-colour LED (most are blue) and RGB
 * boards look the same in a demo: solid or blinking is what matters.
 */
#include <stdint.h>

#define IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define EXPORT(name) __attribute__((export_name(name)))

IMPORT("temp_mc") int32_t temp_mc(void);          /* internal sensor, milli-C */
IMPORT("threshold_mc") int32_t threshold_mc(void); /* fleet config threshold_c */
IMPORT("led") void led(int32_t r, int32_t g, int32_t b);
IMPORT("set_state") void set_state(const char *s, int32_t len); /* -> status */

#ifndef ALARM_VERSION
#define ALARM_VERSION 1
#endif

static int alarm_on;
static int last_state = -1;

static void state(const char *s, int32_t len, int code)
{
    if (code != last_state) {
        set_state(s, len);
        last_state = code;
    }
}
#define STATE(lit, code) state(lit, sizeof(lit) - 1, code)

EXPORT("tick") void tick(int32_t now_ms)
{
    int32_t t = temp_mc(), thr = threshold_mc();
#if ALARM_VERSION == 1
    alarm_on = t >= thr;
    led(0, 0, alarm_on ? 40 : 0);
    if (alarm_on) {
        STATE("v1 alarm (solid blue)", 1);
    } else {
        STATE("v1 ok", 0);
    }
#else
    alarm_on = alarm_on ? t >= thr - 1000 : t >= thr;
    if (alarm_on) {
        int on = (now_ms / 250) & 1; /* 2 Hz blink */
        led(0, 0, on ? 40 : 0);
        STATE("v2 alarm (blinking blue)", 2);
    } else if (t >= thr - 2000) {
        led(0, 0, 8);
        STATE("v2 pre-alarm (dim blue)", 1);
    } else {
        led(0, 0, 0);
        STATE("v2 ok", 0);
    }
#endif
}
