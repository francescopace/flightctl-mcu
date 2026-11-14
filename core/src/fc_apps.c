/*
 * WASM application manager.
 *
 * Flight Control delivers apps as spec.applications entries of appType
 * "container"; on an MCU the image must be a WASM OCI artifact (layer
 * application/vnd.wasm.content.layer.v1+wasm, directly or through an index
 * entry with platform.architecture "wasm"). The module is kept in the port
 * key/value store, so apps restart after a reboot without the network.
 *
 * The app interface is wasm-apps/sdk/fc_app.h (docs/APPS.md): the module
 * imports from "fc" (fc_appapi.c), exports fc_abi_version() and any of
 * init(), tick() and on_event(type, handle, value), and reaches the hardware
 * of the board map (fc_board.c) the fleet grants it in FC_ALLOW. Timers and
 * events are run here, on the app's thread. A module built for the earlier
 * interface (imports from "env") is refused when downloaded. A trap stops the app;
 * it is restarted after a back-off and counted in
 * status.applications[].restarts. So does a call that runs past its
 * instruction budget, so an app in an endless loop can always be stopped.
 */
#include "fc_apps.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fc_agent.h"
#include "fc_apps_int.h"
#include "fc_oci.h"
#include "fc_port.h"
#include "wasm_export.h"

#define FC_APP_ABI_VERSION 1 /* wasm-apps/sdk/fc_app.h */
#define MAX_WASM_SIZE   (64 * 1024)
#define WASM_STACK      2048 /* WASM operand/frame stack per app */
#if defined(__ZEPHYR__) && defined(CONFIG_ARCH_POSIX)
#define THREAD_STACK    (64 * 1024) /* a 64-bit host's frames, and Zephyr gives a thread what it asks */
#elif defined(ESP_PLATFORM) || defined(__ZEPHYR__)
#define THREAD_STACK    8192
#else
#define THREAD_STACK    (256 * 1024) /* a host: its threads cannot have less than 16 KB or so */
#endif
#define RESTART_DELAY_S 5
/* Instructions one call into an app may run (init, tick, on_event) before it
 * traps, so that an app in an endless loop can always be stopped. Counted by
 * WAMR's instruction metering, where the port builds WAMR with it; host
 * calls (a sleep, a bus transfer) do not count, and check the time themselves.
 * Provisional: to be measured on the slowest chip (about a second there). */
#ifndef FC_APP_CALL_BUDGET
#define FC_APP_CALL_BUDGET 20000000
#endif
#if WASM_ENABLE_INSTRUCTION_METERING
static int s_call_budget = FC_APP_CALL_BUDGET;
#endif
#define LAYER_WASM      "application/vnd.wasm.content.layer.v1+wasm"
#define KV_APPS         "apps"
#define APP_CONFIG_DIR  "/etc/fc-app/"

static app_t s_apps[MAX_APPS];
static pthread_mutex_t s_ev_lock[MAX_APPS];
static pthread_cond_t s_ev_cond[MAX_APPS];
const fc_hw_t *g_fc_hw;
static char s_device[96];

int fc_apps_stopping(const app_t *a)
{
    return __atomic_load_n(&a->stop, __ATOMIC_ACQUIRE);
}

static int stopping(const app_t *a)
{
    return fc_apps_stopping(a);
}

static void set_stop(app_t *a, int v)
{
    __atomic_store_n(&a->stop, v, __ATOMIC_RELEASE);
}
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

void fc_apps_set_hw(const fc_hw_t *hw)
{
    g_fc_hw = hw;
}

void fc_apps_set_device(const char *name)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_device, sizeof(s_device), "%s", name ? name : "");
    pthread_mutex_unlock(&s_lock);
}

const char *fc_apps_device_name(void)
{
    pthread_mutex_lock(&s_lock);
    const char *d = s_device[0] ? s_device : NULL; /* set once, before or while apps run */
    pthread_mutex_unlock(&s_lock);
    return d;
}

