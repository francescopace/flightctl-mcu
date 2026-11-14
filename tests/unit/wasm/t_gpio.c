/* Drives relay1 and led0, reads the chip's temperature, and reports
 * button0's edges: "edge <level>". */
#include "fc_app.h"

FC_APP_ABI()

static int32_t relay, button, led;

int32_t init(void)
{
    relay = fc_gpio_open(FC_STR("relay1"), FC_GPIO_OUT);
    button = fc_gpio_open(FC_STR("button0"), FC_GPIO_IN_PULLUP);
    led = fc_led_open(FC_STR("led0"));
    if (relay < 0 || button < 0 || led < 0) {
        return 1;
    }
    int32_t t = 0;
    if (fc_sensor_read(FC_STR("die_temp"), &t) != 0 || fc_gpio_write(relay, 1) != 0 ||
        fc_gpio_irq(button, FC_EDGE_BOTH, 0) != 0 || fc_led_rgb(led, 1, 2, 300) != 0) {
        return 2;
    }
    char s[32];
    int32_t n = 0;
    s[n++] = 't';
    s[n++] = ' ';
    n += fc_fmt_int(s + n, t);
    s[n++] = ' ';
    n += fc_fmt_int(s + n, fc_gpio_read(button));
    fc_set_state(s, n);
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    if (type == FC_EV_GPIO && h == button) {
        char s[16] = "edge ";
        int32_t n = 5;
        n += fc_fmt_int(s + n, value);
        fc_set_state(s, n);
    }
}
