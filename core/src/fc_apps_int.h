/*
 * What fc_apps.c (apps: install, run, status) and fc_appapi.c (the functions
 * of module "fc" that apps call) share.
 */
#pragma once

#include <pthread.h>
#include <stdint.h>

#include "cJSON.h"
#include "fc_board.h"
#include "fc_hw.h"
#include "wasm_export.h"

#define MAX_APPS      2
#define APP_HANDLES   12
#define APP_TIMERS    4  /* FC_MAX_TIMERS */
#define APP_EVENTS    16 /* FC_EVENT_QUEUE */
#define APP_CALL_MS   1000 /* FC_CALL_LIMIT_MS: what a call may spend waiting in host functions */
#define APP_WAIT_MS   100  /* FC_WAIT_MAX_MS */
#define APP_KV_QUOTA  4096
#define APP_KV_KEY    31

/* Events, as FC_EV_* in fc_app.h. */
enum { EV_GPIO = 1, EV_TIMER = 2, EV_UART = 3, EV_MQTT = 4, EV_CONFIG = 5, EV_NET = 6, EV_OVERFLOW = 7 };

enum { H_FREE = 0, H_TIMER, H_GPIO, H_LED };

struct app;

/* What a GPIO interrupt callback carries: the handle, and the serial it had
 * when the interrupt was set, so that a late edge of a closed handle is
 * dropped. Lives in app_t, which is never freed. */
typedef struct {
    struct app *app;
    int handle;
} app_irq_ctx_t;

typedef struct {
    int kind;
    uint32_t serial;          /* changes at each open: late interrupts see it */
    fc_board_entry_t *res;    /* GPIO, LED */
    int mode;                 /* GPIO */
    int irq;                  /* GPIO: interrupt set */
    int debounce_ms;
    int64_t last_edge_ms;
    int64_t due_ms;           /* timer */
    int32_t period_ms;
    int periodic;
    int fired;
} app_handle_t;

typedef struct {
    int32_t type, handle, value;
} app_event_t;

typedef struct app {
    int slot;
    int used;
    char name[48];
    char image[200];
    char digest[72];
    int tick_ms;
    char *env_json; /* the app's envVars as JSON, as the spec gave them */
    char allow[160]; /* envVars.FC_ALLOW */
    /* Loaded once and owned by the slot; the runner only instantiates it.
     * NULL when the stored module could not be loaded. The .wasm bytes are
     * not kept in RAM (load_module). */
    wasm_module_t module;

    pthread_t thread;
    pthread_attr_t thread_attr; /* until the thread is joined: on Zephyr it owns the stack */
    int thread_running;
    int stop; /* set by the agent's thread, read by the runner: __atomic */

    /* reported (under s_lock in fc_apps.c) */
    char status[12]; /* Preparing, Running, Error, Stopped */
    int restarts;
    char state[64];
    char error[96];

    /* The running instance (runner thread). The handles' kinds and serials
     * and the event queue are also touched by GPIO callbacks: under ev_lock. */
    wasm_module_inst_t inst;
    int64_t call_start_ms;
    cJSON *config;
    app_handle_t h[APP_HANDLES];
    app_irq_ctx_t irq_ctx[APP_HANDLES];
    pthread_mutex_t *ev_lock; /* the slot's, set by fc_apps.c */
    pthread_cond_t *ev_cond;
    int accepting; /* events are queued only while the instance runs */
    app_event_t ev[APP_EVENTS];
    int ev_head, ev_count;
    int32_t ev_lost;
} app_t;

extern const fc_hw_t *g_fc_hw;

int64_t fc_apps_now_ms(void);
int fc_apps_stopping(const app_t *a);
const char *fc_apps_device_name(void);

/* fc_set_state. */
void fc_apps_set_state_native(wasm_exec_env_t env, const char *s, int32_t len);

/* Queues an event for the app's on_event; drops it when the instance is not
 * running. Any thread. */
void fc_apps_post(app_t *a, int32_t type, int32_t handle, int32_t value);

/* fc_appapi.c */
NativeSymbol *fc_appapi_natives(int *count);
/* Reads the app's config file again (runner, before FC_EV_CONFIG). */
void fc_appapi_load_config(app_t *a);
/* Closes all the app's handles, leaving the hardware safe (runner, at the
 * end of an instance). */
void fc_appapi_close_all(app_t *a);
/* The app's key/value record, removed with the app. */
void fc_appapi_kv_key(const char *app_name, char *out, size_t len);
