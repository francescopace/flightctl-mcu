/*
 * On-board LED: a WS2812 "RGB" LED driven through RMT (ESP32-S3/C3/C6
 * DevKits) or a plain GPIO LED. The GPIO can change at runtime from config.
 */
#include "led.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

static const char *TAG = "led";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
static int s_gpio = -1;
static bool s_ws2812;
static bool s_active_low;
static char s_order[4] = "grb";

static void led_release(void)
{
    if (s_chan) {
        rmt_disable(s_chan);
        rmt_del_channel(s_chan);
        s_chan = NULL;
    }
    if (s_enc) {
        rmt_del_encoder(s_enc);
        s_enc = NULL;
    }
    if (s_gpio >= 0) {
        gpio_reset_pin(s_gpio);
    }
    s_gpio = -1;
}

int led_setup(int gpio, bool ws2812, bool active_low)
{
    if (gpio == s_gpio && ws2812 == s_ws2812 && active_low == s_active_low) {
        return 0;
    }
    led_release();
    if (gpio < 0 || !GPIO_IS_VALID_OUTPUT_GPIO(gpio)) {
        ESP_LOGW(TAG, "invalid LED GPIO %d", gpio);
        return -1;
    }
    s_ws2812 = ws2812;
    s_active_low = active_low;
    if (!ws2812) {
        gpio_reset_pin(gpio);
        gpio_set_direction(gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(gpio, active_low ? 1 : 0);
        ESP_LOGI(TAG, "GPIO LED on GPIO %d%s", gpio, active_low ? " (active low)" : "");
        s_gpio = gpio;
        return 0;
    }
    rmt_tx_channel_config_t ch = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 0.1 us per tick */
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    rmt_copy_encoder_config_t enc = {};
    if (rmt_new_tx_channel(&ch, &s_chan) != ESP_OK || rmt_new_copy_encoder(&enc, &s_enc) != ESP_OK ||
        rmt_enable(s_chan) != ESP_OK) {
        ESP_LOGW(TAG, "RMT setup on GPIO %d failed", gpio);
        led_release();
        return -1;
    }
    s_gpio = gpio;
    ESP_LOGI(TAG, "WS2812 LED on GPIO %d", gpio);
    return 0;
}

int led_set_order(const char *order)
{
    if (order == NULL || strlen(order) != 3 || !strchr(order, 'r') || !strchr(order, 'g') || !strchr(order, 'b')) {
        return -1;
    }
    memcpy(s_order, order, 4);
    return 0;
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_gpio < 0) {
        return;
    }
    if (!s_ws2812) {
        gpio_set_level(s_gpio, ((r | g | b) != 0) != s_active_low);
        return;
    }
    /* At 10 MHz each bit is 12 ticks, high for 3 (zero) or 9 (one). */
    rmt_symbol_word_t frame[25];
    for (int i = 0; i < 3; i++) {
        uint8_t value = s_order[i] == 'r' ? r : s_order[i] == 'g' ? g : b;
        for (int bit = 0; bit < 8; bit++) {
            unsigned high = (value & (0x80 >> bit)) ? 9 : 3;
            frame[8 * i + bit] = (rmt_symbol_word_t){.level0 = 1, .duration0 = high, .duration1 = 12 - high};
        }
    }
    /* Keep the line low for 300 us so consecutive writes address the first LED. */
    frame[24] = (rmt_symbol_word_t){.duration0 = 1500, .duration1 = 1500};
    rmt_transmit_config_t tx = {.loop_count = 0};
    if (rmt_transmit(s_chan, s_enc, frame, sizeof(frame), &tx) == ESP_OK) {
        rmt_tx_wait_all_done(s_chan, 100);
    }
}
