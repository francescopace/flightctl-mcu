/*
 * Host "fake device": runs the same agent core as the firmware.
 *
 *   fc-host-agent --server agent-api.192.0.2.10.nip.io:8443 \
 *       --ca .secrets/ca.crt --enroll-cert .secrets/enroll.crt \
 *       --enroll-key .secrets/enroll.key [--signing-key .secrets/signing.pub] \
 *       [--data-dir DIR] [--label k=v]... [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fc_agent.h"

void fc_port_host_init(const char *data_dir, int debug);

/* Simulated hardware for WASM apps: a board with an LED (led0) printed when
 * it changes, a button that is never pressed (button0), free GPIOs 2..15 for
 * the fleet's board.json, outputs printed when they change, and a chip
 * temperature that swings 30..46 C over two minutes. An I2C bus (i2c0) has
 * one device, at 0x48, whose register 0 holds that temperature in
 * hundredths of a degree (big-endian) and registers 2 and 3 whatever is
 * written there; any pin's ADC reads 1650 mV; a PWM's duty is printed when
 * it changes. An SPI device's MISO is wired to its MOSI, and a UART's RX to
 * its TX: what is sent comes back. */
static int32_t host_temp_mc(void)
{
    long t = time(NULL) % 120;
    return 30000 + (int32_t)(t < 60 ? t : 120 - t) * 266;
}

static void host_led(int32_t r, int32_t g, int32_t b)
{
    static int32_t last = -1;
    int32_t v = (r << 16) | (g << 8) | b;
    if (v != last) {
        fprintf(stderr, "LED r=%d g=%d b=%d\n", r, g, b);
        last = v;
    }
}

#define HOST_PINS 16
static int s_level[HOST_PINS];

static char *host_board(void)
{
    return strdup("{\"gpio\": {\"button0\": {\"pin\": 0, \"active_low\": true}},"
                  " \"led\": {\"led0\": {\"pin\": 1}}, \"i2c\": {\"i2c0\": {\"sda\": 14, \"scl\": 15}}}");
}

static int host_pin_reserved(int pin)
{
    return pin < 0 || pin >= HOST_PINS;
}

static int host_gpio_setup(int pin, int mode)
{
    if (pin == 0) {
        s_level[0] = 1; /* the button, released */
    }
    return 0;
}

static int host_gpio_read(int pin)
{
    return s_level[pin];
}

static int host_gpio_write(int pin, int level)
{
    if (s_level[pin] != (level != 0)) {
        fprintf(stderr, "GPIO %d = %d\n", pin, level != 0);
    }
    s_level[pin] = level != 0;
    return 0;
}

static int host_gpio_irq(int pin, int edge, void (*cb)(void *), void *ctx)
{
    return 0; /* nothing presses the button */
}

static void host_gpio_release(int pin) {}

static int host_led_open(const fc_hw_led_t *led)
{
    return 0;
}

static int host_led_rgb(const fc_hw_led_t *led, uint8_t r, uint8_t g, uint8_t b)
{
    host_led(r, g, b);
    return 0;
}

static void host_led_close(const fc_hw_led_t *led)
{
    host_led(0, 0, 0);
}

static int host_sensor(const char *name, int32_t *value)
{
    if (strcmp(name, "die_temp") != 0) {
        return FC_HW_ENOENT;
    }
    *value = host_temp_mc();
    return 0;
}

static uint8_t s_reg[4];
static int s_reg_at;

static int host_i2c_open(const fc_hw_bus_t *bus)
{
    return 0;
}

static int host_i2c_transfer(int bus, int addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms)
{
    if (addr != 0x48) {
        return FC_HW_EIO; /* no device answers */
    }
    int32_t t = host_temp_mc() / 10;
    s_reg[0] = (uint8_t)(t >> 8);
    s_reg[1] = (uint8_t)t;
    for (size_t i = 0; i < wlen; i++) {
        if (i == 0) {
            s_reg_at = w[0] & 3;
        } else {
            if (s_reg_at >= 2) {
                s_reg[s_reg_at] = w[i];
            }
            s_reg_at = (s_reg_at + 1) & 3;
        }
    }
    for (size_t i = 0; i < rlen; i++) {
        r[i] = s_reg[s_reg_at];
        s_reg_at = (s_reg_at + 1) & 3;
    }
    return 0;
}

static void host_i2c_close(int bus) {}

static int host_adc_open(int pin)
{
    return pin;
}

static int host_adc_read_mv(int ch)
{
    return 1650;
}

static void host_adc_close(int ch) {}

static uint32_t s_duty[HOST_PINS];

static int host_pwm_open(int pin, int hz)
{
    fprintf(stderr, "PWM %d at %d Hz\n", pin, hz);
    s_duty[pin] = 0;
    return pin;
}

static int host_pwm_duty(int pin, uint32_t duty)
{
    if (s_duty[pin] != duty) {
        fprintf(stderr, "PWM %d duty %u/65535\n", pin, (unsigned)duty);
    }
    s_duty[pin] = duty;
    return 0;
}

static void host_pwm_close(int pin)
{
    host_pwm_duty(pin, 0);
}

static int host_spi_open(const fc_hw_bus_t *dev)
{
    return 0;
}

