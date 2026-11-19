/*
 * What fc_apps.c (apps: install, run, status) and fc_appapi.c (the functions
 * of module "fc" that apps call) share.
 */
#pragma once

#include <pthread.h>
#include <stdint.h>
#include <unistd.h>

#include "cJSON.h"
#include "fc_apps.h"
#include "fc_board.h"
#include "fc_hw.h"
#include "fc_mqtt.h"
#include "wasm_export.h"
#ifdef FC_WASM_SRAM_PLACEMENT
#include "fc_wasm_placement.h"
#endif

#define APP_HANDLES   12
#define APP_TIMERS    4  /* FC_MAX_TIMERS */
#define APP_EVENTS    16 /* FC_EVENT_QUEUE */
#define APP_CALL_MS   1000 /* FC_CALL_LIMIT_MS: what a call may spend in the device's functions (waits, the C library) */
#define APP_WAIT_MS   100  /* FC_WAIT_MAX_MS */
#define APP_KV_QUOTA  4096
#define APP_KV_KEY    31

/* Events, as FC_EV_* in fc_app.h. */
enum { EV_GPIO = 1, EV_TIMER = 2, EV_UART = 3, EV_MQTT = 4, EV_CONFIG = 5, EV_NET = 6, EV_OVERFLOW = 7, EV_SENSOR = 8, EV_LOCAL = 9 };

enum { H_FREE = 0, H_TIMER, H_GPIO, H_LED, H_I2C, H_ADC, H_PWM, H_SPI, H_UART, H_MQTT, H_SENSOR };

struct app;

/* GPIO/UART callbacks run until the port finishes closing the handle.
 * The runner closes all handles before the slot is released. */
typedef struct {
    struct app *app;
    int handle;
} app_irq_ctx_t;

typedef struct {
    int kind;
    uint32_t serial;          /* changes at each open: late interrupts see it */
    fc_board_entry_t *res;    /* GPIO, LED and the buses */
    union {
        struct {              /* GPIO */
            int64_t last_edge_ms;
            uint16_t debounce_ms; /* 0..1000 */
            uint8_t mode;
            bool irq;
        };
        struct {              /* timer */
            int64_t due_ms;
            int32_t period_ms;
            bool periodic;
            bool fired;
        };
        struct {              /* sensor notification */
            int subscription;
            int trigger;
        };
        struct {              /* buses, ADC, PWM and MQTT */
            int port;
            bool kept;        /* SPI: chip select held past the transfer */
            bool ev_pending;  /* UART: an FC_EV_UART queued and not read since (under the slot lock) */
        };
    };
} app_handle_t;

/* 12 bytes on a 32-bit chip, as before it carried a message: on the ESP32-S3
 * with Zephyr the static RAM has little to spare. */
typedef struct {
    int8_t type;        /* EV_* */
    int8_t handle;      /* < APP_HANDLES, or -1 */
    int32_t value;
    fc_mqtt_msg_t *msg; /* FC_EV_MQTT: the message, the queue's until it is handled */
} app_event_t;

typedef struct {
    app_handle_t h[APP_HANDLES];
    app_irq_ctx_t irq_ctx[APP_HANDLES];
    app_event_t ev[APP_EVENTS];
    char out[96]; /* printf/puts/putchar: the line not yet logged */
} app_io_t;

/* A slot's permanent identity for asynchronous MQTT deliveries. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
} app_sync_t;

typedef struct app {
    uint8_t slot;
    uint8_t module_slot; /* flash slot selected by the committed catalog, 0 for a legacy module */
    bool primary;       /* FC_PRIMARY in the spec or saved catalog */
    bool parked;        /* verified module released, still in storage */
    bool can_grow;      /* under s_lock, while the instance is live */
    bool one_shot;      /* under s_lock: this instance runs FC_ENTRY or _start */
    bool thread_running;
    bool live;          /* under s_lock: an instance exists */
    bool settled;       /* past startup for the boot guard */
    bool retried_idle;  /* already released idle agent TLS for this refusal */
    bool relocated;     /* already unloaded this refused primary to coalesce its heap */
    bool mem_wait;      /* retry the instance when there is room */
    char name[FC_APP_NAME_MAX + 1];
    char image[200];
    char digest[72];
    int tick_ms;
    char *env_json; /* the app's envVars as JSON, as the spec gave them */
    char allow[160]; /* envVars.FC_ALLOW */
    char entry[32];  /* envVars.FC_ENTRY: an export run once, with no budget */
    /* Loaded once and owned by the slot; the runner only instantiates it.
     * NULL when the stored module could not be loaded. The .wasm bytes are
     * not kept in RAM (load_module). */
    wasm_module_t module;
#ifdef FC_WASM_SRAM_PLACEMENT
    fc_wasm_reserve_t linear; /* app_start gives ownership to the runner through go */
