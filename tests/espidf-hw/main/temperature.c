/* Exercise the production sensor and its subscription lifecycle in esp-emu. */
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include "fc_temperature.h"
#include "hw.h"

#define CHECK(expr) do { if (!(expr)) { printf("WIRING FAIL: %s\n", #expr); abort(); } } while (0)

#if SOC_TEMP_SENSOR_SUPPORTED
static void temperature_event(void *done)
{
    xSemaphoreGive(done);
}
#endif

void check_temperature(const fc_hw_t *hw)
{
    int32_t mc;
    fc_idf_temperature_init();
#if SOC_TEMP_SENSOR_SUPPORTED
    CHECK(hw->sensor_read("die_temp", &mc) == 0);
    SemaphoreHandle_t ready = xSemaphoreCreateCounting(8, 0);
    SemaphoreHandle_t crossed = xSemaphoreCreateBinary();
    CHECK(ready && crossed);
    int sub = hw->sensor_subscribe(-1, FC_HW_SENSOR_READY, 0, 0, temperature_event, ready);
    CHECK(sub >= 0);
    CHECK(hw->sensor_subscribe(-1, FC_HW_SENSOR_THRESHOLD, 1000, 2000, temperature_event, crossed) == FC_HW_EBUSY);
    CHECK(xSemaphoreTake(ready, portMAX_DELAY) == pdTRUE);
    CHECK(xSemaphoreTake(ready, portMAX_DELAY) == pdTRUE);
    hw->sensor_unsubscribe(sub);
    /* Drain anything queued before close, then check the previous callback
     * stays quiet while the new subscription receives a sample. */
    xQueueReset(ready);
    CHECK(hw->sensor_subscribe(-1, FC_HW_SENSOR_THRESHOLD, 1, 1, temperature_event, crossed) == FC_HW_EINVAL);
    sub = hw->sensor_subscribe(-1, FC_HW_SENSOR_THRESHOLD, -200001, -199999, temperature_event, crossed);
    CHECK(sub >= 0 && xSemaphoreTake(crossed, portMAX_DELAY) == pdTRUE);
    hw->sensor_unsubscribe(sub);
    CHECK(xSemaphoreTake(ready, 0) == pdFALSE);
#if SOC_TEMPERATURE_SENSOR_INTR_SUPPORT
    sub = hw->sensor_subscribe(-1, FC_HW_SENSOR_THRESHOLD, 10000, 30000, temperature_event, crossed);
    CHECK(sub >= 0);
    hw->sensor_unsubscribe(sub);
#endif
    CHECK(hw->sensor_read("die_temp", &mc) == 0);
    vSemaphoreDelete(ready);
    vSemaphoreDelete(crossed);
#else
    float celsius = 123.0f;
    CHECK(fc_port_temperature(&celsius) == FC_HW_ENOTSUP && celsius == 123.0f);
    CHECK(hw->sensor_read("die_temp", &mc) == FC_HW_ENOTSUP);
    CHECK(!hw->sensor_subscribe && !hw->sensor_unsubscribe);
#endif
    puts("WIRING TEMPERATURE OK");
}

#if !CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_IDF_TARGET_ESP32C3
void app_main(void)
{
    check_temperature(hw_espidf());
}
#endif
