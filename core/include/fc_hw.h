/*
 * The hardware a port gives to WASM apps (the app interface of
 * wasm-apps/sdk/fc_app.h): GPIO, LEDs, the chip's sensors, I2C, SPI, UART,
 * ADC and PWM,
 * under the names of the board map.
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

/* A bus of the board map: the pins it uses (-1 for none) and its settings. */
typedef struct {
    int sda, scl;            /* I2C */
    int sck, mosi, miso, cs; /* an SPI device: its bus's pins and its chip select (active low) */
    int mode;                /* SPI mode, 0 to 3 */
    int tx, rx, de;          /* UART; de drives an RS-485 transceiver while sending */
    int baud;                /* UART: the speed when the app gives 0; 0: 115200 */
    int hz;                  /* I2C or SPI clock; 0: the port's default (100 kHz, 1 MHz) */
    int dt;                  /* where the port names its hardware (dt_find): the node's id, else -1 */
} fc_hw_bus_t;

/* The kinds of a board map's entries. */
enum {
    FC_HW_KIND_GPIO = 1,
    FC_HW_KIND_LED,
    FC_HW_KIND_I2C,
    FC_HW_KIND_SPI,
    FC_HW_KIND_UART,
    FC_HW_KIND_ADC,
    FC_HW_KIND_PWM,
};

/* UART framings, as FC_UART_* in fc_app.h. */
#define FC_HW_UART_8N1 0
#define FC_HW_UART_8E1 1
#define FC_HW_UART_8N2 2

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
    /* A port that names its hardware in its own description (Zephyr's
     * devicetree) rather than by pin: the id of the node `name` of that
     * kind, or -1. The board map's entries then say "dt": name, never a
     * pin, and the functions below get that id where they take a pin, or in
     * fc_hw_bus_t.dt. NULL: pins. */
    int (*dt_find)(int kind, const char *name);

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

    /* The functions below return a port handle (>= 0) from open, or an
     * FC_HW_E* code; the core keeps it in the app's handle and gives it back.
     * Each call returns within timeout_ms where it takes one, and should
     * within 100 ms (FC_WAIT_MAX_MS) where it does not, so an app can be
     * stopped; where the system's driver takes no timeout (Zephyr's I2C and
     * SPI), a transfer lasts as long as the driver makes it. */

    /* I2C: a bus in master mode. A transfer writes w[0..wlen) to the 7-bit
     * address addr, then, if rlen > 0, reads r[0..rlen) after a repeated
     * start (wlen 0: a read alone). 0, FC_HW_EIO (no acknowledge, a bus
     * error) or FC_HW_ETIMEDOUT. */
    int (*i2c_open)(const fc_hw_bus_t *bus);
    int (*i2c_transfer)(int bus, int addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms);
    void (*i2c_close)(int bus);

    /* ADC: a pin's voltage in millivolts, calibrated where the chip allows;
     * FC_HW_ENOTSUP from open for a pin without a usable ADC channel. */
    int (*adc_open)(int pin);
    int (*adc_read_mv)(int ch);
    void (*adc_close)(int ch);

    /* PWM at freq_hz, starting at duty 0. duty is 0 (always low) to 65535
     * (always high), scaled to what the port's timer resolves. close leaves
     * the pin low and released. */
    int (*pwm_open)(int pin, int freq_hz);
    int (*pwm_duty)(int ch, uint32_t duty);
    void (*pwm_close)(int ch);

    /* SPI: a device on a bus; devices with the same sck, mosi and miso share
     * the bus. A transfer of len bytes sends tx (NULL: 0xFF) and receives
     * into rx (NULL: dropped), with the chip select held from start to end;
     * keep_cs leaves it held, and the bus the device's, for the next
     * transfer. 0, FC_HW_EBUSY (another device holds the bus past
     * timeout_ms) or FC_HW_EIO. release ends a kept chip select. */
    int (*spi_open)(const fc_hw_bus_t *dev);
    int (*spi_transfer)(int dev, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs, int timeout_ms);
    void (*spi_release)(int dev);
    void (*spi_close)(int dev);

    /* UART at baud with an FC_HW_UART_* framing; with u->de, RS-485 half
     * duplex. cb(ctx, n) is called from a task, never an interrupt, when
     * data has come (n bytes waiting), and not once close returns. write
     * takes what fits within timeout_ms and returns how many bytes; read
     * returns what has come, waiting up to timeout_ms for the first byte
     * (0: nothing). */
    int (*uart_open)(const fc_hw_bus_t *u, int baud, int format, void (*cb)(void *ctx, int n), void *ctx);
    int (*uart_write)(int h, const uint8_t *data, size_t len, int timeout_ms);
    int (*uart_read)(int h, uint8_t *buf, size_t cap, int timeout_ms);
    void (*uart_close)(int h);

    /* 1 if the device last restarted because the firmware crashed (a fault,
     * an abort, a watchdog), 0 if it was powered on or restarted on purpose.
     * Apps that keep crashing it as they start are then not started again
     * (fc_apps.c). NULL: it cannot tell, and no app is blamed. */
    int (*restart_was_crash)(void);
} fc_hw_t;

#ifdef __cplusplus
}
#endif
