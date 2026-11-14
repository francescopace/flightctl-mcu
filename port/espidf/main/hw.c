/*
 * The hardware WASM apps reach on ESP-IDF (fc_hw.h): GPIOs with interrupts,
 * the board's LED (led.c) and the chip's temperature sensor.
 *
 * The firmware's board map has led0, the board's LED (CONFIG_FC_LED_GPIO and
 * CONFIG_FC_LED_WS2812); the fleet moves it or adds the rest with
 * /etc/fc/board.json. Interrupts are taken in an ISR that only queues the
 * pin; a task calls the core, never the ISR.
 */
#include "hw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_private/esp_gpio_reserve.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"
#include "sdkconfig.h"
#include "soc/io_mux_reg.h"
#include "soc/soc_caps.h"
#include "soc/uart_pins.h"

int fc_port_temperature(float *celsius);

static const char *TAG = "hw";

static char *board_defaults(void)
{
    if (CONFIG_FC_LED_GPIO < 0) {
        return strdup("{}");
    }
#if CONFIG_FC_LED_WS2812
    const char *ws2812 = "true";
#else
    const char *ws2812 = "false";
#endif
    char *s = malloc(100);
    if (s != NULL) {
        snprintf(s, 100, "{\"led\": {\"led0\": {\"pin\": %d, \"ws2812\": %s}}}", CONFIG_FC_LED_GPIO, ws2812);
    }
    return s;
}

/* What the board map may not name: no such GPIO, flash and PSRAM (reserved
 * by ESP-IDF), the console's UART and the USB pins the console may use. */
static int pin_reserved(int pin)
{
    if (pin < 0 || !GPIO_IS_VALID_GPIO(pin) || esp_gpio_is_reserved(BIT64(pin))) {
        return 1;
    }
    if (pin == U0TXD_GPIO_NUM || pin == U0RXD_GPIO_NUM) {
        return 1;
    }
#ifdef USB_INT_PHY0_DM_GPIO_NUM
    if (pin == USB_INT_PHY0_DM_GPIO_NUM || pin == USB_INT_PHY0_DP_GPIO_NUM) {
        return 1;
    }
#endif
    return 0;
}

static int gpio_setup(int pin, int mode)
{
    gpio_config_t c = {
        .pin_bit_mask = BIT64(pin),
        .mode = mode == FC_HW_GPIO_OUT      ? GPIO_MODE_INPUT_OUTPUT
                : mode == FC_HW_GPIO_OUT_OD ? GPIO_MODE_INPUT_OUTPUT_OD
                                            : GPIO_MODE_INPUT,
        .pull_up_en = mode == FC_HW_GPIO_IN_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = mode == FC_HW_GPIO_IN_PULLDOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&c) == ESP_OK ? 0 : FC_HW_EIO;
}

static int gpio_read(int pin)
{
    return gpio_get_level(pin);
}

static int gpio_write(int pin, int level)
{
    return gpio_set_level(pin, level != 0) == ESP_OK ? 0 : FC_HW_EIO;
}

static void gpio_release(int pin)
{
    gpio_setup(pin, FC_HW_GPIO_IN);
}

/* ---- interrupts ------------------------------------------------------------ */

static QueueHandle_t s_edges;
static SemaphoreHandle_t s_dispatch; /* held while a callback runs: gpio_irq(0) waits for it */
static void (*s_cb[GPIO_NUM_MAX])(void *);
static void *s_ctx[GPIO_NUM_MAX];

static void IRAM_ATTR edge_isr(void *arg)
{
    uint8_t pin = (uint8_t)(uintptr_t)arg;
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_edges, &pin, &woken); /* dropped when full: the core debounces anyway */
    portYIELD_FROM_ISR(woken);
}

static void edge_task(void *arg)
{
    for (;;) {
        uint8_t pin;
        if (xQueueReceive(s_edges, &pin, portMAX_DELAY) == pdTRUE && pin < GPIO_NUM_MAX) {
            xSemaphoreTake(s_dispatch, portMAX_DELAY);
            if (s_cb[pin] != NULL) {
                s_cb[pin](s_ctx[pin]);
            }
            xSemaphoreGive(s_dispatch);
        }
    }
}

