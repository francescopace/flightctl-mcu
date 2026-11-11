/*
 * WASM application manager.
 *
 * Flight Control delivers apps as spec.applications entries of appType
 * "container"; on an MCU the image must be a WASM OCI artifact (layer
 * application/vnd.wasm.content.layer.v1+wasm, directly or through an index
 * entry with platform.architecture "wasm"). The module is kept in the port
 * key/value store, so apps restart after a reboot without the network.
 *
 * App contract (wasm-apps/sdk/fc_app.h, version FC_APP_ABI_VERSION): export
 * tick(i32 now_ms), called every TICK_MS (env var, default 100), and
 * optionally fc_abi_version(). Host functions: "env" module, the core's
 * set_state(ptr, len) and log(ptr, len) plus whatever the firmware registers
 * (sensors, LED...).
 * A trap stops the app; it is restarted after a back-off and counted in
 * status.applications[].restarts.
 */
#include "fc_apps.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fc_agent.h"
#include "fc_oci.h"
#include "fc_port.h"
#include "wasm_export.h"

#define MAX_APPS        2
#define FC_APP_ABI_VERSION 1 /* wasm-apps/sdk/fc_app.h */
#define MAX_WASM_SIZE   (64 * 1024)
#define WASM_STACK      2048 /* WASM operand/frame stack per app */
#if defined(__ZEPHYR__) && defined(CONFIG_ARCH_POSIX)
#define THREAD_STACK    (64 * 1024) /* a 64-bit host's frames, and Zephyr gives a thread what it asks */
#else
#define THREAD_STACK    8192
#endif
#define RESTART_DELAY_S 5
#define LAYER_WASM      "application/vnd.wasm.content.layer.v1+wasm"
#define KV_APPS         "apps"

typedef struct {
    int used;
    char name[48];
    char image[200];
    char digest[72];
    int tick_ms;
    unsigned char *wasm;
    size_t wasm_len;

    pthread_t thread;
    pthread_attr_t thread_attr; /* until the thread is joined: on Zephyr it owns the stack */
    int thread_running;
    volatile int stop;

    /* reported (under s_lock) */
    char status[12]; /* Preparing, Running, Error, Stopped */
    int restarts;
    char state[64];
    char error[96];
} app_t;

