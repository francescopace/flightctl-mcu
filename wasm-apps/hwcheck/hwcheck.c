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
#include <stdio.h>

#include "fc_app.h"

FC_APP_ABI()

static int32_t i2c, spi, uart, adc, pwm;

int32_t init(void)
{
    i2c = fc_i2c_open(FC_STR("i2c0"));
    spi = fc_spi_open(FC_STR("spi0"));
    uart = fc_uart_open(FC_STR("uart0"), 0, FC_UART_8N1);
    adc = fc_adc_open(FC_STR("adc0"));
    pwm = fc_pwm_open(FC_STR("pwm0"), 1000);
    return 0;
}

/* The devices that acknowledged, or the error that is not a missing device. */
static int32_t scan_i2c(int32_t *err)
{
    int32_t found = 0;
    uint8_t b;
    *err = 0;
    for (int32_t addr = 0x08; addr <= 0x77; addr++) {
        int32_t rc = fc_i2c_read(i2c, addr, &b, 1);
        if (rc == 0) {
            found++;
        } else if (rc != FC_EIO) {
            *err = rc;
        }
    }
    return found;
}

void tick(void)
{
    char s[128];
    int32_t n = 0, err = 0;
    /* A handle below 0 is the error that opening it gave. */
    int32_t found = i2c < 0 ? i2c : scan_i2c(&err);
    n += snprintf(s + n, sizeof(s) - n, err ? "i2c %d!%d" : "i2c %d", found, err);
    if (spi < 0) {
        n += snprintf(s + n, sizeof(s) - n, " spi %d", spi);
    } else {
        uint8_t tx[4] = {0x9f, 0, 0, 0}, rx[4] = {0, 0, 0, 0}; /* a flash's JEDEC ID */
        int32_t rc = fc_spi_transfer(spi, tx, rx, 4, 0);
        n += snprintf(s + n, sizeof(s) - n, " spi %d:%d", rc, rx[1]);
    }
    if (uart < 0) {
        n += snprintf(s + n, sizeof(s) - n, " uart %d", uart);
    } else {
        uint8_t buf[16];
        int32_t w = fc_uart_write(uart, "hwcheck", 7);
        n += snprintf(s + n, sizeof(s) - n, " uart %d/%d", w, fc_uart_read(uart, buf, sizeof(buf), 50));
    }
    int32_t mv = adc < 0 ? adc : fc_adc_read_mv(adc);
    n += snprintf(s + n, sizeof(s) - n, " adc %d pwm %d", mv, pwm < 0 ? pwm : fc_pwm_duty(pwm, 32768));
    fc_set_state(s, n);
}
