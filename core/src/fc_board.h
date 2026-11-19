/*
 * The board map: the names under which apps open GPIOs, LEDs, buses and
 * channels. It is the firmware's defaults (fc_hw_t.board_defaults) with the
 * fleet's /etc/fc/board.json on top, entry by entry. A name is unique in the
 * whole map, whatever its kind, and held by one app at a time.
 */
#pragma once

#include <stddef.h>

#include "fc_hw.h"

#define FC_BOARD_PATH    "/etc/fc/board.json"
#define FC_BOARD_MAX     24
#define FC_BOARD_NAME_MAX 23

typedef enum {
    FC_RES_GPIO = FC_HW_KIND_GPIO,
    FC_RES_LED = FC_HW_KIND_LED,
    FC_RES_I2C = FC_HW_KIND_I2C,
    FC_RES_SPI = FC_HW_KIND_SPI,
    FC_RES_UART = FC_HW_KIND_UART,
    FC_RES_ADC = FC_HW_KIND_ADC,
    FC_RES_PWM = FC_HW_KIND_PWM,
    FC_RES_SENSOR = FC_HW_KIND_SENSOR,
} fc_res_kind_t;

typedef enum {
    FC_SENSOR_TEMPERATURE,
    FC_SENSOR_BATTERY_ADC,
    FC_SENSOR_SBS_VOLTAGE,
    FC_SENSOR_SBS_PERCENT,
    FC_SENSOR_SBS_CHARGING,
} fc_sensor_source_t;

typedef struct {
    char name[FC_BOARD_NAME_MAX + 1];
    int kind;
    int pin;        /* -1 when the entry has none (a bus, a devicetree node) */
    int has_dt;     /* names a devicetree node ("dt") */
    union {
        char dt[FC_BOARD_NAME_MAX + 1];
        char input[FC_BOARD_NAME_MAX + 1]; /* ADC or I2C entry for battery sensors */
    };
    fc_sensor_source_t source;
    int divider_num, divider_den;
    int dt_id;      /* the port's id for it (fc_hw_t.dt_find), -1 if none */
    int active_low; /* GPIO */
    int safe;       /* GPIO output level when its app stops; -1: back to input */
    fc_hw_led_t led;
    fc_hw_bus_t bus; /* I2C, SPI, UART */
    int hz;          /* PWM: the frequency when the app gives 0; 0: 1 kHz */
    int holder;     /* the app slot holding it, -1 if none */
} fc_board_entry_t;

/* Builds the map from the firmware's defaults and the stored board.json. A
 * stored file that is not valid (it was checked when it came) is ignored,
 * with a log line. */
void fc_board_load(const fc_hw_t *hw);

/* Checks a board.json the fleet sends, against the firmware's defaults:
 * 0, or -1 with the reason in err. */
int fc_board_check(const fc_hw_t *hw, const char *json, char *err, size_t err_len);

/* The entry named name[0..nlen), or NULL. The map changes only while no app
 * runs, so the pointer stays valid while an app runs. */
fc_board_entry_t *fc_board_find(const char *name, size_t nlen);

/* What the port's functions take for the entry's pin: its pin, or the id of
 * its devicetree node. */
static inline int fc_board_pin(const fc_board_entry_t *e)
{
    return e->has_dt ? e->dt_id : e->pin;
}

/* Takes the entry for app `slot`: 0, or FC_HW_EBUSY if it is held already. */
int fc_board_claim(fc_board_entry_t *e, int slot);
void fc_board_release(fc_board_entry_t *e);
