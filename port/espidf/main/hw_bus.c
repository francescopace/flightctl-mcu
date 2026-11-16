/*
 * I2C, SPI, UART, ADC and PWM for WASM apps on ESP-IDF (fc_hw.h).
 *
 * I2C: the I2C master driver, one bus per board-map entry (ESP-IDF picks
 * the controller: two on the ESP32 and S3, one on the C chips), with one
 * device whose address changes to the one each transfer names.
 * ADC: ADC1 in one-shot mode at 12 dB (0 to about 3.1 V), calibrated with
 * the chip's eFuse values where it has them; ADC2 is left out, as on the
 * ESP32 it does not work while WiFi runs.
 * PWM: LEDC in low-speed mode, one timer per channel, so each PWM has its
 * own frequency: four at most.
 * SPI: the SPI master driver without DMA, so an app's memory (in PSRAM on
 * the S3) needs no copy: transfers go in pieces of 64 bytes, the FIFO, with
 * the chip select driven here as a GPIO, held across the pieces and, with
 * keep_cs, across transfers. Devices with the same pins share a host (SPI2,
 * and SPI3 on the ESP32 and S3); a mutex per bus keeps one device's held
 * chip select from meeting another's transfer.
 * UART: UART1 (and UART2 where the chip has it; UART0 is the console's), a
 * task per UART that hears the driver's events and tells the core.
 *
 * The core gives each handle to one app at a time; the tables here are
 * under s_lock, since two apps may open and close at once.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hw.h"
#include "soc/soc_caps.h"

static const char *TAG = "hw";
static SemaphoreHandle_t s_lock;

static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* ---- I2C ------------------------------------------------------------------------- */

#define I2C_BUSES 2

static struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int addr;
} s_i2c[I2C_BUSES];

static int i2c_open(const fc_hw_bus_t *b)
{
    lock();
    int i = 0;
    while (i < I2C_BUSES && s_i2c[i].bus != NULL) {
        i++;
    }
    if (i == I2C_BUSES) {
        unlock();
        return FC_HW_EBUSY;
    }
    i2c_master_bus_config_t bc = {
        .i2c_port = -1, /* the first free controller */
        .sda_io_num = b->sda,
        .scl_io_num = b->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1, /* weak: a bus wants its own pull-ups */
    };
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0, /* set before each transfer */
        .scl_speed_hz = b->hz > 0 ? (uint32_t)b->hz : 100000,
    };
    esp_err_t err = i2c_new_master_bus(&bc, &s_i2c[i].bus);
    if (err == ESP_OK && (err = i2c_master_bus_add_device(s_i2c[i].bus, &dc, &s_i2c[i].dev)) != ESP_OK) {
        i2c_del_master_bus(s_i2c[i].bus);
    }
    if (err != ESP_OK) {
        s_i2c[i].bus = NULL;
        unlock();
        ESP_LOGW(TAG, "I2C on sda %d scl %d: %s", b->sda, b->scl, esp_err_to_name(err));
        return err == ESP_ERR_NOT_FOUND ? FC_HW_EBUSY : FC_HW_EINVAL;
    }
    s_i2c[i].addr = 0;
    unlock();
    return i;
}

static int i2c_transfer(int i, int addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms)
{
    if (i < 0 || i >= I2C_BUSES || s_i2c[i].dev == NULL) {
        return FC_HW_EINVAL;
    }
    esp_err_t err = ESP_OK;
    if (s_i2c[i].addr != addr) {
        err = i2c_master_device_change_address(s_i2c[i].dev, (uint16_t)addr, timeout_ms);
        if (err == ESP_OK) {
            s_i2c[i].addr = addr;
        }
    }
    if (err == ESP_OK) {
        err = wlen > 0 && rlen > 0 ? i2c_master_transmit_receive(s_i2c[i].dev, w, wlen, r, rlen, timeout_ms)
              : wlen > 0           ? i2c_master_transmit(s_i2c[i].dev, w, wlen, timeout_ms)
                                   : i2c_master_receive(s_i2c[i].dev, r, rlen, timeout_ms);
    }
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_TIMEOUT:
        return FC_HW_ETIMEDOUT;
    case ESP_ERR_INVALID_ARG:
        return FC_HW_EINVAL;
    default:
        return FC_HW_EIO; /* ESP_ERR_INVALID_RESPONSE: no acknowledge */
    }
}

