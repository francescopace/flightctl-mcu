#pragma once

#include "fc_hw.h"

/* The hardware for apps of the 1.0 interface, for fc_agent_config_t.hw. */
const fc_hw_t *hw_espidf(void);

/* I2C, ADC and PWM (hw_bus.c). */
void hw_bus_fill(fc_hw_t *hw);
