/* What it may open: "<relay1> <nothere> <i2c0> <metric> <relay1 again>",
 * with "relay1 i2c0" granted. */
#include "fc_app.h"

FC_APP_ABI()

int32_t init(void)
{
    char s[48];
    int32_t n = 0;
    n += fc_fmt_int(s + n, fc_gpio_open(FC_STR("relay1"), FC_GPIO_OUT));
    s[n++] = ' ';
    n += fc_fmt_int(s + n, fc_gpio_open(FC_STR("nothere"), FC_GPIO_OUT));
    s[n++] = ' ';
    n += fc_fmt_int(s + n, fc_i2c_open(FC_STR("i2c0")));
    s[n++] = ' ';
    n += fc_fmt_int(s + n, fc_metric(FC_STR("x"), FC_METRIC_GAUGE, 1.0));
    s[n++] = ' ';
    n += fc_fmt_int(s + n, fc_gpio_open(FC_STR("relay1"), FC_GPIO_OUT));
    fc_set_state(s, n);
    return 0;
}

void tick(void) {}