static void i2c_close(int i)
{
    if (i < 0 || i >= I2C_BUSES) {
        return;
    }
    lock();
    if (s_i2c[i].dev != NULL) {
        i2c_master_bus_rm_device(s_i2c[i].dev);
    }
    if (s_i2c[i].bus != NULL) {
        i2c_del_master_bus(s_i2c[i].bus);
    }
    memset(&s_i2c[i], 0, sizeof(s_i2c[i]));
    unlock();
}

/* ---- ADC --------------------------------------------------------------------------- */

#define ADC_CHANNELS 8

static adc_oneshot_unit_handle_t s_adc;
static int s_adc_users;
static struct {
    int used;
    adc_channel_t ch;
    adc_cali_handle_t cali; /* NULL: the chip has no calibration in eFuse */
} s_adc_ch[ADC_CHANNELS];

static adc_cali_handle_t adc_calibration(adc_channel_t ch)
{
    adc_cali_handle_t h = NULL;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t c = {
        .unit_id = ADC_UNIT_1, .chan = ch, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&c, &h) != ESP_OK) {
        h = NULL;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t c = {.unit_id = ADC_UNIT_1, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_line_fitting(&c, &h) != ESP_OK) {
        h = NULL;
    }
#endif
    return h;
}

static int adc_open(int pin)
{
    adc_unit_t unit;
    adc_channel_t ch;
    if (adc_oneshot_io_to_channel(pin, &unit, &ch) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO %d has no ADC1 channel", pin);
        return FC_HW_ENOTSUP;
    }
    lock();
    int i = 0;
    while (i < ADC_CHANNELS && s_adc_ch[i].used) {
        i++;
    }
    esp_err_t err = i == ADC_CHANNELS ? ESP_ERR_NO_MEM : ESP_OK;
    if (err == ESP_OK && s_adc == NULL) {
        adc_oneshot_unit_init_cfg_t uc = {.unit_id = ADC_UNIT_1, .ulp_mode = ADC_ULP_MODE_DISABLE};
        err = adc_oneshot_new_unit(&uc, &s_adc);
    }
    if (err == ESP_OK) {
        adc_oneshot_chan_cfg_t cc = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
        err = adc_oneshot_config_channel(s_adc, ch, &cc);
    }
    if (err != ESP_OK) {
        if (s_adc != NULL && s_adc_users == 0) {
            adc_oneshot_del_unit(s_adc);
            s_adc = NULL;
        }
        unlock();
        ESP_LOGW(TAG, "ADC on GPIO %d: %s", pin, esp_err_to_name(err));
        return err == ESP_ERR_NO_MEM ? FC_HW_ENOSPC : FC_HW_EIO;
    }
    s_adc_users++;
    s_adc_ch[i].used = 1;
    s_adc_ch[i].ch = ch;
    s_adc_ch[i].cali = adc_calibration(ch);
    unlock();
    return i;
}

static int adc_read_mv(int i)
{
    if (i < 0 || i >= ADC_CHANNELS || !s_adc_ch[i].used) {
        return FC_HW_EINVAL;
    }
    int raw = 0, mv = 0;
    lock();
    esp_err_t err = adc_oneshot_read(s_adc, s_adc_ch[i].ch, &raw);
    if (err == ESP_OK && s_adc_ch[i].cali != NULL) {
        err = adc_cali_raw_to_voltage(s_adc_ch[i].cali, raw, &mv);
    } else if (err == ESP_OK) {
        mv = raw * 3100 / 4095; /* uncalibrated: the range at 12 dB */
    }
    unlock();
    return err == ESP_OK ? mv : FC_HW_EIO;
}

static void adc_close(int i)
{
    if (i < 0 || i >= ADC_CHANNELS) {
        return;
    }
    lock();
    if (s_adc_ch[i].used) {
        if (s_adc_ch[i].cali != NULL) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
            adc_cali_delete_scheme_curve_fitting(s_adc_ch[i].cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
            adc_cali_delete_scheme_line_fitting(s_adc_ch[i].cali);
#endif
        }
        memset(&s_adc_ch[i], 0, sizeof(s_adc_ch[i]));
        if (--s_adc_users == 0) {
            adc_oneshot_del_unit(s_adc);
            s_adc = NULL;
        }
    }
    unlock();
}

