/*
 * Example application "line temperature alarm": samples the internal
 * temperature sensor and lights the LED (blue) above a threshold. Its settings come from the
 * Flight Control inline config file /etc/fc-app/alarm.json:
 *
 *   {"threshold_c": 40, "hysteresis_c": 1, "sample_ms": 2000,
 *    "led_gpio": 48, "led_ws2812": true, "led_order": "grb", "led_active_low": false}
 *
 * Every field is optional (missing fields take their defaults); led_gpio -1
 * means the board default, so a fleet template can set it per device with
 * {{ getOrDefault .metadata.labels "led-gpio" "-1" }}. A new file
 * applies at once, without reboot; an invalid one is rejected and the
 * previous settings stay.
 */
#include "alarm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "fc_agent.h"
#include "fc_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led.h"
#include "esp_timer.h"
#include "wasm_export.h"
#include "sdkconfig.h"

#define CONFIG_PATH "/etc/fc-app/alarm.json"

int fc_port_temperature(float *celsius);

typedef struct {
    float threshold_c;
    float hysteresis_c;
    int sample_ms;
    int led_gpio;
    bool led_ws2812;
    bool led_active_low;
    char led_order[4];
} alarm_cfg_t;

static const char *TAG = "alarm";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_led_lock; /* LED driver: alarm task and WASM app */
static volatile int64_t s_app_led_us; /* last LED write by a WASM app */
#define APP_LED_HOLD_US 1000000      /* the app owns the LED while it writes it */
static TaskHandle_t s_task;
static alarm_cfg_t s_cfg;
static bool s_alarm;
static unsigned s_transitions;
static float s_max_c = -100.0f; /* highest temperature seen since boot (v2) */

static alarm_cfg_t defaults(void)
{
    return (alarm_cfg_t){
        .threshold_c = 45.0f,
        .hysteresis_c = 1.0f,
        .sample_ms = 2000,
        .led_gpio = CONFIG_FC_LED_GPIO,
        .led_order = "grb",
#if CONFIG_FC_LED_WS2812
        .led_ws2812 = true,
#else
        .led_ws2812 = false,
#endif
    };
}

static int num_field(const cJSON *o, const char *name, double lo, double hi, double *out)
{
    const cJSON *v = cJSON_GetObjectItem(o, name);
    if (v == NULL) {
        return 0;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble < lo || v->valuedouble > hi) {
        ESP_LOGW(TAG, "%s: expected a number in [%g, %g]", name, lo, hi);
        return -1;
    }
    *out = v->valuedouble;
    return 0;
}

static int bool_field(const cJSON *o, const char *name, bool *out)
{
    const cJSON *v = cJSON_GetObjectItem(o, name);
    if (v == NULL) {
        return 0;
    }
    if (!cJSON_IsBool(v)) {
        ESP_LOGW(TAG, "%s: expected a boolean", name);
        return -1;
    }
    *out = cJSON_IsTrue(v);
    return 0;
}

static int parse(const char *json, alarm_cfg_t *out)
{
    cJSON *o = cJSON_Parse(json);
    if (!cJSON_IsObject(o)) {
        ESP_LOGW(TAG, "%s: not a JSON object", CONFIG_PATH);
        cJSON_Delete(o);
        return -1;
    }
    alarm_cfg_t c = defaults();
    double thr = c.threshold_c, hyst = c.hysteresis_c, ms = c.sample_ms, gpio = c.led_gpio;
    int rc = num_field(o, "threshold_c", -10, 80, &thr) | num_field(o, "hysteresis_c", 0, 20, &hyst) |
             num_field(o, "sample_ms", 100, 60000, &ms) | num_field(o, "led_gpio", -1, 48, &gpio);
    const cJSON *ord = cJSON_GetObjectItem(o, "led_order");
    if (ord != NULL) {
        const char *v = cJSON_GetStringValue(ord);
        if (v != NULL && strlen(v) == 3 && strchr(v, 'r') && strchr(v, 'g') && strchr(v, 'b')) {
            memcpy(c.led_order, v, 4);
        } else {
            ESP_LOGW(TAG, "led_order: expected a permutation of \"rgb\"");
            rc = -1;
        }
    }
    rc |= bool_field(o, "led_ws2812", &c.led_ws2812) | bool_field(o, "led_active_low", &c.led_active_low);
    cJSON_Delete(o);
    if (rc != 0) {
        return -1;
    }
    c.threshold_c = (float)thr;
    c.hysteresis_c = (float)hyst;
    c.sample_ms = (int)ms;
    if (gpio >= 0) { /* -1: keep the board default (CONFIG_FC_LED_GPIO) */
        c.led_gpio = (int)gpio;
    }
    *out = c;
    return 0;
}