static app_t s_apps[MAX_APPS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_inited;

static void set_status(app_t *a, const char *status, const char *error)
{
    pthread_mutex_lock(&s_lock);
    int changed = strcmp(a->status, status) != 0 || strcmp(a->error, error ? error : "") != 0;
    snprintf(a->status, sizeof(a->status), "%s", status);
    snprintf(a->error, sizeof(a->error), "%s", error ? error : "");
    pthread_mutex_unlock(&s_lock);
    if (changed) {
        fc_agent_notify_status();
    }
}

/* ---- host functions provided by the core -------------------------------- */

static void native_set_state(wasm_exec_env_t env, const char *s, int32_t len)
{
    app_t *a = wasm_runtime_get_user_data(env);
    char next[sizeof(a->state)];
    snprintf(next, sizeof(next), "%.*s", (int)len, s);
    pthread_mutex_lock(&s_lock);
    int changed = strcmp(a->state, next) != 0;
    memcpy(a->state, next, sizeof(next));
    pthread_mutex_unlock(&s_lock);
    if (changed) {
        fc_port_log(FC_LOG_INFO, "app %s: %s", a->name, next);
        fc_agent_notify_status(); /* rate-limited by the status thread */
    }
}

static void native_log(wasm_exec_env_t env, const char *s, int32_t len)
{
    app_t *a = wasm_runtime_get_user_data(env);
    fc_port_log(FC_LOG_INFO, "app %s: %.*s", a->name, (int)len, s);
}

static NativeSymbol s_core_natives[] = {
    {"set_state", (void *)native_set_state, "(*~)", NULL},
    {"log", (void *)native_log, "(*~)", NULL},
};

/* ---- runner -------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void *runner(void *arg)
{
    app_t *a = arg;
    char ebuf[128];
    wasm_runtime_init_thread_env();
    /* WAMR may patch the buffer while loading: give it a private copy. */
    unsigned char *code = malloc(a->wasm_len);
    wasm_module_t module = NULL;
    if (code != NULL) {
        memcpy(code, a->wasm, a->wasm_len);
        module = wasm_runtime_load(code, (uint32_t)a->wasm_len, ebuf, sizeof(ebuf));
    }
    if (module == NULL) {
        set_status(a, "Error", code ? ebuf : "out of memory");
        fc_port_log(FC_LOG_WARN, "app %s: load: %s", a->name, code ? ebuf : "out of memory");
        goto done;
    }
    while (!a->stop) {
        wasm_module_inst_t inst = wasm_runtime_instantiate(module, WASM_STACK, 0, ebuf, sizeof(ebuf));
        wasm_exec_env_t env = inst ? wasm_runtime_create_exec_env(inst, WASM_STACK) : NULL;
        wasm_function_inst_t tick = inst ? wasm_runtime_lookup_function(inst, "tick") : NULL;
        if (inst == NULL || env == NULL || tick == NULL) {
            const char *why = inst == NULL ? ebuf : env == NULL ? "exec env: out of memory" : "no exported tick()";
            fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, why);
            set_status(a, "Error", why);
        } else {
            wasm_runtime_set_user_data(env, a);
            wasm_function_inst_t abi = wasm_runtime_lookup_function(inst, "fc_abi_version");
            uint32_t abi_argv[1] = {0};
            if (abi != NULL && wasm_runtime_call_wasm(env, abi, 0, abi_argv) && abi_argv[0] > FC_APP_ABI_VERSION) {
                snprintf(ebuf, sizeof(ebuf), "app needs interface v%u, this device has v%d", (unsigned)abi_argv[0],
                         FC_APP_ABI_VERSION);
                fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, ebuf);
                set_status(a, "Error", ebuf);
                wasm_runtime_destroy_exec_env(env);
                wasm_runtime_deinstantiate(inst);
                wasm_runtime_unload(module);
                goto done;
            }
            set_status(a, "Running", NULL);
            fc_port_log(FC_LOG_INFO, "app %s running (%s, tick %d ms)", a->name, a->image, a->tick_ms);
            while (!a->stop) {
                uint32_t argv[1] = {now_ms()};
                if (!wasm_runtime_call_wasm(env, tick, 1, argv)) {
                    const char *ex = wasm_runtime_get_exception(inst);
                    fc_port_log(FC_LOG_WARN, "app %s trapped: %s", a->name, ex ? ex : "?");
                    set_status(a, "Error", ex);
                    break;
                }
                for (int slept = 0; slept < a->tick_ms && !a->stop; slept += 50) {
                    usleep((useconds_t)(a->tick_ms - slept < 50 ? a->tick_ms - slept : 50) * 1000);
                }
            }
        }
        if (env) {
            wasm_runtime_destroy_exec_env(env);
        }
        if (inst) {
            wasm_runtime_deinstantiate(inst);
        }
        if (!a->stop) {
            for (int i = 0; i < RESTART_DELAY_S * 10 && !a->stop; i++) {
                usleep(100 * 1000);
            }
            if (!a->stop) {
                pthread_mutex_lock(&s_lock);
                a->restarts++;
                pthread_mutex_unlock(&s_lock);
            }
        }
    }
    wasm_runtime_unload(module);
done:
    free(code);
    wasm_runtime_destroy_thread_env();
    return NULL;
}

static void app_start(app_t *a)
{
    a->stop = 0;
    set_status(a, "Preparing", NULL);
    pthread_attr_init(&a->thread_attr);
    pthread_attr_setstacksize(&a->thread_attr, THREAD_STACK);
    a->thread_running = pthread_create(&a->thread, &a->thread_attr, runner, a) == 0;
    if (!a->thread_running) {
        pthread_attr_destroy(&a->thread_attr);
        set_status(a, "Error", "cannot start thread");
    }
}

static void app_stop(app_t *a)
{
    if (a->thread_running) {
        a->stop = 1;
        pthread_join(a->thread, NULL);
        pthread_attr_destroy(&a->thread_attr);
        a->thread_running = 0;
    }
    set_status(a, "Stopped", NULL);
}

/* ---- persistence ----------------------------------------------------------- */

static void wasm_key(const char *name, char *out, size_t len)
{
    snprintf(out, len, "app:%s", name);
}

