/* ESP-IDF die temperature shared by telemetry and WASM apps. */
#include "fc_temperature.h"
#include "fc_hw.h"
#include "soc/soc_caps.h"

#if SOC_TEMP_SENSOR_SUPPORTED
#include <pthread.h>
#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if SOC_TEMPERATURE_SENSOR_INTR_SUPPORT
#include "freertos/timers.h"
#endif

static const char *TAG = "tsens";
static temperature_sensor_handle_t s_tsens;
/* The status thread, the alarm task and WASM apps all read the sensor. On
 * the S2 a read starts a conversion and busy-waits for it, without a lock in
 * the driver: two overlapping reads can leave one spinning forever. */
static pthread_mutex_t s_tsens_lock = PTHREAD_MUTEX_INITIALIZER;

static void (*s_tsens_notify)(void *);
static void *s_tsens_ctx;
static TaskHandle_t s_sampler;
static int s_sample_mode, s_zone;
static int32_t s_low, s_high;

/* The lock covers the conversion and dispatch. Unsubscribe drains any
 * callback before its app context can be freed. A notification wakes the
 * worker on close so an idle probe costs neither a stack nor conversions. */
static void temperature_sample_task(void *unused)
{
    for (;;) {
        bool due = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)) == 0;
        pthread_mutex_lock(&s_tsens_lock);
        if (!s_sample_mode) {
            s_sampler = NULL;
            pthread_mutex_unlock(&s_tsens_lock);
            vTaskDelete(NULL);
            return;
        }
        float celsius;
        if (due && temperature_sensor_get_celsius(s_tsens, &celsius) == ESP_OK) {
            int32_t mc = (int32_t)(celsius * 1000.0f);
            int zone = mc <= s_low ? -1 : mc >= s_high ? 1 : 0;
            if (s_sample_mode == FC_HW_SENSOR_READY || zone != s_zone) {
                s_tsens_notify(s_tsens_ctx);
            }
            s_zone = zone;
        }
        pthread_mutex_unlock(&s_tsens_lock);
    }
}

#if SOC_TEMPERATURE_SENSOR_INTR_SUPPORT
/* Odd generations own the comparator. Queued interrupts carry the
 * generation instead of a pointer into an app that may have stopped. */
static uint32_t s_tsens_generation, s_tsens_pending;

