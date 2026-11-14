/*
 * The board map (fc_board.h): parsed from JSON, checked, and who holds what.
 */
#include "fc_board.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fc_port.h"

static fc_board_entry_t s_map[FC_BOARD_MAX];
static int s_count;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static const struct {
    const char *section;
    int kind;
} s_sections[] = {
    {"gpio", FC_RES_GPIO}, {"led", FC_RES_LED}, {"i2c", FC_RES_I2C}, {"spi", FC_RES_SPI},
    {"uart", FC_RES_UART}, {"adc", FC_RES_ADC}, {"pwm", FC_RES_PWM},
};

/* Members that are pins, in any kind of entry: all are checked against the
 * pins the firmware uses. */
static const char *const s_pin_fields[] = {"pin", "sda", "scl", "sck", "mosi", "miso", "cs", "tx", "rx", "de"};

static int int_field(const cJSON *o, const char *k, int def, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (v == NULL) {
        *out = def;
        return 0;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(int)v->valuedouble) {
        return -1;
    }
    *out = (int)v->valuedouble;
    return 0;
}

static int bool_field(const cJSON *o, const char *k, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (v == NULL) {
        *out = 0;
        return 0;
    }
    if (!cJSON_IsBool(v)) {
        return -1;
    }
    *out = cJSON_IsTrue(v);
    return 0;
}

static int valid_name(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > FC_BOARD_NAME_MAX || strcmp(n, "mqtt") == 0 || strcmp(n, "metrics") == 0) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

static int valid_order(const char *o)
{
    return strlen(o) == 3 && strchr(o, 'r') && strchr(o, 'g') && strchr(o, 'b');
}

/* One entry from its JSON object. */
static int parse_entry(const fc_hw_t *hw, int kind, const cJSON *o, fc_board_entry_t *e, char *err, size_t err_len)
{
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", o->string);
    e->kind = kind;
    e->holder = -1;
    if (!cJSON_IsObject(o)) {
        snprintf(err, err_len, "%s: not an object", o->string);
        return -1;
    }
    for (size_t i = 0; i < sizeof(s_pin_fields) / sizeof(s_pin_fields[0]); i++) {
        int pin;
        if (int_field(o, s_pin_fields[i], -1, &pin) != 0 || pin < -1) {
            snprintf(err, err_len, "%s: %s is not a pin number", e->name, s_pin_fields[i]);
            return -1;
        }
        if (pin >= 0 && hw != NULL && hw->pin_reserved != NULL && hw->pin_reserved(pin)) {
            snprintf(err, err_len, "%s: pin %d is used by the firmware or does not exist", e->name, pin);
            return -1;
        }
    }
    int safe;
    if (int_field(o, "pin", -1, &e->pin) != 0 || int_field(o, "safe", -1, &safe) != 0 ||
        bool_field(o, "active_low", &e->active_low) != 0 || bool_field(o, "ws2812", &e->led.ws2812) != 0) {
        snprintf(err, err_len, "%s: a field has the wrong type", e->name);
        return -1;
    }
    if (safe < -1 || safe > 1) {
        snprintf(err, err_len, "%s: safe is 0 or 1", e->name);
        return -1;
    }
    e->safe = safe;
    e->has_dt = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(o, "dt"));
    if (kind == FC_RES_LED) {
        const cJSON *ord = cJSON_GetObjectItemCaseSensitive(o, "order");
        snprintf(e->led.order, sizeof(e->led.order), "%s", cJSON_IsString(ord) ? ord->valuestring : "grb");
        if (!valid_order(e->led.order)) {
            snprintf(err, err_len, "%s: order is a permutation of \"rgb\"", e->name);
            return -1;
        }
        e->led.active_low = e->active_low;
    }
    return 0;
}

static int needs_pin(int kind)
{
    return kind == FC_RES_GPIO || kind == FC_RES_LED || kind == FC_RES_ADC || kind == FC_RES_PWM;
}

/* Adds the entries of a board map in JSON to map[0..*count), replacing those
 * with the same name. */