#endif

    pthread_t thread;
    pthread_attr_t thread_attr; /* until the thread is joined: on Zephyr it owns the stack */
    int made;                /* atomic: the runner has tried to make the instance (app_start waits for it) */
    int go;                  /* atomic: app_start let go of the heap's largest block, the runner may make the instance */
    int stop; /* set by the agent's thread, read by the runner: __atomic */
    long stack_unused;   /* lowest seen since app_start, -1 none: __atomic */
    int64_t stack_at_ms; /* runner: when it last read it */
    wasm_exec_env_t run_env; /* under s_lock: the call app_stop interrupts */
    int64_t started_ms;      /* when the instance started: env.clock_ms */
    const char *start_why;   /* runner: why it was started ("boot", "install"...), NULL for a restart */
    long heap_after;         /* runner: the heap free after the instance was made, -1 unknown */
    int heap_watch;          /* runner: watching the heap after it (fc_apps.c, heap_note_*) */
    int64_t heap_watch_ms, heap_quiet_ms;
    const char *start_pending; /* spec thread: to be started by fc_apps_start_pending, and why */
    /* The instance did not fit in the heap (no block for its memory, or the
     * agent's floor): its runner ended, and fc_apps_poll starts it again once
     * the heap has room (fc_apps.c, mem_room). */
    long mem_block;      /* the block its memory asked for, 0 unknown */
    long mem_cost;       /* refused by the floor: the heap its instance took, else 0 */
    long mem_largest;    /* the heap's largest free block when it failed, -1 unknown */
    int64_t mem_next_ms; /* fc_apps_poll: when to look again */
    int runner_done;     /* atomic: the runner has returned */

    /* reported (under s_lock in fc_apps.c) */
    char status[12]; /* Preparing, Running, Completed, Error, Stopped */
    int restarts;
    int64_t settle_at_ms; /* when it is past its start (fc_apps.c, guard) */
    long begun_oks;       /* statuses accepted when app_start started it, for the guard */
    char state[64];
    char error[96];

    /* The running instance (runner thread). The handles' kinds and serials
     * and the event queue are also touched by GPIO callbacks: under the slot lock. */
    wasm_module_inst_t inst;
    int64_t call_start_ms;
    int64_t native_us; /* what the call has spent in the device's functions (fc_apps_native_time) */
    app_io_t *io; /* allocated for a runner, freed when its app is parked */
    cJSON *config;
    app_sync_t *sync;
    bool unlimited; /* the call has no limits: a module run once (FC_ENTRY, _start) */
    bool accepting; /* events are queued only while the instance runs */
    uint8_t out_len;
    uint8_t ev_head, ev_count;
    int32_t ev_lost;
    fc_mqtt_msg_t *msg; /* runner: the message of the FC_EV_MQTT being handled */
} app_t;

extern const fc_hw_t *g_fc_hw;

int64_t fc_apps_now_ms(void);
int64_t fc_apps_now_us(void);
/* Counts the time since t0 (fc_apps_now_us) against the call's APP_CALL_MS in
 * the device's functions; past it, sets the exception (the app traps on
 * return) and returns 1. A module run once has no limit. */
int fc_apps_native_time(wasm_exec_env_t env, app_t *a, int64_t t0);
int fc_apps_stopping(const app_t *a);
const char *fc_apps_device_name(void);

int32_t fc_apps_local_send(wasm_exec_env_t env, const char *peer, int32_t len, int32_t value);

/* fc_set_state. */
void fc_apps_set_state_native(wasm_exec_env_t env, const char *s, int32_t len);

/* Queues an event for the app's on_event; drops it when the instance is not
 * running. Any thread. */
void fc_apps_post(app_t *a, int32_t type, int32_t handle, int32_t value);
/* The same with a message (FC_EV_MQTT), under the slot lock already: 0 queued
 * (the queue owns msg; when full, the oldest event is dropped), -1 not
 * running (msg is still the caller's). */
int fc_apps_post_locked(app_t *a, int32_t type, int32_t handle, int32_t value, fc_mqtt_msg_t *msg);
/* Drops the queued events for which keep() is 0, freeing their messages;
 * under the slot lock. keep NULL: all of them. */
void fc_apps_drop_events(app_t *a, int (*keep)(const app_event_t *e, int arg), int arg);

/* fc_appapi.c */
NativeSymbol *fc_appapi_natives(int *count);
/* Reads the app's config file again (runner, before FC_EV_CONFIG). */
void fc_appapi_load_config(app_t *a);
/* Closes all the app's handles, leaving the hardware safe (runner, at the
 * end of an instance). */
void fc_appapi_close_all(app_t *a);
/* After each call into the app: ends what may not outlast it (an SPI chip
 * select held with FC_SPI_KEEP_CS). */
void fc_appapi_end_call(app_t *a);
/* The app's key/value record, removed with the app. */
void fc_appapi_kv_key(const char *app_name, char *out, size_t len);
/* Whether FC_ALLOW grants `name`. */
int fc_appapi_allowed(const app_t *a, const char *name);
/* fc_mqtt_hooks_t.on_message: a message for the app's subscription. */
int fc_appapi_mqtt_message(app_t *a, int sub, fc_mqtt_msg_t *m);
/* fc's GPIO, LED and close functions, with a name in the firmware's memory:
 * for the module "arduino" (fc_arduino.c). */
int32_t fc_appapi_gpio_open(wasm_exec_env_t env, const char *name, int32_t mode);
int32_t fc_appapi_gpio_write(wasm_exec_env_t env, int32_t h, int32_t level);
int32_t fc_appapi_led_open(wasm_exec_env_t env, const char *name);
int32_t fc_appapi_led_rgb(wasm_exec_env_t env, int32_t h, int32_t r, int32_t g, int32_t b);
int32_t fc_appapi_close(wasm_exec_env_t env, int32_t h);

/* Waits ms in steps of 10 ms, at least one, each letting the other threads
 * run: fc_sleep_ms and arduino's delay. In a call with limits each step
 * counts against the call's second at once. 0, or 1: the app is stopping or
 * past its second (the exception is set). The steps sleep with
 * fc_appapi_usleep: usleep, or a counter in the unit tests. */
int fc_appapi_wait(wasm_exec_env_t env, app_t *a, uint32_t ms);
extern int (*fc_appapi_usleep)(useconds_t us);

/* fc_arduino.c: the module "arduino", for wasm3-arduino's examples. */
NativeSymbol *fc_arduino_natives(int *count);
