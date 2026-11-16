/* I2C, ADC and PWM, with "i2c0 level fan" granted:
 * "i2c <register read back> nack <no device> long <too long> adc <mV> pwm <duty 70000>",
 * after writing 42 to register 1 of the device at 0x48 and setting fan's
 * duty to half. */
#include "fc_app.h"

FC_APP_ABI()

static uint8_t big[FC_I2C_MAX + 1];

int32_t init(void)
{
    int32_t bus = fc_i2c_open(FC_STR("i2c0"));
    int32_t level = fc_adc_open(FC_STR("level"));
    int32_t fan = fc_pwm_open(FC_STR("fan"), 0);
    if (bus < 0 || level < 0 || fan < 0) {
        return 1;
    }
    uint8_t w[2] = {1, 42}, reg = 1, r = 0, x;
    if (fc_i2c_write(bus, 0x48, w, 2) != 0 || fc_i2c_write_read(bus, 0x48, &reg, 1, &r, 1) != 0 ||
        fc_pwm_duty(fan, 32768) != 0) {
        return 2;
    }
    char s[96];
    int32_t n = 0;
#define PUT(str)                         \
    for (const char *p = str; *p; p++) { \
        s[n++] = *p;                     \
    }
    PUT("i2c ");
    n += fc_fmt_int(s + n, r);
    PUT(" nack ");
    n += fc_fmt_int(s + n, fc_i2c_read(bus, 0x10, &x, 1));
    PUT(" long ");
    n += fc_fmt_int(s + n, fc_i2c_write(bus, 0x48, big, sizeof(big)));
    PUT(" adc ");
    n += fc_fmt_int(s + n, fc_adc_read_mv(level));
    PUT(" pwm ");
    n += fc_fmt_int(s + n, fc_pwm_duty(fan, 70000));
    fc_set_state(s, n);
    return 0;
}

void tick(void) {}