static void save_list(void)
{
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < MAX_APPS; i++) {
        app_t *a = &s_apps[i];
        if (!a->used) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", a->name);
        cJSON_AddStringToObject(o, "image", a->image);
        cJSON_AddStringToObject(o, "digest", a->digest);
        cJSON_AddNumberToObject(o, "tick_ms", a->tick_ms);
        cJSON_AddItemToArray(list, o);
    }
    char *s = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    if (s) {
        fc_port_kv_set(KV_APPS, s, strlen(s));
        free(s);
    }
}

static app_t *find(const char *name)
{
    for (int i = 0; i < MAX_APPS; i++) {
        if (s_apps[i].used && strcmp(s_apps[i].name, name) == 0) {
            return &s_apps[i];
        }
    }
    return NULL;
}

static app_t *alloc_slot(void)
{
    for (int i = 0; i < MAX_APPS; i++) {
        if (!s_apps[i].used) {
            memset(&s_apps[i], 0, sizeof(s_apps[i]));
            return &s_apps[i];
        }
    }
    return NULL;
}

int fc_apps_init(const void *natives, int n_natives)
{
    if (s_inited) {
        return 0;
    }
    RuntimeInitArgs init;
    memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) {
        fc_port_log(FC_LOG_ERROR, "WAMR init failed");
        return -1;
    }
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
    wasm_runtime_register_natives("env", s_core_natives, sizeof(s_core_natives) / sizeof(s_core_natives[0]));
    if (natives != NULL && n_natives > 0) {
        wasm_runtime_register_natives("env", (NativeSymbol *)natives, (uint32_t)n_natives);
    }
    s_inited = 1;

    char *saved = NULL;
    size_t len = 0;
    if (fc_port_kv_get(KV_APPS, &saved, &len) != 0) {
        return 0;
    }
    cJSON *list = cJSON_Parse(saved);
    free(saved);
    const cJSON *o;
    cJSON_ArrayForEach(o, list) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(o, "name"));
        app_t *a = name ? alloc_slot() : NULL;
        if (a == NULL) {
            continue;
        }
        char key[64];
        wasm_key(name, key, sizeof(key));
        char *code = NULL;
        if (fc_port_kv_get(key, &code, &a->wasm_len) != 0) {
            continue;
        }
        a->used = 1;
        a->wasm = (unsigned char *)code;
        snprintf(a->name, sizeof(a->name), "%s", name);
        snprintf(a->image, sizeof(a->image), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "image")));
        snprintf(a->digest, sizeof(a->digest), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "digest")));
        a->tick_ms = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "tick_ms"));
        fc_port_log(FC_LOG_INFO, "starting saved app %s (%s)", a->name, a->image);
        app_start(a);
    }
    cJSON_Delete(list);
    return 0;
}

/* ---- reconcile ------------------------------------------------------------- */

static int tick_ms_of(const cJSON *app)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(app, "envVars"), "TICK_MS"));
    int ms = v ? atoi(v) : 100;
    return ms < 10 ? 10 : ms > 60000 ? 60000 : ms;
}

/* Downloads and checks a module; on success *out is malloc'd. */
static int fetch_module(const char *image, unsigned char **out, size_t *len, char *digest, size_t dlen,
                        char *err, size_t err_len)
{
    if (fc_oci_fetch_layer(image, "wasm", LAYER_WASM, MAX_WASM_SIZE, out, len, digest, dlen, err, err_len) != 0) {
        return -1;
    }
    /* Reject what WAMR cannot load before replacing a working app. */
    char ebuf[96];
    unsigned char *copy = malloc(*len);
    wasm_module_t m = copy ? (memcpy(copy, *out, *len), wasm_runtime_load(copy, (uint32_t)*len, ebuf, sizeof(ebuf)))
                           : NULL;
    if (m == NULL) {
        snprintf(err, err_len, "%s: not a loadable WASM module: %s", image, copy ? ebuf : "out of memory");
        free(copy);
        free(*out);
        *out = NULL;
        return 1; /* permanent */
    }
    wasm_runtime_unload(m);
    free(copy);
    return 0;
}