int64_t fc_apps_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- host functions provided by the core -------------------------------- */

void fc_apps_set_state_native(wasm_exec_env_t env, const char *s, int32_t len)
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

/* ---- events ------------------------------------------------------------------ */

void fc_apps_post(app_t *a, int32_t type, int32_t handle, int32_t value)
{
    pthread_mutex_lock(a->ev_lock);
    if (!a->accepting) {
        pthread_mutex_unlock(a->ev_lock);
        return;
    }
    /* An edge replaces the one still queued for the same GPIO. */
    for (int k = 0; type == EV_GPIO && k < a->ev_count; k++) {
        app_event_t *e = &a->ev[(a->ev_head + k) % APP_EVENTS];
        if (e->type == EV_GPIO && e->handle == handle) {
            e->value = value;
            pthread_mutex_unlock(a->ev_lock);
            return;
        }
    }
    if (a->ev_count == APP_EVENTS) {
        a->ev_head = (a->ev_head + 1) % APP_EVENTS;
        a->ev_count--;
        a->ev_lost++;
    }
    a->ev[(a->ev_head + a->ev_count) % APP_EVENTS] = (app_event_t){type, handle, value};
    a->ev_count++;
    pthread_cond_signal(a->ev_cond);
    pthread_mutex_unlock(a->ev_lock);
}

/* The next event: FC_EV_OVERFLOW first when some were dropped. */
static int next_event(app_t *a, app_event_t *e)
{
    int got = 1;
    pthread_mutex_lock(a->ev_lock);
    if (a->ev_lost > 0) {
        *e = (app_event_t){EV_OVERFLOW, -1, a->ev_lost};
        a->ev_lost = 0;
    } else if (a->ev_count > 0) {
        *e = a->ev[a->ev_head];
        a->ev_head = (a->ev_head + 1) % APP_EVENTS;
        a->ev_count--;
    } else {
        got = 0;
    }
    pthread_mutex_unlock(a->ev_lock);
    return got;
}

/* Waits for an event, a stop, or the monotonic time `until`, at most 100 ms
 * at a time so that a change of the wall clock (SNTP) does not matter. */
static void wait_event(app_t *a, int64_t until)
{
    int64_t ms = until - fc_apps_now_ms();
    if (ms <= 0) {
        return;
    }
    if (ms > 100) {
        ms = 100;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000);
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(a->ev_lock);
    if (a->ev_count == 0 && a->ev_lost == 0 && !stopping(a)) {
        pthread_cond_timedwait(a->ev_cond, a->ev_lock, &ts);
    }
    pthread_mutex_unlock(a->ev_lock);
}

static void wake(app_t *a)
{
    pthread_mutex_lock(a->ev_lock);
    pthread_cond_broadcast(a->ev_cond);
    pthread_mutex_unlock(a->ev_lock);
}

/* ---- runner -------------------------------------------------------------- */

/* Loads a module so that the .wasm buffer can be freed as soon as this
 * returns: WAMR copies the strings and data segments it needs and leaves the
 * buffer as it was (a plain wasm_runtime_load patches it and keeps pointing
 * into it), so the bytes can still be stored after this check. */
static wasm_module_t load_module(unsigned char *code, size_t len, char *ebuf, size_t elen)
{
    LoadArgs args;
    memset(&args, 0, sizeof(args));
    args.wasm_binary_freeable = true;
    return wasm_runtime_load_ex(code, (uint32_t)len, &args, ebuf, (uint32_t)elen);
}

