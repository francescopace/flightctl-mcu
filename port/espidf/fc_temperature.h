#pragma once

#include <stdint.h>

void fc_idf_temperature_init(void);
int fc_port_temperature(float *celsius);
int fc_idf_sensor_subscribe(int id, int mode, int32_t low, int32_t high, void (*notify)(void *), void *ctx);
void fc_idf_sensor_unsubscribe(int subscription);
