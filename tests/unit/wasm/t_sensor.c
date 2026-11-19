/* Standalone notification probe, also used on the physical lab. */
#include "fc_app.h"

FC_APP_ABI()

static int32_t sub = -1, mode;

int32_t init(void)
{
    mode = fc_config_int(FC_STR("mode"), FC_SENSOR_THRESHOLD);
    int32_t low = fc_config_int(FC_STR("low_mc"), 4000);
    int32_t high = fc_config_int(FC_STR("high_mc"), 5000);
    sub = fc_sensor_subscribe(FC_STR("line_temp"), mode, low, high);
    if (sub < 0) {
        char s[24] = "subscribe ";
        int n = fc_fmt_int(s + 10, sub);
        fc_set_state(s, 10 + n);
    } else {
        FC_STATE("subscribed");
    }
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    if (type == FC_EV_CONFIG && sub >= 0) {
        fc_close(sub);
        sub = -1;
        FC_STATE("closed");
    } else if (type == FC_EV_SENSOR) {
        int32_t temperature;
        if (h != sub || value != mode || fc_sensor_read(FC_STR("line_temp"), &temperature) != 0) {
            FC_STATE("invalid notification");
        } else if (value == FC_SENSOR_READY) {
            FC_STATE("data ready");
        } else {
            FC_STATE("threshold interrupt");
        }
    }
}