static void set_config(const alarm_cfg_t *c, const char *source)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *c;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "config (%s): threshold %.1f C, hysteresis %.1f C, sample %d ms, LED GPIO %d%s", source,
             c->threshold_c, c->hysteresis_c, c->sample_ms, c->led_gpio, c->led_ws2812 ? " (WS2812)" : "");
    if (s_task) {
        xTaskNotifyGive(s_task); /* re-evaluate now with the new settings */
    }
}

int alarm_on_config_file(const char *path, const char *data, size_t len, void *ctx)
{
    if (strcmp(path, CONFIG_PATH) != 0) {
        return 0; /* not ours */
    }
    alarm_cfg_t c;
    if (parse(data, &c) != 0) {
        return -1;
    }
    set_config(&c, "fleet");
    return 0;
}

void alarm_custom_info(void *obj, void *ctx)
{
    cJSON *o = obj;
    char buf[16];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    alarm_cfg_t c = s_cfg;
    bool alarm = s_alarm;
    unsigned transitions = s_transitions;
    float max_c = s_max_c;
    xSemaphoreGive(s_lock);

    cJSON_AddStringToObject(o, "alarm", alarm ? "on" : "off");
    snprintf(buf, sizeof(buf), "%.1f", c.threshold_c);
    cJSON_AddStringToObject(o, "alarmThresholdC", buf);
    snprintf(buf, sizeof(buf), "%u", transitions);
    cJSON_AddStringToObject(o, "alarmTransitions", buf);
    if (max_c > -100.0f) {
        snprintf(buf, sizeof(buf), "%.1f", max_c);
        cJSON_AddStringToObject(o, "alarmMaxC", buf);
    }
}

static void alarm_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        alarm_cfg_t c = s_cfg;
        bool alarm = s_alarm;
        xSemaphoreGive(s_lock);

        xSemaphoreTake(s_led_lock, portMAX_DELAY);
        led_setup(c.led_gpio, c.led_ws2812, c.led_active_low);
        led_set_order(c.led_order);
        xSemaphoreGive(s_led_lock);
        float t;
        if (fc_port_temperature(&t) == 0) {
            bool next = alarm ? t >= c.threshold_c - c.hysteresis_c : t >= c.threshold_c;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_alarm = next;
            if (next != alarm) {
                s_transitions++;
            }
            if (t > s_max_c) {
                s_max_c = t;
            }
            xSemaphoreGive(s_lock);
            if (next != alarm) {
                ESP_LOGW(TAG, "alarm %s: %.1f C (threshold %.1f C)", next ? "ON" : "off", t, c.threshold_c);
                fc_agent_notify_status();
            }
            alarm = next;
        }
        /* Without a WASM app driving the LED, the built-in logic does. */
        if (esp_timer_get_time() - s_app_led_us > APP_LED_HOLD_US) {
            xSemaphoreTake(s_led_lock, portMAX_DELAY);
            led_set(0, 0, alarm ? 40 : 0); /* blue, like the WASM app */
            xSemaphoreGive(s_led_lock);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(c.sample_ms));
    }
}

void alarm_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_led_lock = xSemaphoreCreateMutex();
    alarm_cfg_t c = defaults();
    const char *source = "default";
    char *saved = NULL;
    size_t len = 0;
    if (fc_port_kv_get("file:" CONFIG_PATH, &saved, &len) == 0) {
        if (parse(saved, &c) == 0) {
            source = "fleet";
        } else {
            c = defaults();
        }
        free(saved);
    }
    set_config(&c, source);
    xTaskCreate(alarm_task, "alarm", 4096, NULL, 5, &s_task);
}

/* ---- host functions for WASM apps (module "env") ------------------------ */

static int32_t native_temp_mc(wasm_exec_env_t env)
{
    float t;
    return fc_port_temperature(&t) == 0 ? (int32_t)(t * 1000) : -273000;
}

static int32_t native_threshold_mc(wasm_exec_env_t env)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int32_t v = (int32_t)(s_cfg.threshold_c * 1000);
    xSemaphoreGive(s_lock);
    return v;
}

static void native_led(wasm_exec_env_t env, int32_t r, int32_t g, int32_t b)
{
    s_app_led_us = esp_timer_get_time();
    xSemaphoreTake(s_led_lock, portMAX_DELAY);
    led_set((uint8_t)r, (uint8_t)g, (uint8_t)b);
    xSemaphoreGive(s_led_lock);
}

static NativeSymbol s_natives[] = {
    {"temp_mc", (void *)native_temp_mc, "()i", NULL},
    {"threshold_mc", (void *)native_threshold_mc, "()i", NULL},
    {"led", (void *)native_led, "(iii)", NULL},
};

const void *alarm_wasm_natives(int *count)
{
    *count = sizeof(s_natives) / sizeof(s_natives[0]);
    return s_natives;
}
