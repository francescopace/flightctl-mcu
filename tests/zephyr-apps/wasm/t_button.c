/* Read the single GPIO named in the app's fleet config. */
#include "fc_app.h"

FC_APP_ABI()
static int32_t button;

int32_t init(void)
{
    char name[32];
    int32_t len = fc_config_get(FC_STR("input"), name, sizeof(name));
    if (len <= 0 || len > (int32_t)sizeof(name)) {
        return 1;
    }
    button = fc_gpio_open(name, len, FC_GPIO_IN_PULLUP);
    return button < 0;
}

void tick(void)
{
    switch (fc_gpio_read(button)) {
    case 0: FC_STATE("released"); break;
    case 1: FC_STATE("pressed"); break;
    default: FC_STATE("read error"); break;
    }
}