/* ---- PWM --------------------------------------------------------------------------- */

#define PWM_CHANNELS 4 /* one LEDC timer each */
/* One clock for every timer, at 80 MHz: the low-speed timers share their
 * source, and LEDC_AUTO_CLK picks one per frequency (on the C6 a second PWM
 * at another frequency was refused, "timer clock conflict"). */
#define PWM_CLOCK_HZ 80000000
#if SOC_LEDC_SUPPORT_APB_CLOCK
#define PWM_CLOCK LEDC_USE_APB_CLK
#else
#define PWM_CLOCK LEDC_USE_PLL_DIV_CLK
#endif

static struct {
    int used;
    int pin;
    uint32_t bits;
} s_pwm[PWM_CHANNELS];

static int pwm_open(int pin, int hz)
{
    if (hz <= 0 || hz > PWM_CLOCK_HZ / 2) {
        return FC_HW_EINVAL;
    }
    /* The finest duty the frequency allows, up to 14 bits (every chip's). */
    uint32_t bits = 1;
    while (bits < 14 && ((uint32_t)PWM_CLOCK_HZ / (uint32_t)hz) >> (bits + 1) != 0) {
        bits++;
    }
    lock();
    int i = 0;
    while (i < PWM_CHANNELS && s_pwm[i].used) {
        i++;
    }
    if (i == PWM_CHANNELS) {
        unlock();
        return FC_HW_EBUSY;
    }
    ledc_timer_config_t tc = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = (ledc_timer_bit_t)bits,
        .timer_num = (ledc_timer_t)i,
        .freq_hz = (uint32_t)hz,
        .clk_cfg = PWM_CLOCK,
    };
    ledc_channel_config_t cc = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)i,
        .timer_sel = (ledc_timer_t)i,
        .duty = 0,
        .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&tc);
    if (err == ESP_OK) {
        err = ledc_channel_config(&cc);
    }
    if (err != ESP_OK) {
        unlock();
        ESP_LOGW(TAG, "PWM on GPIO %d at %d Hz: %s", pin, hz, esp_err_to_name(err));
        return FC_HW_EINVAL;
    }
    s_pwm[i].used = 1;
    s_pwm[i].pin = pin;
    s_pwm[i].bits = bits;
    unlock();
    return i;
}

static int pwm_duty(int i, uint32_t duty)
{
    if (i < 0 || i >= PWM_CHANNELS || !s_pwm[i].used) {
        return FC_HW_EINVAL;
    }
    /* 65535 is always on: 2^bits, which LEDC takes as 100%. */
    uint32_t d = (uint32_t)(((uint64_t)duty << s_pwm[i].bits) / 65535u);
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, d);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i);
    }
    return err == ESP_OK ? 0 : FC_HW_EIO;
}

static void pwm_close(int i)
{
    if (i < 0 || i >= PWM_CHANNELS) {
        return;
    }
    lock();
    if (s_pwm[i].used) {
        ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)i, 0); /* low */
        /* The channel lets go of the pin (else it stays reserved: "GPIO is
         * not usable" when opened again), which then drives nothing. */
        ledc_channel_config_t cc = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .channel = (ledc_channel_t)i, .deconfigure = true};
        ledc_channel_config(&cc);
        gpio_config_t g = {.pin_bit_mask = BIT64(s_pwm[i].pin), .mode = GPIO_MODE_INPUT};
        gpio_config(&g);
        ledc_timer_config_t tc = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = (ledc_timer_t)i, .deconfigure = true};
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, (ledc_timer_t)i);
        ledc_timer_config(&tc);
        memset(&s_pwm[i], 0, sizeof(s_pwm[i]));
    }
    unlock();
}

/* ---- SPI --------------------------------------------------------------------------- */

