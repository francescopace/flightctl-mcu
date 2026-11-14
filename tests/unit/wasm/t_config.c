/* Its state is the "greeting" of its config file, read again on a new file. */
#include "fc_app.h"

FC_APP_ABI()

static void show(void)
{
    char b[48];
    int32_t n = fc_config_get(FC_STR("greeting"), b, sizeof(b));
    if (n < 0) {
        FC_STATE("no greeting");
    } else {
        fc_set_state(b, n);
    }
}

int32_t init(void)
{
    show();
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    if (type == FC_EV_CONFIG) {
        show();
    }
}