/* A function export's signature, as "ii>i" (params > results), or NULL. */
static const char *export_sig(wasm_module_t m, const char *name, char *buf, size_t len)
{
    int32_t n = wasm_runtime_get_export_count(m);
    for (int32_t i = 0; i < n; i++) {
        wasm_export_t ex;
        wasm_runtime_get_export_type(m, i, &ex);
        if (ex.kind != WASM_IMPORT_EXPORT_KIND_FUNC || strcmp(ex.name, name) != 0) {
            continue;
        }
        size_t k = 0;
        uint32_t np = wasm_func_type_get_param_count(ex.u.func_type);
        uint32_t nr = wasm_func_type_get_result_count(ex.u.func_type);
        for (uint32_t p = 0; p < np && k + 2 < len; p++) {
            wasm_valkind_t v = wasm_func_type_get_param_valkind(ex.u.func_type, p);
            buf[k++] = v == WASM_I32 ? 'i' : v == WASM_I64 ? 'I' : v == WASM_F32 ? 'f' : v == WASM_F64 ? 'F' : '?';
        }
        buf[k++] = '>';
        for (uint32_t r = 0; r < nr && k + 1 < len; r++) {
            wasm_valkind_t v = wasm_func_type_get_result_valkind(ex.u.func_type, r);
            buf[k++] = v == WASM_I32 ? 'i' : v == WASM_I64 ? 'I' : v == WASM_F32 ? 'f' : v == WASM_F64 ? 'F' : '?';
        }
        buf[k] = '\0';
        return buf;
    }
    return NULL;
}

/* Whether this firmware can run a module: 0, or -1 with the reason. */
static int check_module(wasm_module_t m, char *err, size_t err_len)
{
    int32_t n = wasm_runtime_get_import_count(m);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t im;
        wasm_runtime_get_import_type(m, i, &im);
        if (strcmp(im.module_name, "env") == 0) {
            snprintf(err, err_len, "built for the earlier app interface (imports from \"env\"): rebuild with fc_app.h");
            return -1;
        }
        if (strcmp(im.module_name, "fc") != 0) {
            snprintf(err, err_len, "imports from module \"%.30s\", which the device does not have", im.module_name);
            return -1;
        }
        if (im.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !im.linked) {
            snprintf(err, err_len, "imports fc.%.40s, which this firmware does not have", im.name);
            return -1;
        }
    }
    char sig[16];
    static const struct {
        const char *name, *sig;
    } want[] = {{"init", ">i"}, {"tick", ">"}, {"on_event", "iii>"}, {"fc_abi_version", ">i"}};
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        const char *s = export_sig(m, want[i].name, sig, sizeof(sig));
        if (s != NULL && strcmp(s, want[i].sig) != 0) {
            snprintf(err, err_len, "%s() has the wrong signature (rebuild with fc_app.h)", want[i].name);
            return -1;
        }
    }
    if (export_sig(m, "tick", sig, sizeof(sig)) == NULL && export_sig(m, "on_event", sig, sizeof(sig)) == NULL) {
        snprintf(err, err_len, "exports neither tick() nor on_event()");
        return -1;
    }
    return 0;
}

/* One call into the app, within its instruction budget. 0: it trapped. */
static int call_app(wasm_exec_env_t env, wasm_function_inst_t f, uint32_t argc, uint32_t *argv)
{
#if WASM_ENABLE_INSTRUCTION_METERING
    wasm_runtime_set_instruction_count_limit(env, s_call_budget);
#endif
    return wasm_runtime_call_wasm(env, f, argc, argv);
}

/* One call into the app: 0, or -1 when it trapped (status set). */
static int call_fc(app_t *a, wasm_exec_env_t env, wasm_function_inst_t f, uint32_t argc, uint32_t *argv,
                   const char *what)
{
    a->call_start_ms = fc_apps_now_ms();
    if (call_app(env, f, argc, argv)) {
        return 0;
    }
    if (!stopping(a)) {
        const char *ex = wasm_runtime_get_exception(a->inst);
        fc_port_log(FC_LOG_WARN, "app %s trapped in %s: %s", a->name, what, ex ? ex : "?");
        set_status(a, "Error", ex);
    }
    return -1;
}

/* init(), then tick() every TICK_MS, timers and events through on_event(),
 * until the app stops or traps. */