#if SOC_SPI_PERIPH_NUM > 2
#define SPI_HOSTS 2 /* SPI2 and SPI3; SPI1 is the flash's */
#else
#define SPI_HOSTS 1
#endif
#define SPI_DEVICES 4
#define SPI_PIECE SOC_SPI_MAXIMUM_BUFFER_SIZE

static struct {
    int users;
    int sck, mosi, miso;
    SemaphoreHandle_t lock; /* held for a transfer, or while a device keeps its chip select */
} s_spi_bus[SPI_HOSTS];

static struct {
    int used;
    int bus, cs;
    int kept; /* holds its bus's lock */
    spi_device_handle_t dev;
} s_spi_dev[SPI_DEVICES];

static spi_host_device_t spi_host(int bus)
{
#if SPI_HOSTS > 1
    return bus == 0 ? SPI2_HOST : SPI3_HOST;
#else
    return SPI2_HOST;
#endif
}

static int spi_open(const fc_hw_bus_t *d)
{
    lock();
    int b = -1, free_bus = -1, i = 0;
    for (int k = 0; k < SPI_HOSTS; k++) {
        if (s_spi_bus[k].users > 0 && s_spi_bus[k].sck == d->sck && s_spi_bus[k].mosi == d->mosi &&
            s_spi_bus[k].miso == d->miso) {
            b = k;
        } else if (s_spi_bus[k].users == 0 && free_bus < 0) {
            free_bus = k;
        }
    }
    while (i < SPI_DEVICES && s_spi_dev[i].used) {
        i++;
    }
    if (i == SPI_DEVICES || (b < 0 && free_bus < 0)) {
        unlock();
        return FC_HW_EBUSY;
    }
    esp_err_t err = ESP_OK;
    if (b < 0) {
        b = free_bus;
        spi_bus_config_t bc = {
            .sclk_io_num = d->sck,
            .mosi_io_num = d->mosi,
            .miso_io_num = d->miso,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = SPI_PIECE,
        };
        err = spi_bus_initialize(spi_host(b), &bc, SPI_DMA_DISABLED);
        if (err == ESP_OK) {
            s_spi_bus[b].sck = d->sck;
            s_spi_bus[b].mosi = d->mosi;
            s_spi_bus[b].miso = d->miso;
            if (s_spi_bus[b].lock == NULL) {
                /* Binary, not a mutex: a held chip select may be let go by
                 * another task than the one that took it. */
                s_spi_bus[b].lock = xSemaphoreCreateBinary();
                xSemaphoreGive(s_spi_bus[b].lock);
            }
        }
    }
    spi_device_interface_config_t dc = {
        .mode = (uint8_t)d->mode,
        .clock_speed_hz = d->hz > 0 ? d->hz : 1000000,
        .spics_io_num = -1, /* driven here */
        .queue_size = 1,
    };
    if (err == ESP_OK) {
        err = spi_bus_add_device(spi_host(b), &dc, &s_spi_dev[i].dev);
        if (err != ESP_OK && s_spi_bus[b].users == 0) {
            spi_bus_free(spi_host(b));
        }
    }
    if (err != ESP_OK) {
        unlock();
        ESP_LOGW(TAG, "SPI on sck %d mosi %d miso %d cs %d: %s", d->sck, d->mosi, d->miso, d->cs, esp_err_to_name(err));
        return FC_HW_EINVAL;
    }
    gpio_set_level(d->cs, 1);
    gpio_config_t g = {.pin_bit_mask = BIT64(d->cs), .mode = GPIO_MODE_OUTPUT};
    gpio_config(&g);
    gpio_set_level(d->cs, 1); /* not selected */
    s_spi_bus[b].users++;
    s_spi_dev[i].used = 1;
    s_spi_dev[i].bus = b;
    s_spi_dev[i].cs = d->cs;
    s_spi_dev[i].kept = 0;
    unlock();
    return i;
}

static void spi_end(int i)
{
    gpio_set_level(s_spi_dev[i].cs, 1);
    s_spi_dev[i].kept = 0;
    xSemaphoreGive(s_spi_bus[s_spi_dev[i].bus].lock);
}

