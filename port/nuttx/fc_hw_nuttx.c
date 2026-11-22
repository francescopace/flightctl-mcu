#include <nuttx/config.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <nuttx/timers/pwm.h>
#include <nuttx/wqueue.h>
#include "espressif/esp_gpio.h"
#include "soc/soc_caps_eval.h"
#include "driver/rmt_tx.h"
#include "hardware/esp32s3_gpio_sigmap.h"
#include "fc_agent.h"
#include "fc_hw_nuttx.h"

#define PIN_COUNT 49

static int reserved(int pin)
{
    if (pin < 0 || pin >= PIN_COUNT || (pin >= 19 && pin <= 32 && pin != 21) || pin == 43 || pin == 44) {
        return 1;
    }
#ifdef CONFIG_ESP32S3_SPIRAM_MODE_OCT
    if (pin >= 33 && pin <= 37) return 1;
#endif
    return 0;
}

static int setup(int pin, int mode)
{
    static const gpio_pinattr_t attrs[] = {INPUT, INPUT_PULLUP, INPUT_PULLDOWN, INPUT | OUTPUT, INPUT | OUTPUT_OPEN_DRAIN};
    if (reserved(pin) || mode < 0 || mode > FC_HW_GPIO_OUT_OD) return FC_HW_EINVAL;
    esp_gpio_matrix_out(pin, MATRIX_DETACH_OUT_SIG, false, false);
    return esp_configgpio(pin, attrs[mode]) == 0 ? 0 : FC_HW_EIO;
}

static int level(int pin) { return esp_gpioread(pin); }
static int drive(int pin, int value) { esp_gpiowrite(pin, value != 0); return 0; }

struct pin_event {
    struct work_s work;
    void (*cb)(void *);
    void *ctx;
    unsigned edges;
    gpio_pinattr_t attr;
    bool installed;
};
static struct pin_event pins[PIN_COUNT];

static void dispatch(void *arg)
{
    struct pin_event *p = arg;
    unsigned n = __atomic_exchange_n(&p->edges, 0, __ATOMIC_RELAXED);
    while (n-- && p->cb) p->cb(p->ctx);
}

static int interrupt(int irq, void *frame, void *arg)
{
    struct pin_event *p = arg;
    if (__atomic_fetch_add(&p->edges, 1, __ATOMIC_RELAXED) == 0) {
        work_queue(LPWORK, &p->work, dispatch, p, 0);
    }
    return 0;
}

static int gpio_setup(int pin, int mode)
{
    int rc = setup(pin, mode);
    if (!rc) pins[pin].attr = mode == FC_HW_GPIO_IN_PULLUP ? INPUT_PULLUP :
                              mode == FC_HW_GPIO_IN_PULLDOWN ? INPUT_PULLDOWN : INPUT;
    return rc;
}

static int gpio_irq(int pin, int edge, void (*cb)(void *), void *ctx)
{
    struct pin_event *p = &pins[pin];
    if (p->installed) {
        esp_gpioirqdisable(pin);
        work_cancel_sync(LPWORK, &p->work);
    }
    __atomic_store_n(&p->edges, 0, __ATOMIC_RELAXED);
    p->cb = cb;
    p->ctx = ctx;
    if (!edge) return 0;
    static const gpio_pinattr_t trigger[] = {DISABLED, RISING, FALLING, CHANGE};
    if (edge < 1 || edge > 3 || !cb) return FC_HW_EINVAL;
    if (esp_configgpio(pin, p->attr | trigger[edge]) < 0) return FC_HW_EIO;
    if (!p->installed) {
        if (esp_gpio_irq(pin, interrupt, p) < 0) return FC_HW_EIO;
        p->installed = true;
    }
    return esp_gpioirqenable(pin) == 0 ? 0 : FC_HW_EIO;
}

static void release(int pin)
{
    gpio_irq(pin, 0, NULL, NULL);
    setup(pin, FC_HW_GPIO_IN);
}

static struct pixel {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
} pixels[PIN_COUNT];

static int rgb(const fc_hw_led_t *led, uint8_t r, uint8_t g, uint8_t b)
{
    if (!led->ws2812) return drive(led->pin, !!(r || g || b) ^ !!led->active_low);
    struct pixel *p = &pixels[led->pin];
    uint8_t bytes[3];
    for (int i = 0; i < 3; ++i) {
        char c = led->order[i];
        bytes[i] = c == 'r' ? r : c == 'b' ? b : g;
    }
    const rmt_transmit_config_t xfer = {0};
    if (rmt_transmit(p->channel, p->encoder, bytes, sizeof(bytes), &xfer) != ESP_OK ||
        rmt_tx_wait_all_done(p->channel, -1) != ESP_OK) return FC_HW_EIO;
    usleep(300);
    return 0;
}

static void dispose_pixel(struct pixel *p)
{
    if (p->channel) {
        rmt_disable(p->channel);
        rmt_del_channel(p->channel);
    }
    if (p->encoder) rmt_del_encoder(p->encoder);
    memset(p, 0, sizeof(*p));
}