static void temperature_dispatch(void *unused, uint32_t generation)
{
    pthread_mutex_lock(&s_tsens_lock);
    uint32_t expected = generation;
    __atomic_compare_exchange_n(&s_tsens_pending, &expected, 0, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    if (generation == __atomic_load_n(&s_tsens_generation, __ATOMIC_RELAXED) && s_tsens_notify) {
        s_tsens_notify(s_tsens_ctx);
    }
    pthread_mutex_unlock(&s_tsens_lock);
}

static bool IRAM_ATTR temperature_interrupt(temperature_sensor_handle_t sensor,
                                            const temperature_sensor_threshold_event_data_t *event, void *arg)
{
    uint32_t generation = __atomic_load_n(&s_tsens_generation, __ATOMIC_RELAXED);
    uint32_t expected = 0;
    BaseType_t woken = pdFALSE;
    if ((generation & 1) && __atomic_compare_exchange_n(&s_tsens_pending, &expected, generation, false,
                                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        if (xTimerPendFunctionCallFromISR(temperature_dispatch, NULL, generation, &woken) != pdPASS) {
            __atomic_compare_exchange_n(&s_tsens_pending, &generation, 0, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
    }
    return woken == pdTRUE;
}

/* Called with the sensor disabled and locked. Restore ordinary reads even
 * after an interrupt setup failure. */
static void temperature_disarm(void)
{
    temperature_sensor_event_callbacks_t off = {0};
    temperature_sensor_register_callbacks(s_tsens, &off, NULL);
    if (temperature_sensor_enable(s_tsens) != ESP_OK) {
        ESP_LOGE(TAG, "cannot restore temperature sampling");
    }
}

static int temperature_arm(int32_t low, int32_t high)
{
    if (temperature_sensor_disable(s_tsens) != ESP_OK) {
        return FC_HW_EIO;
    }
    temperature_sensor_abs_threshold_config_t window = {.low_threshold = low / 1000, .high_threshold = high / 1000};
    temperature_sensor_event_callbacks_t cbs = {.on_threshold = temperature_interrupt};
    if (temperature_sensor_set_absolute_threshold(s_tsens, &window) == ESP_OK &&
        temperature_sensor_register_callbacks(s_tsens, &cbs, NULL) == ESP_OK) {
        __atomic_fetch_add(&s_tsens_generation, 1, __ATOMIC_RELAXED);
        if (temperature_sensor_enable(s_tsens) == ESP_OK) {
            return 0;
        }
        __atomic_fetch_add(&s_tsens_generation, 1, __ATOMIC_RELAXED);
    }
    temperature_disarm();
    return FC_HW_EIO;
}
#endif

int fc_idf_sensor_subscribe(int id, int mode, int32_t low, int32_t high, void (*notify)(void *), void *ctx)
{
    if (id != -1) {
        return FC_HW_ENOTSUP;
    }
    if (!notify || (mode != FC_HW_SENSOR_READY && (mode != FC_HW_SENSOR_THRESHOLD || low >= high))) {
        return FC_HW_EINVAL;
    }
    if (!s_tsens) {
        return FC_HW_EIO;
    }
    pthread_mutex_lock(&s_tsens_lock);
    int rc = 0;
    if (s_tsens_notify) {
        rc = FC_HW_EBUSY;
#if SOC_TEMPERATURE_SENSOR_INTR_SUPPORT
    } else if (mode == FC_HW_SENSOR_THRESHOLD && low >= -10000 && high <= 80000 && low % 1000 == 0 && high % 1000 == 0) {
        rc = temperature_arm(low, high);
#endif
    } else if (!s_sampler &&
               xTaskCreate(temperature_sample_task, "fc_tsens", 3072, NULL, tskIDLE_PRIORITY + 1, &s_sampler) != pdPASS) {
        rc = FC_HW_ENOSPC;
    } else {
        s_sample_mode = mode;
        s_low = low;
        s_high = high;
        s_zone = 0;
    }
    if (rc == 0) {
        s_tsens_notify = notify;
        s_tsens_ctx = ctx;
    }
    pthread_mutex_unlock(&s_tsens_lock);
    return rc;
}

void fc_idf_sensor_unsubscribe(int subscription)
{
    pthread_mutex_lock(&s_tsens_lock);
    if (s_sample_mode) {
        s_sample_mode = 0;
        xTaskNotifyGive(s_sampler);
#if SOC_TEMPERATURE_SENSOR_INTR_SUPPORT
    } else if (s_tsens_notify) {
        __atomic_fetch_add(&s_tsens_generation, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s_tsens_pending, 0, __ATOMIC_RELAXED);
        if (temperature_sensor_disable(s_tsens) == ESP_OK) {
            temperature_disarm();
        }
#endif
    }
    s_tsens_notify = NULL;
    s_tsens_ctx = NULL;
    pthread_mutex_unlock(&s_tsens_lock);
}
#endif

void fc_idf_temperature_init(void)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_tsens);
    if (err == ESP_OK && (err = temperature_sensor_enable(s_tsens)) != ESP_OK) {
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "die temperature unavailable: %s", esp_err_to_name(err));
    }
#endif
}

int fc_port_temperature(float *celsius)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    if (!s_tsens) {
        return FC_HW_EIO;
    }
    pthread_mutex_lock(&s_tsens_lock);
    esp_err_t err = temperature_sensor_get_celsius(s_tsens, celsius);
    pthread_mutex_unlock(&s_tsens_lock);
    return err == ESP_OK ? 0 : FC_HW_EIO;
#else
    return FC_HW_ENOTSUP;
#endif
}
