/*
 * port/zephyr/fc_hw_zephyr.c on native_sim's emulated drivers, called as the
 * core calls it: the names from the devicetree (the board's overlay), GPIO with
 * interrupts, ADC, PWM, I2C (an AT24 EEPROM at 0x50), SPI (a loopback
 * device, src/spi_loopback.c), UART (wired back to itself).
 */
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc/adc_emul.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/drivers/pwm/pwm_fake.h>
#include <zephyr/drivers/serial/uart_emul.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "fc_hw.h"

DEFINE_FFF_GLOBALS; /* for the fake PWM */

void fc_hw_zephyr_fill(fc_hw_t *hw);

static fc_hw_t hw;
static const struct device *const gpio0 = DEVICE_DT_GET(DT_NODELABEL(gpio0));

static int id(int kind, const char *name)
{
    return hw.dt_find(kind, name);
}

static void *setup(void)
{
    fc_hw_zephyr_fill(&hw);
    return NULL;
}

ZTEST(fc_hw, test_names)
{
    char *s = hw.board_defaults();
    zassert_not_null(s);
    zassert_not_null(strstr(s, "\"relay1\":{\"dt\":\"relay1\",\"safe\":0}"), "%s", s);
    zassert_not_null(strstr(s, "\"button0\":{\"dt\":\"button0\",\"active_low\":true}"), "%s", s);
    zassert_not_null(strstr(s, "\"led0\":{\"dt\":\"simulated\"}"), "%s", s);
    zassert_not_null(strstr(s, "\"level\":{\"dt\":\"level\"}"), "%s", s);
    zassert_not_null(strstr(s, "\"flash\":{\"dt\":\"flash\"}"), "%s", s);
    zassert_is_null(strstr(s, "console"), "the console's UART is not offered: %s", s);
    free(s);
    zassert_true(id(FC_HW_KIND_GPIO, "relay1") >= 0);
    zassert_equal(id(FC_HW_KIND_ADC, "relay1"), -1, "a GPIO is not an ADC channel");
    zassert_equal(id(FC_HW_KIND_UART, "console"), -1);
    zassert_equal(id(FC_HW_KIND_GPIO, "nope"), -1);
    zassert_equal(id(FC_HW_KIND_LED, "simulated"), 0);
}

static int s_edges;
static void on_edge(void *ctx)
{
    (*(int *)ctx)++;
}

ZTEST(fc_hw, test_gpio)
{
    int r = id(FC_HW_KIND_GPIO, "relay1"), b = id(FC_HW_KIND_GPIO, "button0");
    zassert_ok(hw.gpio_setup(r, FC_HW_GPIO_OUT));
    zassert_ok(hw.gpio_write(r, 1));
    zassert_equal(gpio_emul_output_get(gpio0, 5), 1);
    zassert_ok(hw.gpio_write(r, 0));
    zassert_equal(gpio_emul_output_get(gpio0, 5), 0);
    /* Raw levels: the core makes button0 logical (active low). */
    zassert_ok(hw.gpio_setup(b, FC_HW_GPIO_IN_PULLUP));
    gpio_emul_input_set(gpio0, 6, 0);
    zassert_equal(hw.gpio_read(b), 0);
    gpio_emul_input_set(gpio0, 6, 1);
    zassert_equal(hw.gpio_read(b), 1);
    /* Edges reach the callback, from a work item; none once it is off. */
    s_edges = 0;
    zassert_ok(hw.gpio_irq(b, 3, on_edge, &s_edges));
    gpio_emul_input_set(gpio0, 6, 0);
    k_msleep(20);
    gpio_emul_input_set(gpio0, 6, 1);
    k_msleep(20);
    zassert_equal(s_edges, 2);
    zassert_ok(hw.gpio_irq(b, 1, on_edge, &s_edges)); /* rising only */
    gpio_emul_input_set(gpio0, 6, 0);
    k_msleep(20);
    zassert_equal(s_edges, 2);
    gpio_emul_input_set(gpio0, 6, 1);
    k_msleep(20);
    zassert_equal(s_edges, 3);
    zassert_ok(hw.gpio_irq(b, 0, NULL, NULL));
    gpio_emul_input_set(gpio0, 6, 0);
    k_msleep(20);
    zassert_equal(s_edges, 3);
    hw.gpio_release(r);
    hw.gpio_release(b);
}

