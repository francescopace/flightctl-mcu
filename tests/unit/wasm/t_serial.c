/* SPI and UART, with "flash rs485" granted. init:
 * "spi <two bytes echoed> <read alone> long <too long> bad <bad flags>", after
 * a transfer that keeps the chip select; then, on the data it wrote coming
 * back: "uart <bytes waiting> <text read>". */
#include "fc_app.h"

FC_APP_ABI()

static int32_t uart;

int32_t init(void)
{
    int32_t flash = fc_spi_open(FC_STR("flash"));
    uart = fc_uart_open(FC_STR("rs485"), 0, FC_UART_8E1);
    if (flash < 0 || uart < 0) {
        return 1;
    }
    uint8_t tx[2] = {'A', 'B'}, rx[2] = {0, 0}, one = 0;
    if (fc_spi_transfer(flash, tx, rx, 2, FC_SPI_KEEP_CS) != 0 || fc_spi_transfer(flash, 0, &one, 1, 0) != 0 ||
        fc_spi_transfer(flash, tx, 0, 1, FC_SPI_KEEP_CS) != 0) {
        return 2;
    }
    char s[64];
    int32_t n = 0;
#define PUT(str)                         \
    for (const char *p = str; *p; p++) { \
        s[n++] = *p;                     \
    }
    PUT("spi ");
    n += fc_fmt_int(s + n, rx[0]);
    PUT(" ");
    n += fc_fmt_int(s + n, rx[1]);
    PUT(" ");
    n += fc_fmt_int(s + n, one);
    PUT(" long ");
    n += fc_fmt_int(s + n, fc_spi_transfer(flash, tx, rx, FC_SPI_MAX + 1, 0));
    PUT(" bad ");
    n += fc_fmt_int(s + n, fc_spi_transfer(flash, tx, rx, 1, 2));
    fc_set_state(s, n);
    if (fc_uart_write(uart, "hi", 2) != 2) {
        return 3;
    }
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    if (type != FC_EV_UART || h != uart) {
        return;
    }
    char s[32], buf[8];
    int32_t n = 0;
    s[n++] = 'u';
    s[n++] = 'a';
    s[n++] = 'r';
    s[n++] = 't';
    s[n++] = ' ';
    n += fc_fmt_int(s + n, value);
    s[n++] = ' ';
    int32_t got = fc_uart_read(uart, buf, sizeof(buf), 10);
    for (int32_t i = 0; i < got; i++) {
        s[n++] = buf[i];
        __asm__ volatile("" ::: "memory"); /* not a memcpy: there is no libc */
    }
    fc_set_state(s, n);
}
