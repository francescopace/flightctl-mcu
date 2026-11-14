/*
 * The hardware a port gives to WASM apps (the app interface of
 * wasm-apps/sdk/fc_app.h): GPIO, LEDs and the chip's sensors, under the names
 * of the board map.
 *
 * A port fills an fc_hw_t and puts it in fc_agent_config_t.hw. Every member
 * may be NULL: what is missing is FC_HW_ENOTSUP to the app. The core keeps
 * the board map, the grants and who holds what; the port only drives pins.
 * All functions are called from app threads, one app at a time per pin.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The app interface's error codes (FC_E* in fc_app.h). */
#define FC_HW_EINVAL    (-1)
#define FC_HW_ENOENT    (-2)
#define FC_HW_EPERM     (-3)
#define FC_HW_EBUSY     (-4)
#define FC_HW_EIO       (-5)
#define FC_HW_ETIMEDOUT (-6)
#define FC_HW_ENOSPC    (-7)
#define FC_HW_ENOTSUP   (-8)
#define FC_HW_EAGAIN    (-9)

/* GPIO modes, as FC_GPIO_* in fc_app.h. */
#define FC_HW_GPIO_IN          0
#define FC_HW_GPIO_IN_PULLUP   1
#define FC_HW_GPIO_IN_PULLDOWN 2
#define FC_HW_GPIO_OUT         3
#define FC_HW_GPIO_OUT_OD      4

/* An LED of the board map. */
typedef struct {
    int pin;
    int ws2812;     /* addressable RGB, else a plain LED on a GPIO */
    int active_low; /* plain LED only */
    char order[4];  /* WS2812 byte order, e.g. "grb" */
} fc_hw_led_t;

typedef struct {
    /* What fc_info gives the app as "board" and "firmware". */
    const char *board;
    const char *firmware;
    /* The firmware's own board map, as the JSON of /etc/fc/board.json (see
     * docs/drafts/APPS.md), allocated; the caller frees it. Called each time
     * the map is built (at start and when board.json changes), so it may
     * follow the firmware's settings. NULL: no defaults. */
    char *(*board_defaults)(void);
    /* 1 if the firmware uses the pin (flash, PSRAM, console) or the chip has
     * no such GPIO: a board map that names it is refused. */
    int (*pin_reserved)(int pin);

    /* Levels are electrical: the core handles active_low. */
    int (*gpio_setup)(int pin, int mode);
    int (*gpio_read)(int pin);
    int (*gpio_write)(int pin, int level);
    /* Calls cb(ctx) on each edge (1 rising, 2 falling, 3 both), from a task,
     * never from an interrupt; edge 0 stops it, and cb is not called for that
     * pin once that returns. The core debounces. */
    int (*gpio_irq)(int pin, int edge, void (*cb)(void *ctx), void *ctx);
    /* Back to an input without pulls, driving nothing. */
    void (*gpio_release)(int pin);

    int (*led_open)(const fc_hw_led_t *led);
    int (*led_rgb)(const fc_hw_led_t *led, uint8_t r, uint8_t g, uint8_t b);
    /* Off, and given back to the firmware if it uses it too. */
    void (*led_close)(const fc_hw_led_t *led);

    /* A sensor of the chip by name ("die_temp", milli-degrees C). */
    int (*sensor_read)(const char *name, int32_t *value);
} fc_hw_t;

#ifdef __cplusplus
}
#endif