static int spi_transfer(int i, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs, int timeout_ms)
{
    static const uint8_t ones[SPI_PIECE] = {[0 ... SPI_PIECE - 1] = 0xFF};
    if (i < 0 || i >= SPI_DEVICES || !s_spi_dev[i].used) {
        return FC_HW_EINVAL;
    }
    if (!s_spi_dev[i].kept && xSemaphoreTake(s_spi_bus[s_spi_dev[i].bus].lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return FC_HW_EBUSY;
    }
    gpio_set_level(s_spi_dev[i].cs, 0);
    esp_err_t err = ESP_OK;
    for (size_t at = 0; at < len && err == ESP_OK; at += SPI_PIECE) {
        size_t n = len - at < SPI_PIECE ? len - at : SPI_PIECE;
        spi_transaction_t t = {
            .length = n * 8,
            .rxlength = rx ? n * 8 : 0,
            .tx_buffer = tx ? tx + at : ones,
            .rx_buffer = rx ? rx + at : NULL,
        };
        err = spi_device_polling_transmit(s_spi_dev[i].dev, &t);
    }
    if (err == ESP_OK && keep_cs) {
        s_spi_dev[i].kept = 1;
        return 0;
    }
    spi_end(i);
    return err == ESP_OK ? 0 : FC_HW_EIO;
}

static void spi_release(int i)
{
    if (i >= 0 && i < SPI_DEVICES && s_spi_dev[i].used && s_spi_dev[i].kept) {
        spi_end(i);
    }
}

static void spi_close(int i)
{
    if (i < 0 || i >= SPI_DEVICES) {
        return;
    }
    spi_release(i);
    lock();
    if (s_spi_dev[i].used) {
        int b = s_spi_dev[i].bus;
        spi_bus_remove_device(s_spi_dev[i].dev);
        gpio_config_t g = {.pin_bit_mask = BIT64(s_spi_dev[i].cs), .mode = GPIO_MODE_INPUT};
        gpio_config(&g);
        if (--s_spi_bus[b].users == 0) {
            spi_bus_free(spi_host(b));
        }
        memset(&s_spi_dev[i], 0, sizeof(s_spi_dev[i]));
    }
    unlock();
}

/* ---- UART ------------------------------------------------------------------------- */

#define UART_PORTS (SOC_UART_HP_NUM - 1) /* UART0 is the console's */
#define UART_RX_BUF 512
#define UART_TX_BUF 256

static struct {
    int used;
    QueueHandle_t events;
    SemaphoreHandle_t done;
    volatile int stop;
    void (*cb)(void *, int);
    void *ctx;
} s_uart[UART_PORTS];

static uart_port_t uart_port(int i) { return (uart_port_t)(i + 1); }

static void uart_task(void *arg)
{
    int i = (int)(intptr_t)arg;
    uart_event_t ev;
    while (!s_uart[i].stop) {
        if (xQueueReceive(s_uart[i].events, &ev, portMAX_DELAY) != pdTRUE || s_uart[i].stop) {
            continue;
        }
        if (ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            ESP_LOGW(TAG, "UART%d: receive buffer full, data lost", (int)uart_port(i));
        }
        if (ev.type == UART_DATA || ev.type == UART_FIFO_OVF || ev.type == UART_BUFFER_FULL) {
            size_t n = 0;
            uart_get_buffered_data_len(uart_port(i), &n);
            if (n > 0) {
                s_uart[i].cb(s_uart[i].ctx, (int)n);
            }
        }
    }
    xSemaphoreGive(s_uart[i].done);
    vTaskDelete(NULL);
}

static int uart_open(const fc_hw_bus_t *u, int baud, int format, void (*cb)(void *, int), void *ctx)
{
    lock();
    int i = 0;
    while (i < UART_PORTS && s_uart[i].used) {
        i++;
    }
    if (i == UART_PORTS) {
        unlock();
        return FC_HW_EBUSY;
    }
    uart_port_t port = uart_port(i);
    uart_config_t c = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = format == FC_HW_UART_8E1 ? UART_PARITY_EVEN : UART_PARITY_DISABLE,
        .stop_bits = format == FC_HW_UART_8N2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_uart[i].cb = cb;
    s_uart[i].ctx = ctx;
    s_uart[i].stop = 0;
    if (s_uart[i].done == NULL) {
        s_uart[i].done = xSemaphoreCreateBinary();
    }
    esp_err_t err = uart_driver_install(port, UART_RX_BUF, UART_TX_BUF, 8, &s_uart[i].events, 0);
    if (err == ESP_OK) {
        err = uart_param_config(port, &c);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(port, u->tx, u->rx, u->de >= 0 ? u->de : UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK && u->de >= 0) {
        err = uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX); /* RTS, on de, high while sending */
    }
    if (err == ESP_OK && xTaskCreate(uart_task, "fc_uart", 2560, (void *)(intptr_t)i, 5, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        if (uart_is_driver_installed(port)) {
            uart_driver_delete(port);
        }
        unlock();
        ESP_LOGW(TAG, "UART on tx %d rx %d at %d: %s", u->tx, u->rx, baud, esp_err_to_name(err));
        return err == ESP_ERR_NO_MEM ? FC_HW_ENOSPC : FC_HW_EINVAL;
    }
    s_uart[i].used = 1;
    unlock();
    return i;
}

static int uart_write(int i, const uint8_t *d, size_t len, int timeout_ms)
{
    if (i < 0 || i >= UART_PORTS || !s_uart[i].used) {
        return FC_HW_EINVAL;
    }
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    size_t done = 0;
    while (done < len) {
        size_t room = 0;
        uart_get_tx_buffer_free_size(uart_port(i), &room);
        if (room > 0) {
            size_t n = len - done < room ? len - done : room;
            int w = uart_write_bytes(uart_port(i), d + done, n);
            if (w < 0) {
                return done > 0 ? (int)done : FC_HW_EIO;
            }
            done += (size_t)w;
        } else if ((int32_t)(xTaskGetTickCount() - end) >= 0) {
            break;
        } else {
            vTaskDelay(1);
        }
    }
    return (int)done;
}

static int uart_read(int i, uint8_t *b, size_t cap, int timeout_ms)
{
    if (i < 0 || i >= UART_PORTS || !s_uart[i].used) {
        return FC_HW_EINVAL;
    }
    size_t n = 0;
    uart_get_buffered_data_len(uart_port(i), &n);
    if (n == 0) {
        /* The first byte, within the timeout; then what came with it. */
        int got = uart_read_bytes(uart_port(i), b, 1, pdMS_TO_TICKS(timeout_ms));
        if (got <= 0) {
            return got < 0 ? FC_HW_EIO : 0;
        }
        uart_get_buffered_data_len(uart_port(i), &n);
        n = n < cap - 1 ? n : cap - 1;
        int more = n > 0 ? uart_read_bytes(uart_port(i), b + 1, n, 0) : 0;
        return 1 + (more > 0 ? more : 0);
    }
    int got = uart_read_bytes(uart_port(i), b, n < cap ? n : cap, 0);
    return got < 0 ? FC_HW_EIO : got;
}

static void uart_close(int i)
{
    if (i < 0 || i >= UART_PORTS) {
        return;
    }
    lock();
    if (s_uart[i].used) {
        /* The task ends before the driver goes, and calls no one after. */
        s_uart[i].stop = 1;
        uart_event_t wake = {.type = UART_EVENT_MAX};
        xQueueSend(s_uart[i].events, &wake, portMAX_DELAY);
        xSemaphoreTake(s_uart[i].done, portMAX_DELAY);
        uart_driver_delete(uart_port(i));
        s_uart[i].used = 0;
    }
    unlock();
}

void hw_bus_fill(fc_hw_t *hw)
{
    s_lock = xSemaphoreCreateMutex();
    hw->i2c_open = i2c_open;
    hw->i2c_transfer = i2c_transfer;
    hw->i2c_close = i2c_close;
    hw->adc_open = adc_open;
    hw->adc_read_mv = adc_read_mv;
    hw->adc_close = adc_close;
    hw->pwm_open = pwm_open;
    hw->pwm_duty = pwm_duty;
    hw->pwm_close = pwm_close;
    hw->spi_open = spi_open;
    hw->spi_transfer = spi_transfer;
    hw->spi_release = spi_release;
    hw->spi_close = spi_close;
    hw->uart_open = uart_open;
    hw->uart_write = uart_write;
    hw->uart_read = uart_read;
    hw->uart_close = uart_close;
}