static void run_fc(app_t *a, wasm_module_inst_t inst, wasm_exec_env_t env)
{
    wasm_function_inst_t init = wasm_runtime_lookup_function(inst, "init");
    wasm_function_inst_t tick = wasm_runtime_lookup_function(inst, "tick");
    wasm_function_inst_t on_event = wasm_runtime_lookup_function(inst, "on_event");
    a->inst = inst;
    pthread_mutex_lock(a->ev_lock);
    a->ev_head = a->ev_count = 0;
    a->ev_lost = 0;
    a->accepting = 1;
    pthread_mutex_unlock(a->ev_lock);
    fc_appapi_load_config(a);

    int ok = 1;
    if (init != NULL) {
        uint32_t argv[1] = {0};
        ok = call_fc(a, env, init, 0, argv, "init") == 0;
        if (ok && (int32_t)argv[0] != 0) {
            char why[48];
            snprintf(why, sizeof(why), "init() returned %d", (int)(int32_t)argv[0]);
            fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, why);
            set_status(a, "Error", why);
            ok = 0;
        }
    }
    if (ok) {
        set_status(a, "Running", NULL);
        fc_port_log(FC_LOG_INFO, "app %s running (%s)", a->name, a->image);
    }
    int64_t next_tick = fc_apps_now_ms() + a->tick_ms;
    while (ok && !stopping(a)) {
        int64_t now = fc_apps_now_ms();
        if (tick != NULL && now >= next_tick) {
            uint32_t argv[1] = {0};
            ok = call_fc(a, env, tick, 0, argv, "tick") == 0;
            next_tick += a->tick_ms;
            if (next_tick <= now) {
                next_tick = now + a->tick_ms; /* late: no catching up */
            }
        }
        int64_t until = now + 100;
        for (int i = 0; ok && i < APP_HANDLES; i++) {
            app_handle_t *h = &a->h[i];
            if (h->kind != H_TIMER || h->fired) {
                continue;
            }
            if (h->due_ms <= now) {
                if (h->periodic) {
                    h->due_ms += h->period_ms;
                    if (h->due_ms <= now) {
                        h->due_ms = now + h->period_ms;
                    }
                } else {
                    h->fired = 1;
                }
                if (on_event != NULL) {
                    uint32_t argv[3] = {EV_TIMER, (uint32_t)i, 0};
                    ok = call_fc(a, env, on_event, 3, argv, "on_event") == 0;
                }
            }
            if (h->kind == H_TIMER && !h->fired && h->due_ms < until) {
                until = h->due_ms;
            }
        }
        app_event_t e;
        while (ok && !stopping(a) && next_event(a, &e)) {
            if (e.type == EV_CONFIG) {
                fc_appapi_load_config(a);
            }
            if (on_event != NULL) {
                uint32_t argv[3] = {(uint32_t)e.type, (uint32_t)e.handle, (uint32_t)e.value};
                ok = call_fc(a, env, on_event, 3, argv, "on_event") == 0;
            }
        }
        if (tick != NULL && next_tick < until) {
            until = next_tick;
        }
        if (ok) {
            wait_event(a, until);
        }
    }
    pthread_mutex_lock(a->ev_lock);
    a->accepting = 0;
    a->ev_head = a->ev_count = 0;
    a->ev_lost = 0;
    pthread_mutex_unlock(a->ev_lock);
    fc_appapi_close_all(a);
    a->inst = NULL;
}