int fc_apps_apply(const cJSON *applications, char *err, size_t err_len, int *retry)
{
    *retry = 0;
    if (!s_inited) {
        snprintf(err, err_len, "WASM runtime not available");
        return -1;
    }
    int rc = 0, changed = 0;

    /* Stop apps that are no longer in the spec. */
    for (int i = 0; i < MAX_APPS; i++) {
        app_t *a = &s_apps[i];
        int keep = 0;
        const cJSON *app;
        cJSON_ArrayForEach(app, applications) {
            const char *n = cJSON_GetStringValue(cJSON_GetObjectItem(app, "name"));
            keep |= a->used && n && strcmp(n, a->name) == 0;
        }
        if (a->used && !keep) {
            fc_port_log(FC_LOG_INFO, "removing app %s", a->name);
            app_stop(a);
            char key[64];
            wasm_key(a->name, key, sizeof(key));
            fc_port_kv_del(key);
            free(a->wasm);
            pthread_mutex_lock(&s_lock);
            a->used = 0;
            pthread_mutex_unlock(&s_lock);
            changed = 1;
        }
    }

    const cJSON *app;
    cJSON_ArrayForEach(app, applications) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(app, "name"));
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(app, "appType"));
        const char *image = cJSON_GetStringValue(cJSON_GetObjectItem(app, "image"));
        if (name == NULL || type == NULL || strcmp(type, "container") != 0 || image == NULL) {
            snprintf(err, err_len, "app %s: only container apps with a WASM image are supported",
                     name ? name : "?");
            rc = -1;
            continue;
        }
        app_t *a = find(name);
        int tick = tick_ms_of(app);
        if (a != NULL && strcmp(a->image, image) == 0) {
            if (a->tick_ms != tick) {
                a->tick_ms = tick; /* picked up on the next tick */
                changed = 1;
            }
            continue;
        }
        unsigned char *code = NULL;
        size_t len = 0;
        char digest[72], ferr[160];
        int f = fetch_module(image, &code, &len, digest, sizeof(digest), ferr, sizeof(ferr));
        if (f != 0) {
            snprintf(err, err_len, "app %s: %s", name, ferr);
            *retry |= f < 0;
            rc = -1;
            continue; /* the running version, if any, keeps running */
        }
        if (a == NULL && (a = alloc_slot()) == NULL) {
            snprintf(err, err_len, "app %s: too many apps (max %d)", name, MAX_APPS);
            free(code);
            rc = -1;
            continue;
        }
        if (a->used) {
            fc_port_log(FC_LOG_INFO, "updating app %s: %s -> %s", name, a->image, image);
            app_stop(a);
            free(a->wasm);
        } else {
            fc_port_log(FC_LOG_INFO, "installing app %s (%s, %u bytes)", name, image, (unsigned)len);
        }
        char key[64];
        wasm_key(name, key, sizeof(key));
        fc_port_kv_set(key, code, len);
        pthread_mutex_lock(&s_lock);
        a->used = 1;
        snprintf(a->name, sizeof(a->name), "%s", name);
        snprintf(a->image, sizeof(a->image), "%s", image);
        snprintf(a->digest, sizeof(a->digest), "%s", digest);
        a->state[0] = '\0';
        a->restarts = 0;
        pthread_mutex_unlock(&s_lock);
        a->wasm = code;
        a->wasm_len = len;
        a->tick_ms = tick;
        app_start(a);
        changed = 1;
    }
    if (changed) {
        save_list();
    }
    return rc;
}

void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info)
{
    int n = 0, running = 0, errors = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_APPS; i++) {
        app_t *a = &s_apps[i];
        if (!a->used) {
            continue;
        }
        n++;
        int up = strcmp(a->status, "Running") == 0;
        running += up;
        errors += strcmp(a->status, "Error") == 0;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", a->name);
        cJSON_AddStringToObject(o, "appType", "container");
        cJSON_AddStringToObject(o, "status", a->status);
        cJSON_AddStringToObject(o, "ready", up ? "1/1" : "0/1");
        cJSON_AddNumberToObject(o, "restarts", a->restarts);
        cJSON_AddBoolToObject(o, "embedded", 0);
        cJSON_AddItemToArray(apps, o);
        char key[64];
        snprintf(key, sizeof(key), "app.%s", a->name);
        cJSON_AddStringToObject(custom_info, key, a->error[0] ? a->error : a->state[0] ? a->state : a->status);
    }
    pthread_mutex_unlock(&s_lock);
    const char *st = n == 0 ? "NoApplications" : errors ? (running ? "Degraded" : "Error")
                                                        : running == n ? "Healthy" : "Degraded";
    cJSON_AddStringToObject(summary, "status", st);
    if (n == 0) {
        cJSON_AddStringToObject(summary, "info", "No applications");
    }
}