ZTEST(fc_hw, test_adc)
{
    int a = hw.adc_open(id(FC_HW_KIND_ADC, "level"));
    zassert_true(a >= 0, "%d", a);
    zassert_ok(adc_emul_const_value_set(DEVICE_DT_GET(DT_NODELABEL(adc0)), 0, 1650));
    int mv = hw.adc_read_mv(a);
    zassert_true(mv >= 1645 && mv <= 1655, "%d mV", mv);
    hw.adc_close(a);
}

ZTEST(fc_hw, test_pwm)
{
    int p = hw.pwm_open(id(FC_HW_KIND_PWM, "fan"), 1000);
    zassert_true(p >= 0, "%d", p);
    /* The fake counts at 1 MHz: 1 kHz is 1000 cycles. */
    zassert_equal(fake_pwm_set_cycles_fake.arg2_val, 1000);
    zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 0, "starts off");
    zassert_ok(hw.pwm_duty(p, 32768));
    zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 500);
    zassert_ok(hw.pwm_duty(p, 65535));
    zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 1000);
    hw.pwm_close(p);
    zassert_equal(fake_pwm_set_cycles_fake.arg3_val, 0, "low when closed");
    zassert_equal(hw.pwm_open(id(FC_HW_KIND_PWM, "fan"), 0), FC_HW_EINVAL);
}

ZTEST(fc_hw, test_i2c)
{
    fc_hw_bus_t bus = {.dt = id(FC_HW_KIND_I2C, "sensors")};
    int b = hw.i2c_open(&bus);
    zassert_true(b >= 0, "%d", b);
    uint8_t w[2] = {0x10, 0xAB}, r = 0;
    int rc = hw.i2c_transfer(b, 0x50, w, 2, NULL, 0, 100);
    zassert_equal(rc, 0, "write: %d", rc);
    zassert_ok(hw.i2c_transfer(b, 0x50, w, 1, &r, 1, 100));
    zassert_equal(r, 0xAB);
    zassert_equal(hw.i2c_transfer(b, 0x60, NULL, 0, &r, 1, 100), FC_HW_EIO, "no device at 0x60");
    hw.i2c_close(b);
}

ZTEST(fc_hw, test_spi)
{
    fc_hw_bus_t fb = {.dt = id(FC_HW_KIND_SPI, "flash")}, ob = {.dt = id(FC_HW_KIND_SPI, "other")};
    int f = hw.spi_open(&fb), o = hw.spi_open(&ob);
    zassert_true(f >= 0 && o >= 0, "%d %d", f, o);
    zassert_equal(gpio_emul_output_get(gpio0, 7), 1, "not selected (active low)");
    uint8_t tx[100], rx[100];
    for (int i = 0; i < 100; i++) {
        tx[i] = (uint8_t)i;
    }
    memset(rx, 0, sizeof(rx));
    zassert_ok(hw.spi_transfer(f, tx, rx, sizeof(tx), 0, 100));
    zassert_mem_equal(rx, tx, sizeof(tx));
    zassert_equal(gpio_emul_output_get(gpio0, 7), 1);
    /* Nothing to send: 0xFF. */
    zassert_ok(hw.spi_transfer(f, NULL, rx, 70, 0, 100));
    zassert_equal(rx[0], 0xFF);
    zassert_equal(rx[69], 0xFF);
    /* A kept chip select holds the bus: the other device waits, then gives up. */
    zassert_ok(hw.spi_transfer(f, tx, rx, 4, 1, 100));
    zassert_equal(gpio_emul_output_get(gpio0, 7), 0, "selected");
    int64_t t0 = k_uptime_get();
    zassert_equal(hw.spi_transfer(o, tx, rx, 4, 0, 100), FC_HW_EBUSY);
    zassert_true(k_uptime_get() - t0 >= 100);
    hw.spi_release(f);
    zassert_equal(gpio_emul_output_get(gpio0, 7), 1);
    zassert_ok(hw.spi_transfer(o, tx, rx, 4, 0, 100));
    /* Closed while kept: let go. */
    zassert_ok(hw.spi_transfer(f, tx, rx, 4, 1, 100));
    hw.spi_close(f);
    zassert_ok(hw.spi_transfer(o, tx, rx, 4, 0, 100));
    hw.spi_close(o);
}