static void *runner(void *arg)
{
    app_t *a = arg;
    char ebuf[128];
    wasm_runtime_init_thread_env();
    wasm_module_t module = a->module;
    while (!stopping(a)) {
        wasm_module_inst_t inst = wasm_runtime_instantiate(module, WASM_STACK, 0, ebuf, sizeof(ebuf));
        wasm_exec_env_t env = inst ? wasm_runtime_create_exec_env(inst, WASM_STACK) : NULL;
        if (inst == NULL || env == NULL) {
            const char *why = inst == NULL ? ebuf : "exec env: out of memory";
            fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, why);
            set_status(a, "Error", why);
        } else {
            wasm_runtime_set_user_data(env, a);
            wasm_function_inst_t abi = wasm_runtime_lookup_function(inst, "fc_abi_version");
            uint32_t abi_argv[1] = {0};
            if (abi != NULL && call_app(env, abi, 0, abi_argv) && abi_argv[0] > FC_APP_ABI_VERSION) {
                snprintf(ebuf, sizeof(ebuf), "app needs interface v%u, this device has v%d", (unsigned)abi_argv[0],
                         FC_APP_ABI_VERSION);
                fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, ebuf);
                set_status(a, "Error", ebuf);
                wasm_runtime_destroy_exec_env(env);
                wasm_runtime_deinstantiate(inst);
                goto done;
            }
            run_fc(a, inst, env);
        }
        if (env) {
            wasm_runtime_destroy_exec_env(env);
        }
        if (inst) {
            wasm_runtime_deinstantiate(inst);
        }
        if (!stopping(a)) {
            for (int i = 0; i < RESTART_DELAY_S * 10 && !stopping(a); i++) {
                usleep(100 * 1000);
            }
            if (!stopping(a)) {
                pthread_mutex_lock(&s_lock);
                a->restarts++;
                pthread_mutex_unlock(&s_lock);
            }
        }
    }
done:
    wasm_runtime_destroy_thread_env();
    return NULL;
}

static void app_start(app_t *a)
{
    if (a->module == NULL) {
        return; /* the status already says why */
    }
    set_stop(a, 0);
    set_status(a, "Preparing", NULL);
    pthread_attr_init(&a->thread_attr);
    a->thread_running = pthread_attr_setstacksize(&a->thread_attr, THREAD_STACK) == 0 &&
                        pthread_create(&a->thread, &a->thread_attr, runner, a) == 0;
    if (!a->thread_running) {
        pthread_attr_destroy(&a->thread_attr);
        set_status(a, "Error", "cannot start thread");
    }
}

static void app_stop(app_t *a)
{
    if (a->thread_running) {
        set_stop(a, 1);
        wake(a);
        pthread_join(a->thread, NULL);
        pthread_attr_destroy(&a->thread_attr);
        a->thread_running = 0;
    }
    set_status(a, "Stopped", NULL);
}

/* Stops the app and unloads its module (after the runner is joined). */
static void app_release(app_t *a)
{
    app_stop(a);
    if (a->module != NULL) {
        wasm_runtime_unload(a->module);
        a->module = NULL;
    }
}

/* ---- env vars ---------------------------------------------------------------- */

/* Takes the app's envVars (JSON) and what the device reads in them. */
static void set_env(app_t *a, const char *json)
{
    free(a->env_json);
    a->env_json = strdup(json ? json : "{}");
    a->allow[0] = '\0';
    cJSON *o = cJSON_Parse(a->env_json);
    const char *allow = cJSON_GetStringValue(cJSON_GetObjectItem(o, "FC_ALLOW"));
    snprintf(a->allow, sizeof(a->allow), "%s", allow ? allow : "");
    cJSON_Delete(o);
}

