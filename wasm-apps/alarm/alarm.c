/*
 * "alarm" WASM app: the line temperature alarm (docs/APPS.md).
 *
 * It reads the chip's temperature and drives the LED "led0" against the
 * fleet's threshold, from its config file /etc/fc-app/alarm.json:
 *
 *   {"threshold_c": 40.5, "hysteresis_c": 1, "sample_ms": 2000}
 *
 * One source, two behaviours, chosen by the image's tag: wasm-apps/build.sh
 * passes its major version as APP_VERSION. They show an app being updated
 * without a reboot; they are not versions of the interface, which is
 * FC_APP_ABI_VERSION.
 *   alarm:v1  LED solid blue at or above the threshold.
 *   alarm:v2  hysteresis, dim solid blue pre-alarm 2 C below, blinking blue alarm.
 *
 * Grants: "led0" in FC_ALLOW. Without it the alarm still runs and reports its
 * state, with no LED. It has no tick(): a timer samples, another blinks, and
 * the app sleeps in between. Blue only, so boards with a single-colour LED
 * (most are blue) and RGB boards look the same: solid or blinking is what
 * matters.
 */
#include "fc_app.h"

FC_APP_ABI()

#ifndef APP_VERSION
#define APP_VERSION 1
#endif

#define BLINK_MS   250 /* 2 Hz */
#define PREALARM_C 2

enum { OK, PREALARM, ALARM };

static int32_t led = -1, sampler = -1, blinker = -1, blink_on;
static int32_t sample_ms, threshold_mc, hysteresis_mc;
static int32_t level = -1;

static void set_led(int32_t blue)
{
    if (led >= 0) {
        fc_led_rgb(led, 0, 0, blue);
    }
}

/* Reads the config and (re)starts the sampling timer when its period changes. */
static void configure(void)
{
    int32_t ms = fc_config_int(FC_STR("sample_ms"), 2000);
    ms = ms < 100 ? 100 : ms > 60000 ? 60000 : ms;
    threshold_mc = fc_config_milli(FC_STR("threshold_c"), 45000);
    hysteresis_mc = fc_config_milli(FC_STR("hysteresis_c"), 1000);
    if (ms != sample_ms || sampler < 0) {
        if (sampler >= 0) {
            fc_close(sampler);
        }
        sample_ms = ms;
        sampler = fc_timer_start(ms, 1);
    }
}

static void show(int32_t next)
{
    if (next == level) {
        return;
    }
    level = next;
    if (blinker >= 0) {
        fc_close(blinker);
        blinker = -1;
    }
#if APP_VERSION == 1
    set_led(next == ALARM ? 40 : 0);
    if (next == ALARM) {
        FC_STATE("v1 alarm (solid blue)");
    } else {
        FC_STATE("v1 ok");
    }
#else
    if (next == ALARM) {
        blink_on = 1;
        set_led(40);
        blinker = fc_timer_start(BLINK_MS, 1);
        FC_STATE("v2 alarm (blinking blue)");
    } else if (next == PREALARM) {
        set_led(8);
        FC_STATE("v2 pre-alarm (dim blue)");
    } else {
        set_led(0);
        FC_STATE("v2 ok");
    }
#endif
}

static void sample(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) < 0) {
        FC_LOG(FC_LOG_WARN, "cannot read die_temp");
        return;
    }
#if APP_VERSION == 1
    show(t >= threshold_mc ? ALARM : OK);
#else
    int32_t on = level == ALARM ? t >= threshold_mc - hysteresis_mc : t >= threshold_mc;
    show(on ? ALARM : t >= threshold_mc - PREALARM_C * 1000 ? PREALARM : OK);
#endif
}

int32_t init(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) == FC_ENOTSUP) {
        FC_STATE("no temperature sensor on this chip");
        return 0; /* no timer: the app stays idle */
    }
    led = fc_led_open(FC_STR("led0"));
    if (led < 0) {
        FC_LOG(FC_LOG_INFO, "no led0 granted: state only");
    }
    configure();
    sample();
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    (void)value;
    if (type == FC_EV_TIMER && h == sampler) {
        sample();
    } else if (type == FC_EV_TIMER && h == blinker) {
        blink_on = !blink_on;
        set_led(blink_on ? 40 : 0);
    } else if (type == FC_EV_CONFIG) {
        configure();
        sample();
    }
}