static int led_open(const fc_hw_led_t *led)
{
    if (reserved(led->pin)) return FC_HW_EINVAL;
    if (!led->ws2812) {
        drive(led->pin, led->active_low);
        return setup(led->pin, FC_HW_GPIO_OUT);
    }
    struct pixel *p = &pixels[led->pin];
    const rmt_tx_channel_config_t cfg = {
        .gpio_num = led->pin, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = 48, .trans_queue_depth = 1,
    };
    const rmt_bytes_encoder_config_t encoding = {
        .bit0 = {.duration0 = 4, .level0 = 1, .duration1 = 9},
        .bit1 = {.duration0 = 8, .level0 = 1, .duration1 = 5},
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cfg, &p->channel) == ESP_OK &&
        rmt_new_bytes_encoder(&encoding, &p->encoder) == ESP_OK &&
        rmt_enable(p->channel) == ESP_OK && rgb(led, 0, 0, 0) == 0) return 0;
    dispose_pixel(p);
    return FC_HW_EIO;
}

static void led_close(const fc_hw_led_t *led)
{
    rgb(led, 0, 0, 0);
    if (led->ws2812) dispose_pixel(&pixels[led->pin]);
    release(led->pin);
}

static pthread_mutex_t adc_lock = PTHREAD_MUTEX_INITIALIZER;
static int adc_open(int pin)
{
    if (pin < 1 || pin > 10) return FC_HW_ENOTSUP;
    return esp_configgpio(pin, FUNCTION_2) == 0 ? pin : FC_HW_EIO;
}

static int adc_mv(int pin)
{
    int result = FC_HW_EIO;
    pthread_mutex_lock(&adc_lock);
    int fd = open("/dev/adc0", O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
        struct adc_msg_s samples[10];
        if (ioctl(fd, ANIOC_TRIGGER, 0) == 0) {
            ssize_t n = read(fd, samples, sizeof(samples));
            for (ssize_t i = 0; i < n / (ssize_t)sizeof(*samples); i++) {
                if (samples[i].am_channel == pin - 1) result = samples[i].am_data;
            }
        }
        close(fd);
    }
    pthread_mutex_unlock(&adc_lock);
    return result;
}

static int pwm_hz, pwm_pin = -1;
static pthread_mutex_t pwm_lock = PTHREAD_MUTEX_INITIALIZER;
void fc_nuttx_pwm_route(int pin);
static int pwm_duty(int h, uint32_t duty)
{
    struct pwm_info_s info = {.frequency = pwm_hz};
    info.channels[0].duty = duty;
    if (duty == 0 || duty == 65535) {
        if (ioctl(h, PWMIOC_STOP, 0) < 0) return FC_HW_EIO;
        drive(pwm_pin, duty != 0);
        return setup(pwm_pin, FC_HW_GPIO_OUT);
    }
    /* Restore the LEDC output route after a constant level. */
    esp_gpio_matrix_out(pwm_pin, LEDC_LS_SIG_OUT0_IDX, false, false);
    return ioctl(h, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) == 0 &&
           ioctl(h, PWMIOC_START, 0) == 0 ? 0 : FC_HW_EIO;
}

static int pwm_open(int pin, int hz)
{
    if (reserved(pin) || hz <= 0) return FC_HW_EINVAL;
    pthread_mutex_lock(&pwm_lock);
    int rc = FC_HW_EBUSY;
    if (pwm_pin < 0) {
        fc_nuttx_pwm_route(pin);
        int fd = open("/dev/pwm0", O_RDWR);
        rc = FC_HW_EIO;
        if (fd >= 0) {
            pwm_pin = pin;
            pwm_hz = hz;
            /* Start the timer at zero duty to validate its clock divisor. */
            struct pwm_info_s info = {.frequency = hz};
            if (ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) == 0 &&
                ioctl(fd, PWMIOC_START, 0) == 0 && pwm_duty(fd, 0) == 0) rc = fd;
            else { close(fd); release(pin); pwm_pin = -1; }
        }
    }
    pthread_mutex_unlock(&pwm_lock);
    return rc;
}

static void pwm_close(int fd)
{
    pthread_mutex_lock(&pwm_lock);
    pwm_duty(fd, 0);
    close(fd);
    release(pwm_pin);
    pwm_pin = -1;
    pthread_mutex_unlock(&pwm_lock);
}

const fc_hw_t fc_nuttx_hardware = {
    .board = "esp32s3", .firmware = FC_AGENT_VERSION, .pin_reserved = reserved,
    .gpio_setup = gpio_setup, .gpio_read = level, .gpio_write = drive,
    .gpio_irq = gpio_irq, .gpio_release = release,
    .led_open = led_open, .led_rgb = rgb, .led_close = led_close,
    .adc_open = adc_open, .adc_read_mv = adc_mv, .adc_close = release,
    .pwm_open = pwm_open, .pwm_duty = pwm_duty, .pwm_close = pwm_close,
    .sensor_read = fc_nuttx_sensor, .sensor_subscribe = fc_nuttx_subscribe, .sensor_unsubscribe = fc_nuttx_unsubscribe,
    .i2c_open = fc_nuttx_i2c_open, .i2c_transfer = fc_nuttx_i2c_transfer, .i2c_close = fc_nuttx_i2c_close,
    .spi_open = fc_nuttx_spi_open, .spi_transfer = fc_nuttx_spi_transfer,
    .spi_release = fc_nuttx_spi_release, .spi_close = fc_nuttx_spi_close,
    .uart_open = fc_nuttx_uart_open, .uart_read = fc_nuttx_uart_read,
    .uart_write = fc_nuttx_uart_write, .uart_close = fc_nuttx_uart_close,
    .restart_was_crash = fc_nuttx_restart_was_crash,
};