static char *env_of(const cJSON *app)
{
    const cJSON *e = cJSON_GetObjectItem(app, "envVars");
    return cJSON_IsObject(e) ? cJSON_PrintUnformatted(e) : strdup("{}");
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
        /* Apps start before the device reaches the server: what they may
         * open must be known without the spec. */
        cJSON_AddStringToObject(o, "env", a->env_json ? a->env_json : "{}");
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

/* A clean slot; its event lock and condition stay the slot's. */
static void reset_slot(int i)
{
    app_t *a = &s_apps[i];
    free(a->env_json);
    memset(a, 0, sizeof(*a));
    a->slot = i;
    a->ev_lock = &s_ev_lock[i];
    a->ev_cond = &s_ev_cond[i];
}

static app_t *alloc_slot(void)
{
    for (int i = 0; i < MAX_APPS; i++) {
        if (!s_apps[i].used) {
            reset_slot(i);
            return &s_apps[i];
        }
    }
    return NULL;
}

/* Loads a module and checks it is an app this firmware runs. */
static wasm_module_t load_app(unsigned char *code, size_t len, char *ebuf, size_t elen)
{
    wasm_module_t m = load_module(code, len, ebuf, elen);
    if (m != NULL && check_module(m, ebuf, elen) != 0) {
        wasm_runtime_unload(m);
        m = NULL;
    }
    return m;
}

int fc_apps_init(void)
{
    if (s_inited) {
        return 0;
    }
    static int sync_inited;
    if (!sync_inited) {
        for (int i = 0; i < MAX_APPS; i++) {
            pthread_mutex_init(&s_ev_lock[i], NULL);
            pthread_cond_init(&s_ev_cond[i], NULL);
        }
        sync_inited = 1;
    }
    for (int i = 0; i < MAX_APPS; i++) {
        s_apps[i].slot = i;
        s_apps[i].ev_lock = &s_ev_lock[i];
        s_apps[i].ev_cond = &s_ev_cond[i];
    }
    RuntimeInitArgs init;
    memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) {
        fc_port_log(FC_LOG_ERROR, "WAMR init failed");
        return -1;
    }
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
    int n_fc = 0;
    NativeSymbol *fc = fc_appapi_natives(&n_fc);
    wasm_runtime_register_natives("fc", fc, (uint32_t)n_fc);
    fc_board_load(g_fc_hw);
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
        size_t code_len = 0;
        if (fc_port_kv_get(key, &code, &code_len) != 0) {
            continue;
        }
        char ebuf[96];
        a->module = load_app((unsigned char *)code, code_len, ebuf, sizeof(ebuf));
        free(code);
        a->used = 1;
        snprintf(a->name, sizeof(a->name), "%s", name);
        snprintf(a->image, sizeof(a->image), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "image")));
        snprintf(a->digest, sizeof(a->digest), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "digest")));
        a->tick_ms = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "tick_ms"));
        set_env(a, cJSON_GetStringValue(cJSON_GetObjectItem(o, "env")));
        if (a->module == NULL) {
            /* Shown in the status; the next spec downloads the image again. */
            fc_port_log(FC_LOG_WARN, "app %s: cannot load the saved module: %s", a->name, ebuf);
            set_status(a, "Error", ebuf);
            continue;
        }
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

/* Downloads and loads a module: on success *out is the malloc'd .wasm (to be
 * stored, then freed) and *module the loaded module, which is what runs. A
 * module WAMR cannot load, or that this firmware cannot run, is rejected
 * before a working app is replaced. */
