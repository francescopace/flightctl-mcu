/* Report changes at the wiper of esp-emu's potentiometer (C3 ADC1). */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw.h"

void app_main(void)
{
    fc_hw_t hw = {0};
    hw_bus_fill(&hw);
    int channel = hw.adc_open(3);
    if (channel < 0) {
        printf("ADC FAIL: open returned %d\n", channel);
        return;
    }
    int first = hw.adc_read_mv(channel);
    printf("ADC first %d\n", first);
    int next;
    do {
        vTaskDelay(1);
        next = hw.adc_read_mv(channel);
    } while (next == first);
    hw.adc_close(channel);
    printf("ADC second %d\n", next);
}
