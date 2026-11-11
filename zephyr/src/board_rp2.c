/*
 * Raspberry Pi Pico boards with the console on USB (west build -S
 * cdc-acm-console), for development:
 * - the boot waits a few seconds for a terminal to open the port, so that
 *   the first messages are not lost;
 * - setting the port to 1200 baud restarts the board in its USB bootloader,
 *   ready for a new UF2, as firmware built with the Pico SDK does
 *   (stty -f /dev/cu.usbmodemXXXX 1200).
 */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>

#include <pico/bootrom.h>

#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart)

#define CONSOLE DEVICE_DT_GET(DT_CHOSEN(zephyr_console))

static int wait_for_terminal(void)
{
    for (int i = 0; i < 80; i++) { /* 8 s at most */
        uint32_t dtr = 0, baud = 0;
        /* Also here, before anything else runs: a firmware that fails at
         * every boot can still be replaced. */
        if (uart_line_ctrl_get(CONSOLE, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 && baud == 1200) {
            reset_usb_boot(0, 0);
        }
        if (uart_line_ctrl_get(CONSOLE, UART_LINE_CTRL_DTR, &dtr) == 0 && dtr != 0) {
            k_sleep(K_MSEC(1500)); /* the terminal has opened the port, not yet started to read */
            break;
        }
        k_sleep(K_MSEC(100));
    }
    return 0;
}
SYS_INIT(wait_for_terminal, APPLICATION, 0);

static void bootloader_watch(void *a, void *b, void *c)
{
    for (;;) {
        uint32_t baud = 0;
        if (uart_line_ctrl_get(CONSOLE, UART_LINE_CTRL_BAUD_RATE, &baud) == 0 && baud == 1200) {
            reset_usb_boot(0, 0);
        }
        k_sleep(K_MSEC(500));
    }
}
K_THREAD_DEFINE(fc_bootloader_watch, 1024, bootloader_watch, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

#endif