static int merge(const fc_hw_t *hw, const char *json, fc_board_entry_t *map, int *count, char *err, size_t err_len)
{
    cJSON *doc = cJSON_Parse(json);
    if (!cJSON_IsObject(doc)) {
        snprintf(err, err_len, "not a JSON object");
        cJSON_Delete(doc);
        return -1;
    }
    int rc = 0;
    const cJSON *sec;
    cJSON_ArrayForEach(sec, doc) {
        int kind = 0;
        for (size_t i = 0; i < sizeof(s_sections) / sizeof(s_sections[0]); i++) {
            if (strcmp(sec->string, s_sections[i].section) == 0) {
                kind = s_sections[i].kind;
            }
        }
        if (kind == 0 || !cJSON_IsObject(sec)) {
            snprintf(err, err_len, "unknown section \"%s\"", sec->string);
            rc = -1;
            break;
        }
        const cJSON *o;
        cJSON_ArrayForEach(o, sec) {
            if (!valid_name(o->string)) {
                snprintf(err, err_len, "\"%.30s\" is not a valid name", o->string);
                rc = -1;
                break;
            }
            fc_board_entry_t e;
            if (parse_entry(hw, kind, o, &e, err, err_len) != 0) {
                rc = -1;
                break;
            }
            int at = *count;
            for (int i = 0; i < *count; i++) {
                if (strcmp(map[i].name, e.name) == 0) {
                    at = i;
                }
            }
            /* No pin (or -1) for a name the firmware has: its pin, so that a
             * template can change an LED's colour order without knowing the
             * board's pin. */
            if (at < *count && map[at].kind == e.kind && e.pin < 0 && !e.has_dt) {
                e.pin = map[at].pin;
                e.has_dt = map[at].has_dt;
            }
            if (needs_pin(kind) && e.pin < 0 && !e.has_dt) {
                snprintf(err, err_len, "%s: no pin", e.name);
                rc = -1;
                break;
            }
            e.led.pin = e.pin;
            if (at == *count && *count == FC_BOARD_MAX) {
                snprintf(err, err_len, "more than %d names", FC_BOARD_MAX);
                rc = -1;
                break;
            }
            map[at] = e;
            *count += at == *count;
        }
        if (rc != 0) {
            break;
        }
    }
    cJSON_Delete(doc);
    return rc;
}

/* Two names on the same pin, for what drives a pin of its own. */
static int check_pins(const fc_board_entry_t *map, int count, char *err, size_t err_len)
{
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (map[i].pin >= 0 && map[i].pin == map[j].pin && map[i].kind != FC_RES_I2C &&
                map[i].kind != FC_RES_SPI && map[i].kind != FC_RES_UART && map[j].kind != FC_RES_I2C &&
                map[j].kind != FC_RES_SPI && map[j].kind != FC_RES_UART) {
                snprintf(err, err_len, "%s and %s are both on pin %d", map[i].name, map[j].name, map[i].pin);
                return -1;
            }
        }
    }
    return 0;
}

static int build(const fc_hw_t *hw, const char *fleet, fc_board_entry_t *map, int *count, char *err, size_t err_len)
{
    *count = 0;
    char *defaults = hw != NULL && hw->board_defaults != NULL ? hw->board_defaults() : NULL;
    int rc = 0;
    if (defaults != NULL && merge(NULL, defaults, map, count, err, err_len) != 0) {
        fc_port_log(FC_LOG_WARN, "the firmware's board map is not valid: %s", err);
        *count = 0;
    }
    free(defaults);
    if (fleet != NULL) {
        char why[100];
        if (merge(hw, fleet, map, count, why, sizeof(why)) != 0) {
            snprintf(err, err_len, "%s: %s", FC_BOARD_PATH, why);
            rc = -1;
        } else if (check_pins(map, *count, why, sizeof(why)) != 0) {
            snprintf(err, err_len, "%s: %s", FC_BOARD_PATH, why);
            rc = -1;
        }
    }
    return rc;
}

int fc_board_check(const fc_hw_t *hw, const char *json, char *err, size_t err_len)
{
    fc_board_entry_t *map = calloc(FC_BOARD_MAX, sizeof(*map));
    int count = 0;
    if (map == NULL) {
        snprintf(err, err_len, "%s: out of memory", FC_BOARD_PATH);
        return -1;
    }
    int rc = build(hw, json, map, &count, err, err_len);
    free(map);
    return rc;
}

void fc_board_load(const fc_hw_t *hw)
{
    char *fleet = NULL;
    size_t len = 0;
    if (fc_port_read_file(FC_BOARD_PATH, &fleet, &len) != 0) {
        fleet = NULL;
    }
    fc_board_entry_t *map = calloc(FC_BOARD_MAX, sizeof(*map)); /* not on the caller's small stack */
    int count = 0;
    char err[140];
    if (map == NULL) {
        fc_port_log(FC_LOG_ERROR, "board map: out of memory");
        free(fleet);
        return;
    }
    if (build(hw, fleet, map, &count, err, sizeof(err)) != 0) {
        fc_port_log(FC_LOG_WARN, "%s; using the firmware's board map", err);
        build(hw, NULL, map, &count, err, sizeof(err));
    }
    free(fleet);
    pthread_mutex_lock(&s_lock);
    memcpy(s_map, map, FC_BOARD_MAX * sizeof(*map));
    s_count = count;
    pthread_mutex_unlock(&s_lock);
    free(map);
}

fc_board_entry_t *fc_board_find(const char *name, size_t nlen)
{
    fc_board_entry_t *e = NULL;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < s_count && e == NULL; i++) {
        if (strlen(s_map[i].name) == nlen && memcmp(s_map[i].name, name, nlen) == 0) {
            e = &s_map[i];
        }
    }
    pthread_mutex_unlock(&s_lock);
    return e;
}

int fc_board_claim(fc_board_entry_t *e, int slot)
{
    pthread_mutex_lock(&s_lock);
    int rc = e->holder == -1 ? 0 : FC_HW_EBUSY; /* one handle per name, even within an app */
    if (rc == 0) {
        e->holder = slot;
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

void fc_board_release(fc_board_entry_t *e)
{
    pthread_mutex_lock(&s_lock);
    e->holder = -1;
    pthread_mutex_unlock(&s_lock);
}