static int fetch_module(const char *image, unsigned char **out, size_t *len, wasm_module_t *module, char *digest,
                        size_t dlen, char *err, size_t err_len)
{
    if (fc_oci_fetch_layer(image, "wasm", LAYER_WASM, MAX_WASM_SIZE, out, len, digest, dlen, err, err_len) != 0) {
        return -1;
    }
    char ebuf[96];
    *module = load_app(*out, *len, ebuf, sizeof(ebuf));
    if (*module == NULL) {
        snprintf(err, err_len, "%s: not a loadable WASM module: %s", image, ebuf);
        free(*out);
        *out = NULL;
        return 1; /* permanent */
    }
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
            app_release(a);
            char key[64];
            wasm_key(a->name, key, sizeof(key));
            fc_port_kv_del(key);
            fc_appapi_kv_key(a->name, key, sizeof(key));
            fc_port_kv_del(key);
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
        char *env = env_of(app);
        /* Same image: nothing to do, unless its saved module failed to load
         * at boot, in which case it is downloaded again. */
        if (a != NULL && strcmp(a->image, image) == 0 && a->module != NULL) {
            if (env != NULL && strcmp(env, a->env_json ? a->env_json : "{}") != 0) {
                /* What it may open, or how often it ticks: start again. */
                fc_port_log(FC_LOG_INFO, "app %s: new env vars, restarting it", a->name);
                app_stop(a);
                set_env(a, env);
                a->tick_ms = tick;
                app_start(a);
                changed = 1;
            }
            free(env);
            continue;
        }
        unsigned char *code = NULL;
        size_t len = 0;
        wasm_module_t module = NULL;
        char digest[72], ferr[160];
        int f = fetch_module(image, &code, &len, &module, digest, sizeof(digest), ferr, sizeof(ferr));
        if (f != 0) {
            snprintf(err, err_len, "app %s: %s", name, ferr);
            *retry |= f < 0;
            rc = -1;
            free(env);
            continue; /* the running version, if any, keeps running */
        }
        if (a == NULL && (a = alloc_slot()) == NULL) {
            snprintf(err, err_len, "app %s: too many apps (max %d)", name, MAX_APPS);
            wasm_runtime_unload(module);
            free(code);
            free(env);
            rc = -1;
            continue;
        }
        /* Stored before the running version is stopped: an app that cannot be
         * stored would run until the next reboot and then be gone. A failed
         * write leaves the old module in place on both ports. */
        char key[64];
        wasm_key(name, key, sizeof(key));
        int stored = fc_port_kv_set(key, code, len) == 0;
        free(code); /* from here on only the loaded module is kept */
        if (!stored) {
            snprintf(err, err_len, "app %s: cannot store the module (%u bytes)", name, (unsigned)len);
            fc_port_log(FC_LOG_WARN, "%s", err);
            wasm_runtime_unload(module);
            free(env);
            *retry = 1;
            rc = -1;
            continue; /* the running version, if any, keeps running */
        }
        if (a->used) {
            fc_port_log(FC_LOG_INFO, "updating app %s: %s -> %s", name, a->image, image);
            app_release(a);
        } else {
            fc_port_log(FC_LOG_INFO, "installing app %s (%s, %u bytes)", name, image, (unsigned)len);
        }
        pthread_mutex_lock(&s_lock);
        a->used = 1;
        snprintf(a->name, sizeof(a->name), "%s", name);
        snprintf(a->image, sizeof(a->image), "%s", image);
        snprintf(a->digest, sizeof(a->digest), "%s", digest);
        a->state[0] = '\0';
        a->restarts = 0;
        pthread_mutex_unlock(&s_lock);
        a->module = module;
        a->tick_ms = tick;
        set_env(a, env);
        free(env);
        app_start(a);
        changed = 1;
    }
    if (changed) {
        save_list();
    }
    return rc;
}

/* ---- config files ------------------------------------------------------------ */

int fc_apps_check_file(const char *path, const char *data, char *err, size_t err_len)
{
    if (strcmp(path, FC_BOARD_PATH) == 0) {
        return fc_board_check(g_fc_hw, data, err, err_len);
    }
    return 0;
}

void fc_apps_file_changed(const char *path)
{
    if (!s_inited) {
        return;
    }
    if (strcmp(path, FC_BOARD_PATH) == 0) {
        /* The map changes only while no app runs. */
        int restart[MAX_APPS] = {0};
        for (int i = 0; i < MAX_APPS; i++) {
            app_t *a = &s_apps[i];
            if (a->used && a->thread_running) {
                app_stop(a);
                restart[i] = 1;
            }
        }
        fc_board_load(g_fc_hw);
        fc_port_log(FC_LOG_INFO, "new board map%s", restart[0] || restart[1] ? ": restarting the apps" : "");
        for (int i = 0; i < MAX_APPS; i++) {
            if (restart[i]) {
                app_start(&s_apps[i]);
            }
        }
        return;
    }
    size_t dl = strlen(APP_CONFIG_DIR), pl = strlen(path);
    if (pl > dl + 5 && strncmp(path, APP_CONFIG_DIR, dl) == 0 && strcmp(path + pl - 5, ".json") == 0) {
        char name[48];
        snprintf(name, sizeof(name), "%.*s", (int)(pl - dl - 5), path + dl);
        app_t *a = find(name);
        if (a != NULL) {
            fc_apps_post(a, EV_CONFIG, -1, 0);
        }
    }
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
