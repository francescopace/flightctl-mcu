/*
 * I2C, ADC and PWM for WASM apps on ESP-IDF (fc_hw.h).
 *
 * I2C: the I2C master driver, one bus per board-map entry (ESP-IDF picks
 * the controller: two on the ESP32 and S3, one on the C chips), with one
 * device whose address changes to the one each transfer names.
 * ADC: ADC1 in one-shot mode at 12 dB (0 to about 3.1 V), calibrated with
 * the chip's eFuse values where it has them; ADC2 is left out, as on the
 * ESP32 it does not work while WiFi runs.
 * PWM: LEDC in low-speed mode, one timer per channel, so each PWM has its
 * own frequency: four at most.
 *
 * The core gives each handle to one app at a time; the tables here are
 * under s_lock, since two apps may open and close at once.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
}
