/* Read-only supply monitor. The fleet chooses the threshold for its pack. */
#include "fc_app.h"
#include <stdio.h>

FC_APP_ABI()

static int32_t timer = -1;
static int32_t low_mv;

static void configure(void)
{
    low_mv = fc_config_int(FC_STR("low_mv"), 0);
    int ms = fc_config_int(FC_STR("sample_ms"), 30000);
    if (timer >= 0) {
        fc_close(timer);
    }
    timer = fc_timer_start(ms < 1000 ? 1000 : ms, 1);
}

static void sample(void)
{
    int32_t mv, pct, charging;
    int voltage_rc = fc_sensor_read(FC_STR("battery_mv"), &mv);
    if (fc_sensor_read(FC_STR("battery_pct"), &pct) == 0) {
        fc_metric(FC_STR("charge_pct"), FC_METRIC_GAUGE, pct);
    }
    if (fc_sensor_read(FC_STR("battery_charging"), &charging) == 0) {
        fc_metric(FC_STR("charging"), FC_METRIC_GAUGE, charging);
    }
    if (voltage_rc < 0) {
        if (voltage_rc == FC_ENOENT || voltage_rc == FC_ENOTSUP) {
            FC_STATE("battery unavailable");
        } else if (voltage_rc == FC_EPERM) {
            FC_STATE("battery access denied");
        } else {
            FC_STATE("battery read failed");
        }
        return;
    }
    fc_metric(FC_STR("voltage_mv"), FC_METRIC_GAUGE, mv);
    if (low_mv > 0) {
        fc_metric(FC_STR("low_battery"), FC_METRIC_GAUGE, mv < low_mv);
    }
    char state[40];
    int len = snprintf(state, sizeof(state), "%d mV%s", (int)mv, mv < low_mv ? " (low)" : "");
    fc_set_state(state, len);
}

int32_t init(void)
{
    configure();
    sample();
    return timer < 0 ? timer : 0;
}

void on_event(int32_t type, int32_t handle, int32_t value)
{
    (void)value;
    if (type == FC_EV_CONFIG) {
        configure();
        sample();
    } else if (type == FC_EV_TIMER && handle == timer) {
        sample();
    }
}
