/*
 * hwcheck: opens the buses and channels of the board map and says what each
 * answers, to check a board's map and its drivers with nothing wired, or
 * with what is. Grant it (FC_ALLOW) the names it looks for: i2c0, spi0,
 * uart0, adc0, pwm0; a name not granted or not in the map is reported as
 * such (FC_EPERM, FC_ENOENT).
 *
 * Its state, every 2 s:
 *   "i2c <devices that acknowledged, from 0x08 to 0x77, or an error>
 *    spi <transfer> uart <bytes written>/<bytes read> adc <mV> pwm <duty set>"
 * A UART with TX wired to RX reads back what it wrote; a PWM pin wired to
 * the ADC's (through an RC filter) reads about half the supply.
 */
#include "fc_app.h"

FC_APP_ABI()

static int32_t i2c, spi, uart, adc, pwm;
static char s[128];
static int32_t n;

static void put(const char *str)
{
    for (; *str; str++) {
        s[n++] = *str;
    }
}

static void num(int32_t v)
{
    n += fc_fmt_int(s + n, v);
}

int32_t init(void)
{
    i2c = fc_i2c_open(FC_STR("i2c0"));
    spi = fc_spi_open(FC_STR("spi0"));
    uart = fc_uart_open(FC_STR("uart0"), 0, FC_UART_8N1);
    adc = fc_adc_open(FC_STR("adc0"));
    pwm = fc_pwm_open(FC_STR("pwm0"), 1000);
    return 0;
}

void tick(void)
{
    n = 0;
    put("i2c ");
    if (i2c < 0) {
        num(i2c);
    } else {
        int32_t found = 0, err = 0;
        uint8_t b;
        for (int32_t addr = 0x08; addr <= 0x77; addr++) {
            int32_t rc = fc_i2c_read(i2c, addr, &b, 1);
            if (rc == 0) {
                found++;
            } else if (rc != FC_EIO) {
                err = rc;
            }
        }
        num(found);
        if (err) {
            put("!");
            num(err);
        }
    }
    put(" spi ");
    if (spi < 0) {
        num(spi);
    } else {
        uint8_t tx[4] = {0x9f, 0, 0, 0}, rx[4] = {0, 0, 0, 0}; /* a flash's JEDEC ID */
        num(fc_spi_transfer(spi, tx, rx, 4, 0));
        put(":");
        num(rx[1]);
    }
    put(" uart ");
    if (uart < 0) {
        num(uart);
    } else {
        num(fc_uart_write(uart, "hwcheck", 7));
        put("/");
        uint8_t buf[16];
        num(fc_uart_read(uart, buf, sizeof(buf), 50));
    }
    put(" adc ");
    num(adc < 0 ? adc : fc_adc_read_mv(adc));
    put(" pwm ");
    num(pwm < 0 ? pwm : fc_pwm_duty(pwm, 32768));
    fc_set_state(s, n);
}
