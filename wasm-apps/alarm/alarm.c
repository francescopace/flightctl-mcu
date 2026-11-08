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
#include "fc_app.h"

FC_APP_ABI()


#ifndef APP_VERSION
#define APP_VERSION 1
#endif
#define ALARM_VERSION APP_VERSION

static int alarm_on;
static int last_state = -1;

static void state(const char *s, int32_t len, int code)
{
    if (code != last_state) {
        fc_set_state(s, len);
        last_state = code;
    }
}
#define STATE(lit, code) state(lit, sizeof(lit) - 1, code)

FC_EXPORT("tick") void tick(int32_t now_ms)
{
    int32_t t = fc_temp_mc(), thr = fc_threshold_mc();
#if ALARM_VERSION == 1
    alarm_on = t >= thr;
    fc_led(0, 0, alarm_on ? 40 : 0);
    if (alarm_on) {
        STATE("v1 alarm (solid blue)", 1);
    } else {
        STATE("v1 ok", 0);
    }
#else
    alarm_on = alarm_on ? t >= thr - 1000 : t >= thr;
    if (alarm_on) {
        int on = (now_ms / 250) & 1; /* 2 Hz blink */
        fc_led(0, 0, on ? 40 : 0);
        STATE("v2 alarm (blinking blue)", 2);
    } else if (t >= thr - 2000) {
        fc_led(0, 0, 8);
        STATE("v2 pre-alarm (dim blue)", 1);
    } else {
        fc_led(0, 0, 0);
        STATE("v2 ok", 0);
    }
#endif
}
