#pragma once
#include <stdbool.h>
#include <stdint.h>

/* (Re)configures the LED; a no-op if nothing changed. Returns 0 on success.
 * active_low only applies to a plain GPIO LED (e.g. ESP32-C3 SuperMini). */
int led_setup(int gpio, bool ws2812, bool active_low);
/* Byte order on the wire for WS2812-style LEDs, e.g. "grb" (WS2812B) or
 * "rgb" (some SK6812 variants). Returns -1 if not a permutation of r, g, b. */
int led_set_order(const char *order);
void led_set(uint8_t r, uint8_t g, uint8_t b);