static int host_spi_transfer(int dev, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs, int timeout_ms)
{
    for (size_t i = 0; rx != NULL && i < len; i++) {
        rx[i] = tx != NULL ? tx[i] : 0xFF;
    }
    return 0;
}

static void host_spi_release(int dev) {}
static void host_spi_close(int dev) {}

static uint8_t s_uart_buf[256];
static size_t s_uart_len;
static void (*s_uart_cb)(void *, int);
static void *s_uart_ctx;

static int host_uart_open(const fc_hw_bus_t *u, int baud, int format, void (*cb)(void *, int), void *ctx)
{
    if (s_uart_cb != NULL) {
        return FC_HW_EBUSY; /* one UART */
    }
    s_uart_len = 0;
    s_uart_cb = cb;
    s_uart_ctx = ctx;
    return 0;
}

static int host_uart_write(int h, const uint8_t *d, size_t len, int timeout_ms)
{
    size_t n = len < sizeof(s_uart_buf) - s_uart_len ? len : sizeof(s_uart_buf) - s_uart_len;
    memcpy(s_uart_buf + s_uart_len, d, n);
    s_uart_len += n;
    if (n > 0 && s_uart_cb != NULL) {
        s_uart_cb(s_uart_ctx, (int)s_uart_len);
    }
    return (int)n;
}

static int host_uart_read(int h, uint8_t *b, size_t cap, int timeout_ms)
{
    size_t n = cap < s_uart_len ? cap : s_uart_len;
    memcpy(b, s_uart_buf, n);
    memmove(s_uart_buf, s_uart_buf + n, s_uart_len - n);
    s_uart_len -= n;
    return (int)n;
}

static void host_uart_close(int h)
{
    s_uart_cb = NULL;
}

static const fc_hw_t s_hw = {
    .board = "host",
    .firmware = FC_AGENT_VERSION,
    .board_defaults = host_board,
    .pin_reserved = host_pin_reserved,
    .gpio_setup = host_gpio_setup,
    .gpio_read = host_gpio_read,
    .gpio_write = host_gpio_write,
    .gpio_irq = host_gpio_irq,
    .gpio_release = host_gpio_release,
    .led_open = host_led_open,
    .led_rgb = host_led_rgb,
    .led_close = host_led_close,
    .sensor_read = host_sensor,
    .i2c_open = host_i2c_open,
    .i2c_transfer = host_i2c_transfer,
    .i2c_close = host_i2c_close,
    .adc_open = host_adc_open,
    .adc_read_mv = host_adc_read_mv,
    .adc_close = host_adc_close,
    .pwm_open = host_pwm_open,
    .pwm_duty = host_pwm_duty,
    .pwm_close = host_pwm_close,
    .spi_open = host_spi_open,
    .spi_transfer = host_spi_transfer,
    .spi_release = host_spi_release,
    .spi_close = host_spi_close,
    .uart_open = host_uart_open,
    .uart_write = host_uart_write,
    .uart_read = host_uart_read,
    .uart_close = host_uart_close,
};

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        exit(2);
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *server = NULL, *ca = NULL, *cert = NULL, *key = NULL, *signing = NULL;
    const char *data_dir = "build/host-data";
    const char *labels[33] = {0};
    int nlabels = 0, debug = 0;

    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--server") == 0 && v) {
            server = v; i++;
        } else if (strcmp(argv[i], "--ca") == 0 && v) {
            ca = v; i++;
        } else if (strcmp(argv[i], "--enroll-cert") == 0 && v) {
            cert = v; i++;
        } else if (strcmp(argv[i], "--enroll-key") == 0 && v) {
            key = v; i++;
        } else if (strcmp(argv[i], "--signing-key") == 0 && v) {
            signing = v; i++;
        } else if (strcmp(argv[i], "--data-dir") == 0 && v) {
            data_dir = v; i++;
        } else if (strcmp(argv[i], "--label") == 0 && v && nlabels < 30) {
            char *kv = strdup(v), *eq = strchr(kv, '=');
            if (eq == NULL) {
                fprintf(stderr, "--label expects key=value\n");
                return 2;
            }
            *eq = '\0';
            labels[nlabels++] = kv;
            labels[nlabels++] = eq + 1;
            i++;
        } else if (strcmp(argv[i], "-v") == 0) {
            debug = 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!server || !ca || !cert || !key) {
        fprintf(stderr, "usage: %s --server host:port --ca F --enroll-cert F --enroll-key F "
                        "[--signing-key F] [--data-dir D] [--label k=v]... [-v]\n", argv[0]);
        return 2;
    }

    char *host = strdup(server);
    char *colon = strrchr(host, ':');
    int port = 443;
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    fc_port_host_init(data_dir, debug);
    fc_agent_config_t cfg = {
        .server_host = host,
        .server_port = port,
        .ca_pem = read_file(ca),
        .enrollment_cert_pem = read_file(cert),
        .enrollment_key_pem = read_file(key),
        .signing_pub_pem = signing ? read_file(signing) : NULL,
        .labels = labels,
        .agent_version = FC_AGENT_VERSION,
        .status_interval_s = 60,
        .hw = &s_hw,
    };
    return fc_agent_run(&cfg) == 0 ? 0 : 1;
}