static int s_uart_calls, s_uart_waiting;
static void on_uart(void *ctx, int n)
{
    s_uart_calls++;
    s_uart_waiting = n;
}

ZTEST(fc_hw, test_uart)
{
    fc_hw_bus_t ub = {.dt = id(FC_HW_KIND_UART, "serial")};
    s_uart_calls = 0;
    int u = hw.uart_open(&ub, 9600, FC_HW_UART_8E1, on_uart, NULL);
    zassert_true(u >= 0, "%d", u);
    zassert_equal(hw.uart_write(u, (const uint8_t *)"hi", 2, 100), 2);
    for (int i = 0; i < 50 && s_uart_waiting < 2; i++) {
        k_msleep(2);
    }
    zassert_true(s_uart_calls >= 1);
    zassert_equal(s_uart_waiting, 2);
    uint8_t buf[8];
    zassert_equal(hw.uart_read(u, buf, sizeof(buf), 10), 2);
    zassert_mem_equal(buf, "hi", 2);
    int64_t t0 = k_uptime_get();
    zassert_equal(hw.uart_read(u, buf, sizeof(buf), 30), 0, "nothing waiting");
    zassert_true(k_uptime_get() - t0 >= 30);
    hw.uart_close(u);
    int calls = s_uart_calls;
    /* Closed: the port no longer calls. */
    u = hw.uart_open(&ub, 0 + 9600, FC_HW_UART_8N1, on_uart, NULL);
    hw.uart_close(u);
    k_msleep(20);
    zassert_equal(s_uart_calls, calls);
}

/* The emulated UART provides the FIFO. Draining it moves the character
 * into the modeled shift register; only the test completes its stop bit. */
static const struct device *const rs_uart = DEVICE_DT_GET(DT_NODELABEL(euart1));
static atomic_t shift_busy, fifo_blocked;
static K_SEM_DEFINE(rs_queued, 0, 1);
static K_SEM_DEFINE(rs_idle_seen, 0, 1);
static K_SEM_DEFINE(rs_writer_done, 0, 1);
static struct k_thread rs_writer;
static K_THREAD_STACK_DEFINE(rs_stack, 1536);
static int rs_handle, rs_result, rs_queued_count;
static bool block_after_byte;

int fc_test_uart_tx_state(const struct device *dev, bool *room, bool *idle)
{
    if (dev != rs_uart) {
        return FC_HW_ENOTSUP;
    }
    *room = !atomic_get(&fifo_blocked);
    *idle = !atomic_get(&shift_busy);
    if (*idle) {
        k_sem_give(&rs_idle_seen);
    }
    return 0;
}

static void rs_byte_queued(const struct device *dev, size_t size, void *ctx)
{
    uint8_t byte;
    zassert_equal(gpio_emul_output_get(gpio0, 9), 0, "DE active before loading FIFO");
    zassert_equal(uart_emul_get_tx_data(dev, &byte, 1), 1);
    zassert_equal(byte, 'A');
    rs_queued_count++;
    atomic_set(&shift_busy, 1);
    if (block_after_byte) {
        atomic_set(&fifo_blocked, 1);
    }
    k_sem_give(&rs_queued);
}

static void rs_write(void *a, void *b, void *c)
{
    rs_result = hw.uart_write(rs_handle, (const uint8_t *)"A", 1, 1000);
    k_sem_give(&rs_writer_done);
}

