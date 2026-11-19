/* The fleet grants which peer may supply readings, in millidegrees Celsius. */
#include "fc_app.h"

FC_APP_ABI()

int32_t init(void)
{
    FC_STATE("waiting for sample");
    return 0;
}

void on_event(int32_t type, int32_t handle, int32_t value)
{
    (void)handle;
    if (type == FC_EV_LOCAL) {
        if (value >= fc_config_milli(FC_STR("threshold_c"), 45000)) {
            FC_STATE("alarm");
        } else {
            FC_STATE("ok");
        }
    }
}