static int gpio_irq(int pin, int edge, void (*cb)(void *), void *ctx)
{
    static bool started;
    if (!started) {
        s_edges = xQueueCreate(16, sizeof(uint8_t));
        s_dispatch = xSemaphoreCreateMutex();
        esp_err_t err = gpio_install_isr_service(0);
        if (s_edges == NULL || s_dispatch == NULL || (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ||
            xTaskCreate(edge_task, "fc_gpio", 3072, NULL, 6, NULL) != pdPASS) {
            ESP_LOGE(TAG, "cannot start GPIO interrupts");
            return FC_HW_EIO;
        }
        started = true;
    }
    xSemaphoreTake(s_dispatch, portMAX_DELAY);
    gpio_intr_disable(pin);
    gpio_isr_handler_remove(pin);
    s_cb[pin] = edge ? cb : NULL;
    s_ctx[pin] = edge ? ctx : NULL;
    int rc = 0;
    if (edge) {
        gpio_int_type_t t = edge == 1 ? GPIO_INTR_POSEDGE : edge == 2 ? GPIO_INTR_NEGEDGE : GPIO_INTR_ANYEDGE;
        if (gpio_set_intr_type(pin, t) != ESP_OK ||
            gpio_isr_handler_add(pin, edge_isr, (void *)(uintptr_t)pin) != ESP_OK || gpio_intr_enable(pin) != ESP_OK) {
            s_cb[pin] = NULL;
            s_ctx[pin] = NULL;
            rc = FC_HW_EIO;
        }
    }
    xSemaphoreGive(s_dispatch);
    return rc;
}

/* ---- the LED ------------------------------------------------------------------- */

static SemaphoreHandle_t s_led_lock; /* led.c is not thread safe */

static int led_open(const fc_hw_led_t *led)
{
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    int rc = led_setup(led->pin, led->ws2812, led->active_low) == 0 && led_set_order(led->order) == 0 ? 0 : FC_HW_EIO;
    if (rc == 0) {
        led_set(0, 0, 0);
    }
    xSemaphoreGive(s_led_lock);
    return rc;
}

static int led_rgb(const fc_hw_led_t *led, uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    led_set(r, g, b);
    xSemaphoreGive(s_led_lock);
    return 0;
}

static void led_close(const fc_hw_led_t *led)
{
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    led_set(0, 0, 0);
    xSemaphoreGive(s_led_lock);
}

/* ---- sensors ----------------------------------------------------------------- */

static int sensor_read(const char *name, int32_t *value)
{
    if (strcmp(name, "die_temp") != 0) {
        return FC_HW_ENOENT;
    }
#if SOC_TEMP_SENSOR_SUPPORTED
    float c;
    if (fc_port_temperature(&c) != 0) {
        return FC_HW_EIO;
    }
    *value = (int32_t)(c * 1000.0f);
    return 0;
#else
    return FC_HW_ENOTSUP; /* the ESP32 has no sensor: its customInfo temperature is simulated */
#endif
}

const fc_hw_t *hw_espidf(void)
{
    static fc_hw_t hw;
    static char board[32];
    s_led_lock = xSemaphoreCreateMutex();
    snprintf(board, sizeof(board), "%s", CONFIG_IDF_TARGET);
    hw = (fc_hw_t){
        .board = board,
        .firmware = esp_app_get_description()->version,
        .board_defaults = board_defaults,
        .pin_reserved = pin_reserved,
        .gpio_setup = gpio_setup,
        .gpio_read = gpio_read,
        .gpio_write = gpio_write,
        .gpio_irq = gpio_irq,
        .gpio_release = gpio_release,
        .led_open = led_open,
        .led_rgb = led_rgb,
        .led_close = led_close,
        .sensor_read = sensor_read,
    };
    return &hw;
}