ZTEST(fc_hw, test_rs485_waits_for_the_shift_register)
{
    atomic_clear(&shift_busy);
    atomic_clear(&fifo_blocked);
    rs_queued_count = 0;
    block_after_byte = false;
    k_sem_reset(&rs_queued);
    k_sem_reset(&rs_writer_done);
    fc_hw_bus_t bus = {.dt = id(FC_HW_KIND_UART, "rs485")};
    fc_hw_bus_t alias = {.dt = id(FC_HW_KIND_UART, "rs485-alias")};
    fc_hw_bus_t shared_de = {.dt = id(FC_HW_KIND_UART, "rs485-shared-de")};
    fc_hw_bus_t unsupported = {.dt = id(FC_HW_KIND_UART, "rs485-unsupported")};
    zassert_equal(hw.uart_open(&unsupported, 9600, FC_HW_UART_8N1, on_uart, NULL), FC_HW_ENOTSUP);
    rs_handle = hw.uart_open(&bus, 9600, FC_HW_UART_8N2, on_uart, NULL);
    zassert_true(rs_handle >= 0);
    zassert_equal(gpio_emul_output_get(gpio0, 9), 1, "inactive at open");
    zassert_equal(hw.uart_open(&alias, 9600, FC_HW_UART_8N1, on_uart, NULL), FC_HW_EBUSY);
    zassert_equal(hw.uart_open(&shared_de, 9600, FC_HW_UART_8N1, on_uart, NULL), FC_HW_EBUSY);
    fc_hw_bus_t serial = {.dt = id(FC_HW_KIND_UART, "serial")};
    int ordinary = hw.uart_open(&serial, 9600, FC_HW_UART_8N1, on_uart, NULL);
    zassert_true(ordinary >= 0);
    uart_emul_callback_tx_data_ready_set(rs_uart, rs_byte_queued, NULL);
    k_thread_create(&rs_writer, rs_stack, K_THREAD_STACK_SIZEOF(rs_stack), rs_write,
                    NULL, NULL, NULL, K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
    zassert_ok(k_sem_take(&rs_queued, K_SECONDS(2)));
    zassert_equal(gpio_emul_output_get(gpio0, 9), 0, "empty FIFO, stop bit still on the wire");
    zassert_equal(hw.uart_write(rs_handle, (const uint8_t *)"B", 1, 0), FC_HW_EBUSY);
    zassert_equal(hw.uart_write(ordinary, (const uint8_t *)"X", 1, 20), 1, "independent port");
    atomic_clear(&shift_busy);
    zassert_ok(k_sem_take(&rs_writer_done, K_SECONDS(2)));
    zassert_ok(k_thread_join(&rs_writer, K_SECONDS(2)));
    hw.uart_close(ordinary);
    zassert_equal(rs_result, 1);
    zassert_equal(gpio_emul_output_get(gpio0, 9), 1, "DE released on completion");
    /* The second byte cannot enter the FIFO before the caller's deadline.
     * Close during the last stop bit must preserve DE and the UART claim. */
    block_after_byte = true;
    zassert_equal(hw.uart_write(rs_handle, (const uint8_t *)"AB", 2, 20), 1);
    zassert_equal(rs_queued_count, 2);
    hw.uart_close(rs_handle);
    zassert_equal(gpio_emul_output_get(gpio0, 9), 0);
    zassert_equal(hw.uart_open(&alias, 9600, FC_HW_UART_8N1, on_uart, NULL), FC_HW_EBUSY);
    k_sem_reset(&rs_idle_seen);
    atomic_clear(&shift_busy);
    zassert_ok(k_sem_take(&rs_idle_seen, K_SECONDS(2)));
    int next = hw.uart_open(&alias, 9600, FC_HW_UART_8N1, on_uart, NULL);
    zassert_true(next >= 0);
    zassert_equal(gpio_emul_output_get(gpio0, 9), 1);
    hw.uart_close(next);
    uart_emul_callback_tx_data_ready_set(rs_uart, NULL, NULL);
}

ZTEST_SUITE(fc_hw, NULL, setup, NULL, NULL, NULL);
