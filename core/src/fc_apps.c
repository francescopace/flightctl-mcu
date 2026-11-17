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
 * interface (its functions imported from "env") is refused when downloaded. A trap stops the app;
 * it is restarted after a back-off and counted in
 * status.applications[].restarts. So does a call that runs past its
 * instruction budget, so an app in an endless loop can always be stopped.
 *
 * A module built for no device in particular (a benchmark such as CoreMark)
 * runs too: the export its env var FC_ENTRY names, or _start() in a module
 * with neither tick() nor on_event(), is called once, with no instruction
 * budget, and the app is Completed when it returns, its result in its state.
 * It may import env.clock_ms. Removing or updating it stops the call
 * (wasm_runtime_set_instruction_count_limit(0) from another thread, which
 * third_party/patches/wamr-metering-stop.patch makes WAMR check at branches
 * and calls).
 */
#include "fc_apps.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fc_agent.h"
#include "fc_apps_int.h"
#include "fc_metrics.h"
#include "fc_heap_hold.h"
#include "fc_oci.h"
#include "fc_port.h"
#include "wasm_export.h"

#define FC_APP_ABI_VERSION 1 /* wasm-apps/sdk/fc_app.h */
#define MAX_WASM_SIZE   (64 * 1024) /* at most; less as the heap has less (wasm_limit) */
/* What loading a module takes, in times its .wasm: the module as WAMR's fast
 * interpreter keeps it (2.5 to 3.5 times), and the .wasm while it loads. */
#define LOAD_FACTOR     4
#define WASM_STACK      2048 /* WASM operand/frame stack per app */
#define APP_MADE_WAIT_MS 5000 /* app_start waits this long at most for the instance */
#if defined(__ZEPHYR__) && defined(CONFIG_ARCH_POSIX)
#define THREAD_STACK    (64 * 1024) /* a 64-bit host's frames, and Zephyr gives a thread what it asks */
#elif defined(ESP_PLATFORM) || defined(__ZEPHYR__)
#define THREAD_STACK    8192
#else
#define THREAD_STACK    (256 * 1024) /* a host: its threads cannot have less than 16 KB or so */
#endif
/* The heap an app's memory must leave the agent (fc_port_heap_free, once
 * the instance is made, the agent's connection closed): what it takes to
 * open its TLS connection again and to parse and apply a spec. Measured on
 * the boards (docs/APPS.md). With less, on a C3 running CoreMark, a TLS
 * handshake failed for want of memory and the heap went down to 108 bytes. */
#ifndef FC_APP_HEAP_FLOOR
#define FC_APP_HEAP_FLOOR (32 * 1024)
#endif
static long s_heap_floor = FC_APP_HEAP_FLOOR;
/* What pthread_create takes beside the stack (ESP-IDF: the task's control
 * block, ~350 bytes), and the heap's headers. */
#define THREAD_SLACK    1024
#define RESTART_DELAY_S 5
/* A module run once starts when the agent has had nothing on hand for
 * ENTRY_QUIET_MS, at most ENTRY_WAIT_MS after it was started (see
 * wait_quiet). */
#ifndef ENTRY_WAIT_MS
#define ENTRY_WAIT_MS 30000
#endif
#define ENTRY_QUIET_MS 2000
static int64_t s_entry_wait_ms = ENTRY_WAIT_MS;
/* Instructions one call into an app may run (init, tick, on_event) before it
 * traps, so that an app in an endless loop can always be stopped. Counted by
 * WAMR's instruction metering, where the port builds WAMR with it; host
 * calls (a sleep, a bus transfer, the C library) do not count: their time
 * has a limit of its own, APP_CALL_MS (fc_apps_native_time). 5 million last
 * 0.66 s on the C5 to 1.3 s on the C6, the slowest chip (v3.14-rc1). */
#ifndef FC_APP_CALL_BUDGET
#define FC_APP_CALL_BUDGET 5000000
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

int64_t fc_apps_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* What a call may spend in the device's functions: APP_CALL_MS, or less in
 * the unit tests. */
static int64_t s_native_limit_us = (int64_t)APP_CALL_MS * 1000;

int fc_apps_native_time(wasm_exec_env_t env, app_t *a, int64_t t0)
{
    if (a == NULL || a->unlimited) {
        return 0;
    }
    a->native_us += fc_apps_now_us() - t0;
    if (a->native_us <= s_native_limit_us) {
        return 0;
    }
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "a call spent more than 1 s in the device's functions");
    return 1;
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

/* ---- apps that crash the device as they start --------------------------------
 *
 * An app the device starts at boot (or a module it loads) that crashes the
 * firmware would restart it for good: the saved app starts again before the
 * network is up, and the device never reaches the server to be given another
 * spec. So the device keeps a record, KV_GUARD, of the apps in their first
 * GUARD_SETTLE_MS after they were started, and of the one whose module is
 * being loaded:
 *   {"fw": firmware, "loading": name, "apps": {name: {"image", "env", "n", "bad"}}}
 * At boot, after a restart that was a crash (fc_hw_t.restart_was_crash), the
 * app being loaded is counted, or, if none was, every app still in its first
 * GUARD_SETTLE_MS. An app counted GUARD_CRASHES times in a row is "bad": it
 * is not started again, and the status says why, until its image, its env
 * vars or the firmware change (another firmware forgets the record), or it
 * leaves the spec. A restart that was not a crash clears the counts. */
#define KV_GUARD        "apps.guard"
#define GUARD_CRASHES   2
#ifndef GUARD_SETTLE_MS
#define GUARD_SETTLE_MS 60000
#endif
#define GUARD_ERROR     "crashed the device as it started: not run again until its image, env or firmware change"
static int64_t s_settle_ms = GUARD_SETTLE_MS;
static pthread_mutex_t s_guard_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *firmware_now(void)
{
    return g_fc_hw && g_fc_hw->firmware ? g_fc_hw->firmware : "";
}

/* The record for this firmware: a fresh one if there is none or it was
 * written by another firmware. */
static cJSON *guard_load(void)
{
    char *s = NULL;
    size_t len = 0;
    cJSON *g = fc_port_kv_get(KV_GUARD, &s, &len) == 0 ? cJSON_Parse(s) : NULL;
    free(s);
    const char *fw = cJSON_GetStringValue(cJSON_GetObjectItem(g, "fw"));
    if (!cJSON_IsObject(g) || fw == NULL || strcmp(fw, firmware_now()) != 0) {
        cJSON_Delete(g);
        g = cJSON_CreateObject();
        cJSON_AddStringToObject(g, "fw", firmware_now());
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(g, "apps"))) {
        cJSON_DeleteItemFromObject(g, "apps");
        cJSON_AddObjectToObject(g, "apps");
    }
    return g;
}

static void guard_save(cJSON *g)
{
    const char *loading = cJSON_GetStringValue(cJSON_GetObjectItem(g, "loading"));
    if (cJSON_GetArraySize(cJSON_GetObjectItem(g, "apps")) == 0 && (loading == NULL || !loading[0])) {
        fc_port_kv_del(KV_GUARD);
        return;
    }
    char *s = cJSON_PrintUnformatted(g);
    if (s) {
        fc_port_kv_set(KV_GUARD, s, strlen(s));
        free(s);
    }
}

static int guard_same(const cJSON *e, const char *image, const char *env)
{
    const char *i = cJSON_GetStringValue(cJSON_GetObjectItem(e, "image"));
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(e, "env"));
    return i && v && strcmp(i, image) == 0 && strcmp(v, env ? env : "{}") == 0;
}

/* At boot, before any app starts: counts the restart against the apps that
 * were starting if it was a crash, forgets the counts if it was not. */
static void guard_boot(void)
{
    pthread_mutex_lock(&s_guard_lock);
    char *s = NULL;
    size_t len = 0;
    int had = fc_port_kv_get(KV_GUARD, &s, &len) == 0;
    free(s);
    cJSON *g = guard_load(); /* another firmware's record is dropped here */
    if (!had) {
        cJSON_Delete(g);
        pthread_mutex_unlock(&s_guard_lock);
        return;
    }
    int crash = g_fc_hw && g_fc_hw->restart_was_crash ? g_fc_hw->restart_was_crash() == 1 : 0;
    const char *ls = cJSON_GetStringValue(cJSON_GetObjectItem(g, "loading"));
    char loading[48];
    snprintf(loading, sizeof(loading), "%s", ls ? ls : "");
    cJSON *apps = cJSON_GetObjectItem(g, "apps");
    cJSON *e = apps ? apps->child : NULL;
    while (e != NULL) {
        cJSON *next = e->next;
        if (!cJSON_IsTrue(cJSON_GetObjectItem(e, "bad"))) {
            if (!crash) {
                cJSON_DeleteItemFromObject(apps, e->string);
            } else if (!loading[0] || strcmp(loading, e->string) == 0) {
                int n = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "n")) + 1;
                cJSON_DeleteItemFromObject(e, "n");
                cJSON_AddNumberToObject(e, "n", n);
                fc_port_log(FC_LOG_WARN, "app %s was starting when the device crashed (%d in a row)", e->string, n);
                if (n >= GUARD_CRASHES) {
                    cJSON_AddTrueToObject(e, "bad");
                    fc_port_log(FC_LOG_ERROR, "app %s: %s", e->string, GUARD_ERROR);
                }
            }
        }
        e = next;
    }
    cJSON_DeleteItemFromObject(g, "loading");
    guard_save(g);
    cJSON_Delete(g);
    pthread_mutex_unlock(&s_guard_lock);
}

/* 1 if this app, with this image and env, crashed the device as it started. */
static int guard_is_bad(const char *name, const char *image, const char *env)
{
    pthread_mutex_lock(&s_guard_lock);
    cJSON *g = guard_load();
    cJSON *e = cJSON_GetObjectItem(cJSON_GetObjectItem(g, "apps"), name);
    int bad = e && cJSON_IsTrue(cJSON_GetObjectItem(e, "bad")) && guard_same(e, image, env);
    cJSON_Delete(g);
    pthread_mutex_unlock(&s_guard_lock);
    return bad;
}

/* Before an app starts (loading: its module is about to be loaded): it is
 * in its first GUARD_SETTLE_MS from now. Its count goes on only for the same
 * image and env. */
static void guard_begin(const char *name, const char *image, const char *env, int loading)
{
    pthread_mutex_lock(&s_guard_lock);
    cJSON *g = guard_load();
    cJSON *apps = cJSON_GetObjectItem(g, "apps");
    cJSON *e = cJSON_GetObjectItem(apps, name);
    int n = e && guard_same(e, image, env) ? (int)cJSON_GetNumberValue(cJSON_GetObjectItem(e, "n")) : 0;
    cJSON_DeleteItemFromObject(apps, name);
    e = cJSON_AddObjectToObject(apps, name);
    cJSON_AddStringToObject(e, "image", image);
    cJSON_AddStringToObject(e, "env", env ? env : "{}");
    cJSON_AddNumberToObject(e, "n", n);
    cJSON_DeleteItemFromObject(g, "loading");
    if (loading) {
        cJSON_AddStringToObject(g, "loading", name);
    }
    guard_save(g);
    cJSON_Delete(g);
    pthread_mutex_unlock(&s_guard_lock);
}

/* The module was loaded, or not: no longer the one being loaded. */
static void guard_loaded(void)
{
    pthread_mutex_lock(&s_guard_lock);
    cJSON *g = guard_load();
    if (cJSON_GetObjectItem(g, "loading") != NULL) {
        cJSON_DeleteItemFromObject(g, "loading");
        guard_save(g);
    }
    cJSON_Delete(g);
    pthread_mutex_unlock(&s_guard_lock);
}

/* The app ran its first GUARD_SETTLE_MS, did not start after all, or left
 * the spec (all: forget it even if it was bad). */
static void guard_forget(const char *name, int all)
{
    pthread_mutex_lock(&s_guard_lock);
    cJSON *g = guard_load();
    cJSON *apps = cJSON_GetObjectItem(g, "apps");
    cJSON *e = cJSON_GetObjectItem(apps, name);
    if (e != NULL && (all || !cJSON_IsTrue(cJSON_GetObjectItem(e, "bad")))) {
        cJSON_DeleteItemFromObject(apps, name);
        guard_save(g);
    }
    cJSON_Delete(g);
    pthread_mutex_unlock(&s_guard_lock);
}

/* On the app's thread: once it has been up GUARD_SETTLE_MS, it is past the
 * start. */
static void guard_tick(app_t *a, int64_t now)
{
    if (!a->settled && now >= a->settle_at_ms) {
        a->settled = 1;
        guard_forget(a->name, 0);
    }
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

/* "env": the module wasm-ld and Emscripten import from by default. The device
 * has its C library there (below), and clock_ms, as CoreMark's builds import it, with an i32 or an i64
 * result: milliseconds since the app started. The earlier app interface put
 * its functions there too; those are named so the module can be rebuilt. */
static void env_clock_ms(wasm_exec_env_t env, uint64_t *args)
{
    app_t *a = wasm_runtime_get_user_data(env);
    /* Raw: the result goes in args[0]. An i32 result is its low half, so one
     * function serves both types. NULL: called while the module is
     * instantiated, before the app's context exists. */
    args[0] = (uint64_t)(fc_apps_now_ms() - (a ? a->started_ms : 0));
}

static const NativeSymbol s_env_natives[] = { /* const: see s_libc_natives */
    {"clock_ms", (void *)env_clock_ms, NULL, NULL}, /* checked in check_module */
};

/* A small C library, in "env" as clang's wasm32 build imports it
 * (wasm-apps/sdk/libc/): WAMR's libc-builtin, of which an app may import only
 * the functions named here (check_env_import). Not malloc and the rest: an
 * app has no heap in its linear memory. The device registers them itself
 * (s_libc_natives), WAMR none: each that does work counts its time against
 * the call's limit in the device's functions (LIBC_TIMED); printf, puts and putchar write to the
 * app's log, a line at a time, as fc_log does at INFO (WAMR's write to the
 * serial port only); strtol and strtoul take a NULL endptr (WAMR's would
 * write at the app's address 0); abort takes no argument, as C's; strncmp
 * and strncpy, atoi and ctype are the device's own (below). */
static const char *const s_libc[] = {
    "memcmp", "memcpy", "memmove", "memset", "memchr", "strchr", "strcmp", "strcpy", "strlen", "strncmp",
    "strncpy", "strncasecmp", "strspn", "strcspn", "strstr", "sprintf", "snprintf", "printf", "puts",
    "putchar", "atoi", "strtol", "strtoul", "isalnum", "isalpha", "isdigit", "isgraph", "isprint",
    "isspace", "isupper", "isxdigit", "tolower", "toupper", "abort",
};

static int libc_function(const char *name)
{
    for (size_t i = 0; i < sizeof(s_libc) / sizeof(s_libc[0]); i++) {
        if (strcmp(name, s_libc[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

#if WASM_ENABLE_LIBC_BUILTIN
uint32_t get_libc_builtin_export_apis(NativeSymbol **p_libc_builtin_apis);

/* WAMR's snprintf, which formats from the app's va_list in its memory. */
typedef int (*wa_snprintf_t)(wasm_exec_env_t env, char *str, uint32_t size, const char *fmt, void *va);
static wa_snprintf_t s_wa_snprintf;

static void out_flush(app_t *a)
{
    if (a->out_len > 0) {
        fc_port_log(FC_LOG_INFO, "app %s: %.*s", a->name, a->out_len, a->out);
        a->out_len = 0;
    }
}

/* NULL app: called while the module is instantiated, before the app's
 * context exists; the text is dropped. */
static void out_put(app_t *a, const char *s, size_t n)
{
    if (a == NULL) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\n') {
            out_flush(a);
            continue;
        }
        a->out[a->out_len++] = s[i];
        if (a->out_len == (int)sizeof(a->out)) {
            out_flush(a);
        }
    }
}

/* The time these functions take counts against the call's APP_CALL_MS in
 * the device's functions, with fc's waits (fc_apps_native_time): they run
 * natively, outside the instruction budget, and a loop of them over a large
 * buffer would otherwise run for minutes. Only that time: the app's own
 * instructions are the budget's, which on the slower chips lasts more than
 * a second. Timed around each one that does work; a module run once has no
 * limit. Past the limit the exception is set, so the app traps on return. */
#define LIBC_TIMED(type, call)                                                                                       \
    do {                                                                                                             \
        int64_t t0_ = fc_apps_now_us();                                                                              \
        type r_ = (call);                                                                                            \
        fc_apps_native_time(env, wasm_runtime_get_user_data(env), t0_);                                              \
        return r_;                                                                                                   \
    } while (0)

/* WAMR's own functions, called after that check. Pointers arrive converted
 * and checked by WAMR, as their signatures say (the same as WAMR's). */
static void *s_wa_memcmp, *s_wa_memcpy, *s_wa_memmove, *s_wa_memset, *s_wa_memchr, *s_wa_strchr, *s_wa_strcmp,
    *s_wa_strcpy, *s_wa_strlen, *s_wa_strncasecmp, *s_wa_strspn, *s_wa_strcspn, *s_wa_strstr, *s_wa_sprintf;

typedef uint32_t (*wa_ppn_t)(wasm_exec_env_t, void *, void *, uint32_t);
typedef uint32_t (*wa_pin_t)(wasm_exec_env_t, void *, int32_t, uint32_t);
typedef uint32_t (*wa_pi_t)(wasm_exec_env_t, const char *, int32_t);
typedef uint32_t (*wa_pp_t)(wasm_exec_env_t, const char *, const char *);
typedef uint32_t (*wa_p_t)(wasm_exec_env_t, const char *);
typedef uint32_t (*wa_ppi_t)(wasm_exec_env_t, const char *, const char *, uint32_t);
typedef uint32_t (*wa_ppv_t)(wasm_exec_env_t, char *, const char *, void *);

#define LIBC_PPN(n)                                                                                                  \
    static uint32_t env_##n(wasm_exec_env_t env, void *a, void *b, uint32_t len)                                     \
    {                                                                                                                \
        LIBC_TIMED(uint32_t, ((wa_ppn_t)s_wa_##n)(env, a, b, len));                                                  \
    }
#define LIBC_PIN(n)                                                                                                  \
    static uint32_t env_##n(wasm_exec_env_t env, void *a, int32_t c, uint32_t len)                                   \
    {                                                                                                                \
        LIBC_TIMED(uint32_t, ((wa_pin_t)s_wa_##n)(env, a, c, len));                                                  \
    }
#define LIBC_PP(n)                                                                                                   \
    static uint32_t env_##n(wasm_exec_env_t env, const char *a, const char *b)                                       \
    {                                                                                                                \
        LIBC_TIMED(uint32_t, ((wa_pp_t)s_wa_##n)(env, a, b));                                                        \
    }
LIBC_PPN(memcmp)
LIBC_PPN(memcpy)
LIBC_PPN(memmove)
LIBC_PIN(memset)
LIBC_PIN(memchr)
LIBC_PP(strcmp)
LIBC_PP(strcpy)
LIBC_PP(strspn)
LIBC_PP(strcspn)
LIBC_PP(strstr)

static uint32_t env_strchr(wasm_exec_env_t env, const char *s, int32_t c)
{
    LIBC_TIMED(uint32_t, ((wa_pi_t)s_wa_strchr)(env, s, c));
}

static uint32_t env_strlen(wasm_exec_env_t env, const char *s)
{
    LIBC_TIMED(uint32_t, ((wa_p_t)s_wa_strlen)(env, s));
}

static uint32_t env_strncasecmp(wasm_exec_env_t env, const char *a, const char *b, uint32_t n)
{
    LIBC_TIMED(uint32_t, ((wa_ppi_t)s_wa_strncasecmp)(env, a, b, n));
}

static uint32_t env_sprintf(wasm_exec_env_t env, char *str, const char *fmt, void *va)
{
    LIBC_TIMED(uint32_t, ((wa_ppv_t)s_wa_sprintf)(env, str, fmt, va));
}

static int env_snprintf(wasm_exec_env_t env, char *str, uint32_t size, const char *fmt, void *va)
{
    LIBC_TIMED(int, s_wa_snprintf(env, str, size, fmt, va));
}

/* strncmp and strncpy read the source only up to its end, as C's: WAMR's
 * check n bytes at the source, so "on" near the end of the app's memory,
 * compared or copied with a larger n, stopped the app. */
static int32_t env_strncmp(wasm_exec_env_t env, const char *a, const char *b, uint32_t n)
{
    LIBC_TIMED(int32_t, strncmp(a, b, n));
}

static uint32_t env_strncpy(wasm_exec_env_t env, char *dst, const char *src, uint32_t n)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_native_addr(inst, dst, n)) {
        return 0;
    }
    int64_t t0 = fc_apps_now_us();
    strncpy(dst, src, n);
    fc_apps_native_time(env, wasm_runtime_get_user_data(env), t0);
    return (uint32_t)wasm_runtime_addr_native_to_app(inst, dst);
}

/* One printf writes at most 127 characters; the rest is cut. */
static int env_printf(wasm_exec_env_t env, const char *fmt, void *va)
{
    char buf[128];
    int64_t t0 = fc_apps_now_us();
    int n = s_wa_snprintf(env, buf, sizeof(buf), fmt, va);
    if (n > 0) {
        out_put(wasm_runtime_get_user_data(env), buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
    fc_apps_native_time(env, wasm_runtime_get_user_data(env), t0);
    return n;
}

static int env_puts(wasm_exec_env_t env, const char *s)
{
    size_t n = strlen(s);
    out_put(wasm_runtime_get_user_data(env), s, n);
    out_put(wasm_runtime_get_user_data(env), "\n", 1);
    return (int)n + 1;
}

static int env_putchar(wasm_exec_env_t env, int c)
{
    char ch = (char)c;
    out_put(wasm_runtime_get_user_data(env), &ch, 1);
    return c & 0xff;
}

static void env_abort(wasm_exec_env_t env)
{
    wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "abort()");
}

/* endptr: an address in the app's memory, 0 for none. */
static int set_endptr(wasm_exec_env_t env, uint32_t endptr, const char *end)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (endptr == 0) {
        return 0;
    }
    if (!wasm_runtime_validate_app_addr(inst, endptr, sizeof(uint32_t))) {
        return -1; /* the exception is set: the app traps */
    }
    uint32_t off = (uint32_t)wasm_runtime_addr_native_to_app(inst, (void *)end);
    memcpy(wasm_runtime_addr_app_to_native(inst, endptr), &off, sizeof(off));
    return 0;
}

static int32_t env_strtol(wasm_exec_env_t env, const char *nptr, uint32_t endptr, int32_t base)
{
    char *end;
    long v = strtol(nptr, &end, base);
    set_endptr(env, endptr, end);
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

static uint32_t env_strtoul(wasm_exec_env_t env, const char *nptr, uint32_t endptr, int32_t base)
{
    char *end;
    unsigned long v = strtoul(nptr, &end, base);
    set_endptr(env, endptr, end);
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

/* atoi as a clamped strtol, and the character classes: a value outside
 * 0..255 is no character (tolower and toupper give it back), where WAMR's
 * passed any int to the C library, whose table it indexes. */
static int32_t env_atoi(wasm_exec_env_t env, const char *s)
{
    (void)env;
    long v = strtol(s, NULL, 10);
    return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : (int32_t)v;
}

#define LIBC_CTYPE(n)                                                                                                \
    static int32_t env_##n(wasm_exec_env_t env, int32_t c)                                                           \
    {                                                                                                                \
        (void)env;                                                                                                   \
        return c >= 0 && c <= 255 && n((unsigned char)c) ? 1 : 0;                                                    \
    }
LIBC_CTYPE(isalnum)
LIBC_CTYPE(isalpha)
LIBC_CTYPE(isdigit)
LIBC_CTYPE(isgraph)
LIBC_CTYPE(isprint)
LIBC_CTYPE(isspace)
LIBC_CTYPE(isupper)
LIBC_CTYPE(isxdigit)

static int32_t env_tolower(wasm_exec_env_t env, int32_t c)
{
    (void)env;
    return c >= 0 && c <= 255 ? tolower((unsigned char)c) : c;
}

static int32_t env_toupper(wasm_exec_env_t env, int32_t c)
{
    (void)env;
    return c >= 0 && c <= 255 ? toupper((unsigned char)c) : c;
}

/* Every function an app may import from env but clock_ms (s_libc), in
 * strcmp order of the names: const, in flash, as WAMR does not sort a table
 * in order (third_party/patches/wamr-const-natives.patch; test_apps.c checks
 * the order). WAMR registers none of its own. */
static const NativeSymbol s_libc_natives[] = {
    {"abort", (void *)env_abort, "()", NULL},
    {"atoi", (void *)env_atoi, "($)i", NULL},
    {"isalnum", (void *)env_isalnum, "(i)i", NULL},
    {"isalpha", (void *)env_isalpha, "(i)i", NULL},
    {"isdigit", (void *)env_isdigit, "(i)i", NULL},
    {"isgraph", (void *)env_isgraph, "(i)i", NULL},
    {"isprint", (void *)env_isprint, "(i)i", NULL},
    {"isspace", (void *)env_isspace, "(i)i", NULL},
    {"isupper", (void *)env_isupper, "(i)i", NULL},
    {"isxdigit", (void *)env_isxdigit, "(i)i", NULL},
    {"memchr", (void *)env_memchr, "(*ii)i", NULL},
    {"memcmp", (void *)env_memcmp, "(**~)i", NULL},
    {"memcpy", (void *)env_memcpy, "(**~)i", NULL},
    {"memmove", (void *)env_memmove, "(**~)i", NULL},
    {"memset", (void *)env_memset, "(*ii)i", NULL},
    {"printf", (void *)env_printf, "($*)i", NULL},
    {"putchar", (void *)env_putchar, "(i)i", NULL},
    {"puts", (void *)env_puts, "($)i", NULL},
    {"snprintf", (void *)env_snprintf, "(*~$*)i", NULL},
    {"sprintf", (void *)env_sprintf, "($$*)i", NULL},
    {"strchr", (void *)env_strchr, "($i)i", NULL},
    {"strcmp", (void *)env_strcmp, "($$)i", NULL},
    {"strcpy", (void *)env_strcpy, "(*$)i", NULL},
    {"strcspn", (void *)env_strcspn, "($$)i", NULL},
    {"strlen", (void *)env_strlen, "($)i", NULL},
    {"strncasecmp", (void *)env_strncasecmp, "($$i)i", NULL},
    {"strncmp", (void *)env_strncmp, "($$i)i", NULL},
    {"strncpy", (void *)env_strncpy, "(*$i)i", NULL},
    {"strspn", (void *)env_strspn, "($$)i", NULL},
    {"strstr", (void *)env_strstr, "($$)i", NULL},
    {"strtol", (void *)env_strtol, "($ii)i", NULL},
    {"strtoul", (void *)env_strtoul, "($ii)i", NULL},
    {"tolower", (void *)env_tolower, "(i)i", NULL},
    {"toupper", (void *)env_toupper, "(i)i", NULL},
};

/* After wasm_runtime_full_init: WAMR's own functions, which the ones here
 * call, from its table. */
static void libc_init(void)
{
    NativeSymbol *wa = NULL;
    uint32_t n = get_libc_builtin_export_apis(&wa);
    static const struct {
        const char *name;
        void **fp;
    } want[] = {
        {"memcmp", &s_wa_memcmp},   {"memcpy", &s_wa_memcpy}, {"memmove", &s_wa_memmove},
        {"memset", &s_wa_memset},   {"memchr", &s_wa_memchr}, {"strchr", &s_wa_strchr},
        {"strcmp", &s_wa_strcmp},   {"strcpy", &s_wa_strcpy}, {"strlen", &s_wa_strlen},
        {"strncasecmp", &s_wa_strncasecmp}, {"strspn", &s_wa_strspn}, {"strcspn", &s_wa_strcspn},
        {"strstr", &s_wa_strstr},   {"sprintf", &s_wa_sprintf}, {"snprintf", (void **)&s_wa_snprintf},
    };
    for (uint32_t i = 0; i < n; i++) {
        for (size_t j = 0; j < sizeof(want) / sizeof(want[0]); j++) {
            if (strcmp(wa[i].symbol, want[j].name) == 0) {
                *want[j].fp = wa[i].func_ptr;
            }
        }
    }
    wasm_runtime_register_natives("env", (NativeSymbol *)s_libc_natives,
                                  sizeof(s_libc_natives) / sizeof(s_libc_natives[0]));
}
#else
static void out_flush(app_t *a)
{
    (void)a;
}

static void libc_init(void)
{
}
#endif

static int earlier_interface(const char *name)
{
    static const char *const old[] = {"set_state", "log", "led", "temp_mc", "threshold_mc"};
    for (size_t i = 0; i < sizeof(old) / sizeof(old[0]); i++) {
        if (strcmp(name, old[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int check_env_import(const wasm_import_t *im, char *err, size_t err_len)
{
    if (im->kind == WASM_IMPORT_EXPORT_KIND_FUNC && strcmp(im->name, "clock_ms") == 0) {
        wasm_func_type_t t = im->u.func_type;
        wasm_valkind_t r = wasm_func_type_get_result_count(t) == 1 ? wasm_func_type_get_result_valkind(t, 0) : 0xff;
        if (wasm_func_type_get_param_count(t) == 0 && (r == WASM_I32 || r == WASM_I64)) {
            return 0;
        }
        snprintf(err, err_len, "imports env.clock_ms with a type other than () -> i32 or i64");
        return -1;
    }
    if (im->kind == WASM_IMPORT_EXPORT_KIND_FUNC && libc_function(im->name)) {
        if (im->linked) {
            return 0;
        }
#if WASM_ENABLE_LIBC_BUILTIN
        snprintf(err, err_len, "imports env.%.20s with a type other than the C library's (wasm-apps/sdk/libc)",
                 im->name);
#else
        snprintf(err, err_len, "imports env.%.20s: this firmware is built without the C library", im->name);
#endif
        return -1;
    }
    if (earlier_interface(im->name)) {
        snprintf(err, err_len, "built for the earlier app interface (imports env.%.20s): rebuild with fc_app.h",
                 im->name);
    } else {
        snprintf(err, err_len, "imports env.%.40s, which this firmware does not have", im->name);
    }
    return -1;
}

/* Whether the module imports from "fc". A module run once has no app context
 * around its call (handles, events, the wait limits), so it may not yet. */
static int imports_fc(wasm_module_t m)
{
    int32_t n = wasm_runtime_get_import_count(m);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t im;
        wasm_runtime_get_import_type(m, i, &im);
        if (strcmp(im.module_name, "fc") == 0) {
            return 1;
        }
    }
    return 0;
}

/* Whether this firmware can run a module: 0, or -1 with the reason. entry:
 * the app's FC_ENTRY, or "". */
static int check_module(wasm_module_t m, const char *entry, char *err, size_t err_len)
{
    int32_t n = wasm_runtime_get_import_count(m);
    for (int32_t i = 0; i < n; i++) {
        wasm_import_t im;
        wasm_runtime_get_import_type(m, i, &im);
        if (strcmp(im.module_name, "env") == 0) {
            if (check_env_import(&im, err, err_len) != 0) {
                return -1;
            }
            continue;
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
    int fc_app = export_sig(m, "tick", sig, sizeof(sig)) != NULL || export_sig(m, "on_event", sig, sizeof(sig)) != NULL;
    if (entry[0]) {
        const char *s = export_sig(m, entry, sig, sizeof(sig));
        if (s == NULL || s[0] != '>' || strlen(s) > 2) {
            snprintf(err, err_len, "FC_ENTRY: exports no %.31s() without parameters", entry);
            return -1;
        }
    } else if (!fc_app && export_sig(m, "_start", sig, sizeof(sig)) == NULL) {
        snprintf(err, err_len, "exports neither tick() nor on_event() nor _start(), and has no FC_ENTRY");
        return -1;
    }
    if ((entry[0] || !fc_app) && imports_fc(m)) {
        snprintf(err, err_len, "runs once (FC_ENTRY or _start) and imports from \"fc\": not supported yet");
        return -1;
    }
    return 0;
}

/* Before a module run once starts: waits until the agent has had nothing on
 * hand (fc_agent_busy) for ENTRY_QUIET_MS. Right after an app changes the
 * agent applies the spec and sends two status updates (the second grouped
 * 15 s later), each a TLS exchange: on a chip with one core they took CPU
 * from the first second of the run, which a benchmark such as CoreMark
 * calibrates itself on (its timed run then lasted less than the 10 s it
 * requires, and its score was 0). The instance is made before the wait, so
 * that its memory is in one block (runner). Gives up after ENTRY_WAIT_MS,
 * and returns early when the app is stopped. */
static void wait_quiet(app_t *a)
{
    int64_t now = fc_apps_now_ms(), until = now + s_entry_wait_ms, quiet_since = now;
    while (!stopping(a) && now < until) {
        if (fc_agent_busy()) {
            quiet_since = now;
        } else if (now - quiet_since >= ENTRY_QUIET_MS) {
            return;
        }
        usleep(100 * 1000);
        now = fc_apps_now_ms();
        guard_tick(a, now);
    }
}

/* The export run once, or NULL for an app of init/tick/on_event. */
static const char *entry_of(const app_t *a, wasm_module_inst_t inst)
{
    if (a->entry[0]) {
        return a->entry;
    }
    if (wasm_runtime_lookup_function(inst, "tick") == NULL && wasm_runtime_lookup_function(inst, "on_event") == NULL &&
        wasm_runtime_lookup_function(inst, "_start") != NULL) {
        return "_start";
    }
    return NULL;
}

/* Lets the next call into the app run limit instructions (-1: no limit), and
 * makes it the one app_stop interrupts: 0 when the app is stopping. Under
 * s_lock, as app_stop sets the limit to 0 after it sets stop. */
static int arm_call(app_t *a, wasm_exec_env_t env, int limit)
{
    pthread_mutex_lock(&s_lock);
    int ok = !stopping(a);
    if (ok) {
#if WASM_ENABLE_INSTRUCTION_METERING
        wasm_runtime_set_instruction_count_limit(env, limit);
#endif
        a->run_env = env;
    }
    pthread_mutex_unlock(&s_lock);
    return ok;
}

static void disarm_call(app_t *a)
{
    pthread_mutex_lock(&s_lock);
    a->run_env = NULL;
    pthread_mutex_unlock(&s_lock);
}

/* One call into the app, within its instruction budget. 0: it trapped, or
 * the app is stopping. */
static int call_app(app_t *a, wasm_exec_env_t env, wasm_function_inst_t f, uint32_t argc, uint32_t *argv)
{
#if WASM_ENABLE_INSTRUCTION_METERING
    int budget = s_call_budget;
#else
    int budget = -1;
#endif
    a->call_start_ms = fc_apps_now_ms();
    a->native_us = 0;
    a->unlimited = 0;
    return arm_call(a, env, budget) && wasm_runtime_call_wasm(env, f, argc, argv);
}

/* One call into the app: 0, or -1 when it trapped (status set). */
static int call_fc(app_t *a, wasm_exec_env_t env, wasm_function_inst_t f, uint32_t argc, uint32_t *argv,
                   const char *what)
{
    int ok = call_app(a, env, f, argc, argv);
    out_flush(a);
    fc_appapi_end_call(a);
    if (ok) {
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
/* Once a second at most: on Zephyr reading it scans the unused stack. */
static void note_stack(app_t *a, int64_t now)
{
    if (now - a->stack_at_ms < 1000) {
        return;
    }
    a->stack_at_ms = now;
    long n = fc_port_stack_unused();
    long seen = __atomic_load_n(&a->stack_unused, __ATOMIC_RELAXED);
    if (n >= 0 && (seen < 0 || n < seen)) {
        __atomic_store_n(&a->stack_unused, n, __ATOMIC_RELAXED);
    }
}

long fc_apps_stack_unused(void)
{
    long low = -1;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_APPS; i++) {
        const app_t *a = &s_apps[i];
        long n = a->used && a->thread_running ? __atomic_load_n(&a->stack_unused, __ATOMIC_RELAXED) : -1;
        if (n >= 0 && (low < 0 || n < low)) {
            low = n;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return low;
}

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
        guard_tick(a, now);
        note_stack(a, now);
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
        if (!a->settled && a->settle_at_ms < until) {
            until = a->settle_at_ms;
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

/* A value as text without printf's 64-bit or floating-point conversions,
 * which not every port's libc has: floats in thousandths. */
static void format_result(wasm_valkind_t kind, const uint32_t *cell, char *out, size_t len)
{
    int64_t v;
    int milli = 0;
    if (kind == WASM_I32) {
        v = (int32_t)cell[0];
    } else if (kind == WASM_I64) {
        memcpy(&v, cell, sizeof(v));
    } else {
        double d;
        if (kind == WASM_F32) {
            float f;
            memcpy(&f, cell, sizeof(f));
            d = f;
        } else {
            memcpy(&d, cell, sizeof(d));
        }
        if (!(d == d) || d > 9e15 || d < -9e15) {
            snprintf(out, len, "%s", d == d ? "out of range" : "nan");
            return;
        }
        v = (int64_t)(d * 1000 + (d < 0 ? -0.5 : 0.5));
        milli = 1;
    }
    char digits[24];
    int n = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
        if (milli && n == 3) {
            digits[n++] = '.';
        }
    } while (u != 0 || (milli && n < 5));
    size_t k = 0;
    if (v < 0 && k + 1 < len) {
        out[k++] = '-';
    }
    while (n > 0 && k + 1 < len) {
        out[k++] = digits[--n];
    }
    out[k] = '\0';
}

/* The export an app runs once (FC_ENTRY, or _start), with no instruction
 * budget: app_stop interrupts it. 1: it returned, and the app is Completed;
 * 0: it trapped, or the app is stopping. */
static int run_entry(app_t *a, wasm_module_inst_t inst, wasm_exec_env_t env, const char *name)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, name);
    uint32_t nres = f ? wasm_func_get_result_count(f, inst) : 0;
    if (f == NULL || wasm_func_get_param_count(f, inst) != 0 || nres > 1 || imports_fc(a->module)) {
        char why[80];
        if (f != NULL && imports_fc(a->module)) {
            snprintf(why, sizeof(why), "runs once (FC_ENTRY) and imports from \"fc\": not supported yet");
        } else {
            snprintf(why, sizeof(why), "FC_ENTRY: exports no %.31s() without parameters", name);
        }
        fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, why);
        set_status(a, "Error", why);
        return 1; /* not run again until its image or env change */
    }
    a->inst = inst;
    pthread_mutex_lock(&s_lock);
    a->state[0] = '\0';
    pthread_mutex_unlock(&s_lock);
    set_status(a, "Running", NULL);
    fc_port_log(FC_LOG_INFO, "app %s running %s() (%s)", a->name, name, a->image);
    uint32_t argv[2] = {0, 0};
    a->call_start_ms = fc_apps_now_ms();
    a->unlimited = 1; /* the device's functions too (fc_apps_native_time) */
    int ok = arm_call(a, env, -1) && wasm_runtime_call_wasm(env, f, 0, argv);
    if (!ok) {
        if (!stopping(a)) {
            const char *ex = wasm_runtime_get_exception(inst);
            fc_port_log(FC_LOG_WARN, "app %s trapped in %s(): %s", a->name, name, ex ? ex : "?");
            set_status(a, "Error", ex);
        }
        return 0;
    }
    char val[32] = "";
    if (nres == 1) {
        wasm_valkind_t kind;
        wasm_func_get_result_types(f, inst, &kind);
        format_result(kind, argv, val, sizeof(val));
    }
    int64_t ms = fc_apps_now_ms() - a->call_start_ms;
    pthread_mutex_lock(&s_lock);
    snprintf(a->state, sizeof(a->state), nres ? "%.31s() returned %s" : "%.31s() returned", name, val);
    pthread_mutex_unlock(&s_lock);
    fc_port_log(FC_LOG_INFO, "app %s: %s (in %ld ms)", a->name, a->state, (long)ms);
    set_status(a, "Completed", NULL);
    return 1;
}

/* Code the module runs as it is instantiated (a start section,
 * __wasm_call_ctors: C++ constructors, TinyGo), which WAMR runs in an exec
 * env of its own before wasm_runtime_instantiate returns: run as a call of
 * the app, with its context (an fc call is the app's), its instruction
 * budget and its limit in the device's functions, and stopped by app_stop.
 * Before, it had none of these: a loop there was never stopped, and the
 * app could be neither updated nor removed (third_party/patches/
 * wamr-instantiation-hook.patch). */
static void instantiation_hook(wasm_exec_env_t env, bool begin)
{
    wasm_module_t m = wasm_runtime_get_module(wasm_runtime_get_module_inst(env));
    app_t *a = NULL;
    for (int i = 0; i < MAX_APPS && a == NULL; i++) {
        a = s_apps[i].used && s_apps[i].module == m ? &s_apps[i] : NULL; /* each app has its own module */
    }
    if (a == NULL || !pthread_equal(pthread_self(), a->thread)) {
        return;
    }
    if (begin) {
        wasm_runtime_set_user_data(env, a);
        fc_appapi_load_config(a);
#if WASM_ENABLE_INSTRUCTION_METERING
        int budget = s_call_budget;
#else
        int budget = -1;
#endif
        a->call_start_ms = fc_apps_now_ms();
        a->native_us = 0;
        a->unlimited = 0;
        if (!arm_call(a, env, budget)) {
#if WASM_ENABLE_INSTRUCTION_METERING
            wasm_runtime_set_instruction_count_limit(env, 0); /* stopping: at once */
#endif
        }
        return;
    }
    disarm_call(a);
    out_flush(a);
    fc_appapi_end_call(a);
}

/* The app's instance, or NULL with the reason in ebuf. One whose memory
 * would leave the agent less than s_heap_floor is not kept (*short_of_heap
 * set). */
static wasm_module_inst_t instantiate(app_t *a, char *ebuf, size_t elen, int *short_of_heap)
{
    *short_of_heap = 0;
    wasm_module_inst_t inst = wasm_runtime_instantiate(a->module, WASM_STACK, 0, ebuf, (uint32_t)elen);
    if (inst == NULL) {
        fc_appapi_close_all(a); /* what code run as it was instantiated opened */
    }
    long left = inst != NULL ? fc_port_heap_free() : -1;
    if (left >= 0 && left < s_heap_floor) {
        wasm_runtime_deinstantiate(inst);
        fc_appapi_close_all(a);
        snprintf(ebuf, elen, "its memory would leave the agent %ld KB of heap, less than the %ld KB it needs", left / 1024,
                 s_heap_floor / 1024);
        *short_of_heap = 1;
        return NULL;
    }
    if (inst != NULL && left >= 0) {
        fc_port_log(FC_LOG_INFO, "app %s: instance made, %ld KB of heap left for the agent", a->name, left / 1024);
    }
    return inst;
}

static void *runner(void *arg)
{
    app_t *a = arg;
    char ebuf[128];
    wasm_runtime_init_thread_env();
    int completed = 0;
    /* app_start still holds the heap's largest block. A wait of a tick or
     * more: shorter, ESP-IDF's usleep spins, and on a chip with one core this
     * thread then kept app_start from ever letting go. */
    while (!__atomic_load_n(&a->go, __ATOMIC_ACQUIRE)) {
        usleep(20 * 1000);
    }
    while (!stopping(a)) {
        a->started_ms = fc_apps_now_ms();
        a->out_len = 0;
        /* The instance first, its memory in one block while the agent
         * waits (app_start): made later, after the agent's connection had
         * opened again, a 64 KB memory no longer fitted on an ESP32-C5. */
        int short_of_heap;
        wasm_module_inst_t inst = instantiate(a, ebuf, sizeof(ebuf), &short_of_heap);
        __atomic_store_n(&a->made, 1, __ATOMIC_RELEASE);
        if (inst == NULL && (short_of_heap || strstr(ebuf, "allocate") != NULL)) {
            /* Made while the spec is applied, whose memory (the parsed
             * spec, the status after it) may be in the way: once more when
             * the agent is done. On an ESP32-C5 a restart for new env vars
             * found 58 KB in one piece, and 76 KB five seconds later. */
            wait_quiet(a);
            if (!stopping(a)) {
                inst = instantiate(a, ebuf, sizeof(ebuf), &short_of_heap);
            }
        }
        wasm_exec_env_t env = inst ? wasm_runtime_create_exec_env(inst, WASM_STACK) : NULL;
        pthread_mutex_lock(&s_lock);
        a->live = inst != NULL;
        pthread_mutex_unlock(&s_lock);
        if (inst == NULL || env == NULL) {
            const char *why = inst == NULL ? ebuf : "exec env: out of memory";
            /* Out of memory: how much would fit, to compare with what the
             * module asks for (its memory comes in one block). */
            long largest = fc_port_heap_largest();
            char wbuf[sizeof(ebuf) + 48];
            if (largest >= 0 && (strstr(why, "allocate") != NULL || strstr(why, "out of memory") != NULL)) {
                snprintf(wbuf, sizeof(wbuf), "%s (largest free block %ld KB)", why, largest / 1024);
                why = wbuf;
            }
            fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, why);
            set_status(a, "Error", why);
            if (short_of_heap && !stopping(a)) {
                /* Twice, the second time with the agent idle: not started
                 * again, which would take the agent's heap every few
                 * seconds, until its image, its env vars or the firmware
                 * change, or the device restarts. */
                completed = 1;
            }
        } else {
            wasm_runtime_set_user_data(env, a);
            wasm_function_inst_t abi = wasm_runtime_lookup_function(inst, "fc_abi_version");
            uint32_t abi_argv[1] = {0};
            if (abi != NULL && call_app(a, env, abi, 0, abi_argv) && abi_argv[0] > FC_APP_ABI_VERSION) {
                snprintf(ebuf, sizeof(ebuf), "app needs interface v%u, this device has v%d", (unsigned)abi_argv[0],
                         FC_APP_ABI_VERSION);
                fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, ebuf);
                set_status(a, "Error", ebuf);
                disarm_call(a);
                wasm_runtime_destroy_exec_env(env);
                wasm_runtime_deinstantiate(inst);
                pthread_mutex_lock(&s_lock);
                a->live = 0;
                pthread_mutex_unlock(&s_lock);
                goto done;
            }
            const char *entry = entry_of(a, inst);
            if (entry != NULL) {
                /* Run once: when the agent is quiet (wait_quiet). */
                wait_quiet(a);
                a->started_ms = fc_apps_now_ms();
            }
            if (stopping(a)) {
                /* removed or replaced while it waited */
            } else if (entry == NULL) {
                run_fc(a, inst, env);
            } else if (run_entry(a, inst, env, entry)) {
                completed = 1;
            }
            out_flush(a);
        }
        disarm_call(a);
        a->inst = NULL;
        if (env) {
            wasm_runtime_destroy_exec_env(env);
        }
        if (inst) {
            wasm_runtime_deinstantiate(inst);
        }
        pthread_mutex_lock(&s_lock);
        a->live = 0;
        pthread_mutex_unlock(&s_lock);
        if (completed) {
            /* Ran to its end (or cannot run): not started again, and past
             * its start for the record of apps that crash the device. */
            guard_forget(a->name, 0);
            break;
        }
        if (!stopping(a)) {
            for (int i = 0; i < RESTART_DELAY_S * 10 && !stopping(a); i++) {
                usleep(100 * 1000);
                guard_tick(a, fc_apps_now_ms());
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

/* The heap's largest block held while the app's thread is made (app_start),
 * so that its stack goes elsewhere and the block stays whole for the
 * instance's memory: taken from it, it left an ESP32-C5 62 KB in one piece
 * for a 64 KB memory. When no other block has room for the stack, the hold
 * leaves it room at the block's end: held whole, pthread_create failed
 * first on the C3, C5 and C6 (ESP-IDF logged "Failed to create task!") and
 * the thread was only made at the second try, without the hold. NULL: none. */
static void *hold_but_stack(void)
{
    void *hold = fc_heap_hold(0);
    if (hold != NULL) {
        long rest = fc_port_heap_largest(); /* the largest block but the one held */
        if (rest >= 0 && rest < THREAD_STACK + THREAD_SLACK) {
            free(hold);
            hold = fc_heap_hold(THREAD_STACK + THREAD_SLACK);
        }
    }
    return hold;
}

static void app_start(app_t *a)
{
    if (a->module == NULL) {
        return; /* the status already says why */
    }
    set_stop(a, 0);
    __atomic_store_n(&a->stack_unused, -1L, __ATOMIC_RELAXED);
    a->stack_at_ms = 0;
    a->settle_at_ms = fc_apps_now_ms() + s_settle_ms;
    a->settled = 0;
    set_status(a, "Preparing", NULL);
    pthread_attr_init(&a->thread_attr);
    __atomic_store_n(&a->made, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->go, 0, __ATOMIC_RELAXED);
    /* The thread's stack goes around the heap's largest block, kept whole
     * for the instance's memory (hold_but_stack, fc_heap_hold.h). Made
     * again without the hold if it still did not fit. */
    void *hold = hold_but_stack();
    int sized = pthread_attr_setstacksize(&a->thread_attr, THREAD_STACK) == 0;
    a->thread_running = sized && pthread_create(&a->thread, &a->thread_attr, runner, a) == 0;
    if (sized && !a->thread_running && hold != NULL) {
        free(hold);
        hold = NULL;
        a->thread_running = pthread_create(&a->thread, &a->thread_attr, runner, a) == 0;
    }
    free(hold);
    __atomic_store_n(&a->go, 1, __ATOMIC_RELEASE);
    /* Until the instance is made (or could not be): the caller, the spec
     * being applied, holds the agent's connection closed meanwhile
     * (fc_agent.c, turn_hold), so the instance's memory is taken first. */
    for (int i = 0; a->thread_running && i < APP_MADE_WAIT_MS / 20 && !__atomic_load_n(&a->made, __ATOMIC_ACQUIRE);
         i++) {
        usleep(20 * 1000); /* a tick or more: shorter, ESP-IDF's usleep spins */
    }
    if (!a->thread_running) {
        pthread_attr_destroy(&a->thread_attr);
        set_status(a, "Error", "cannot start thread");
    }
}

static void app_stop(app_t *a)
{
    if (a->thread_running) {
        set_stop(a, 1);
        pthread_mutex_lock(&s_lock);
#if WASM_ENABLE_INSTRUCTION_METERING
        if (a->run_env != NULL) {
            /* The call in progress traps at its next branch or call. */
            wasm_runtime_set_instruction_count_limit(a->run_env, 0);
        }
#endif
        pthread_mutex_unlock(&s_lock);
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
    fc_metrics_app_drop(a->name);
    if (a->module != NULL) {
        wasm_runtime_unload(a->module);
        a->module = NULL;
    }
}

/* ---- env vars ---------------------------------------------------------------- */

/* envVars.FC_ENTRY, or "". */
static void entry_from_env(const char *json, char *out, size_t len)
{
    cJSON *o = cJSON_Parse(json ? json : "{}");
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(o, "FC_ENTRY"));
    snprintf(out, len, "%s", e ? e : "");
    cJSON_Delete(o);
}

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
    entry_from_env(a->env_json, a->entry, sizeof(a->entry));
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
static wasm_module_t load_app(unsigned char *code, size_t len, const char *entry, char *ebuf, size_t elen)
{
    wasm_module_t m = load_module(code, len, ebuf, elen);
    if (m != NULL && check_module(m, entry, ebuf, elen) != 0) {
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
    wasm_runtime_set_instantiation_hook(instantiation_hook);
    wasm_runtime_register_natives_raw("env", (NativeSymbol *)s_env_natives,
                                      sizeof(s_env_natives) / sizeof(s_env_natives[0]));
    libc_init();
    fc_board_load(g_fc_hw);
    s_inited = 1;
    guard_boot();

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
        a->used = 1;
        snprintf(a->name, sizeof(a->name), "%s", name);
        snprintf(a->image, sizeof(a->image), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "image")));
        snprintf(a->digest, sizeof(a->digest), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(o, "digest")));
        a->tick_ms = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(o, "tick_ms"));
        set_env(a, cJSON_GetStringValue(cJSON_GetObjectItem(o, "env")));
        if (guard_is_bad(a->name, a->image, a->env_json)) {
            /* Kept, so the status says why; the spec does not start it again. */
            free(code);
            set_status(a, "Error", GUARD_ERROR);
            continue;
        }
        char ebuf[96];
        guard_begin(a->name, a->image, a->env_json, 1);
        a->module = load_app((unsigned char *)code, code_len, a->entry, ebuf, sizeof(ebuf));
        guard_loaded();
        free(code);
        if (a->module == NULL) {
            /* Shown in the status; the next spec downloads the image again. */
            guard_forget(a->name, 0);
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
/* The largest .wasm the device takes now: what it can load and still leave
 * the agent s_heap_floor, at most MAX_WASM_SIZE. A running version of the
 * app, kept until the new one is ready, takes its part of the heap. Its
 * linear memory and thread come later, checked with the instance (the
 * floor). */
static size_t wasm_limit(void)
{
    long free_now = fc_port_heap_free();
    if (free_now < 0) {
        return MAX_WASM_SIZE;
    }
    long n = (free_now - s_heap_floor) / LOAD_FACTOR;
    return n <= 0 ? 0 : n > MAX_WASM_SIZE ? MAX_WASM_SIZE : (size_t)n;
}

static int fetch_module(const char *image, const char *entry, unsigned char **out, size_t *len,
                        wasm_module_t *module, char *digest, size_t dlen, char *err, size_t err_len)
{
    size_t limit = wasm_limit();
    int rc = fc_oci_fetch_layer(image, "wasm", LAYER_WASM, limit, out, len, digest, dlen, err, err_len);
    if (rc == FC_OCI_REFUSED) {
        /* Larger than the limit: not downloaded, and not tried again until
         * the spec changes. */
        size_t n = strlen(err);
        if (limit < MAX_WASM_SIZE && n > 0 && err[n - 1] == ')') {
            snprintf(err + n - 1, err_len - (n - 1), " with this heap)"); /* "layer is N bytes (max M with this heap)" */
        }
        fc_port_log(FC_LOG_WARN, "%s: %ld KB of heap free, the agent keeps %ld KB", err, fc_port_heap_free() / 1024,
                    s_heap_floor / 1024);
        return 1;
    }
    if (rc != 0) {
        return -1;
    }
    char ebuf[96];
    /* What WAMR keeps of the module (functions, the interpreter's code,
     * many pieces) goes around the largest block (fc_heap_hold.h). */
    void *hold = fc_heap_hold(0);
    *module = load_app(*out, *len, entry, ebuf, sizeof(ebuf));
    if (*module == NULL && hold != NULL) {
        free(hold);
        hold = NULL;
        *module = load_app(*out, *len, entry, ebuf, sizeof(ebuf));
    }
    free(hold);
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
            guard_forget(a->name, 1);
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
        if (guard_is_bad(name, image, env)) {
            /* Neither downloaded nor loaded again: an error in the status,
             * not retried until the image, the env vars or the firmware
             * change. A version that runs, if any, keeps running. */
            snprintf(err, err_len, "app %s: %s", name, GUARD_ERROR);
            rc = -1;
            if (a == NULL && (a = alloc_slot()) != NULL) {
                pthread_mutex_lock(&s_lock);
                a->used = 1;
                snprintf(a->name, sizeof(a->name), "%s", name);
                snprintf(a->image, sizeof(a->image), "%s", image);
                pthread_mutex_unlock(&s_lock);
                set_env(a, env);
            }
            if (a != NULL && a->module == NULL) {
                set_status(a, "Error", GUARD_ERROR);
            }
            free(env);
            continue;
        }
        /* Same image: nothing to do, unless its saved module failed to load
         * at boot, in which case it is downloaded again. */
        if (a != NULL && strcmp(a->image, image) == 0 && a->module != NULL) {
            if (env != NULL && strcmp(env, a->env_json ? a->env_json : "{}") != 0) {
                /* What it may open, or how often it ticks: start again. */
                fc_port_log(FC_LOG_INFO, "app %s: new env vars, restarting it", a->name);
                app_stop(a);
                set_env(a, env);
                a->tick_ms = tick;
                guard_begin(a->name, a->image, a->env_json, 0);
                app_start(a);
                changed = 1;
            }
            free(env);
            continue;
        }
        /* A version with no instance (it could not start, waits to restart,
         * waits to run once, or ran to its end) is released before the new
         * one is loaded: kept, it would split the heap's largest block, where
         * the new module goes, and a 64 KB linear memory then no longer fit
         * on a classic ESP32 (it failed at every restart until the app was
         * removed and given again). One that runs keeps running until the
         * new one is ready. */
        if (a != NULL && a->module != NULL) {
            pthread_mutex_lock(&s_lock);
            int idle = !a->live;
            pthread_mutex_unlock(&s_lock);
            if (idle) {
                fc_port_log(FC_LOG_INFO, "app %s: not running, released before %s is loaded", name, image);
                app_release(a);
            }
        }
        unsigned char *code = NULL;
        size_t len = 0;
        wasm_module_t module = NULL;
        char digest[72], ferr[160];
        guard_begin(name, image, env, 1); /* a module that crashes as it loads is this app's */
        char entry[sizeof(a->entry)];
        entry_from_env(env, entry, sizeof(entry));
        int f = fetch_module(image, entry, &code, &len, &module, digest, sizeof(digest), ferr, sizeof(ferr));
        guard_loaded();
        if (f != 0) {
            guard_forget(name, 0);
            snprintf(err, err_len, "app %s: %s", name, ferr);
            if (a != NULL && a->module == NULL) {
                set_status(a, "Error", ferr); /* nothing runs: say why */
            }
            *retry |= f < 0;
            rc = -1;
            free(env);
            continue; /* the running version, if any, keeps running */
        }
        if (a == NULL && (a = alloc_slot()) == NULL) {
            snprintf(err, err_len, "app %s: too many apps (max %d)", name, MAX_APPS);
            guard_forget(name, 0);
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
            if (a->module == NULL) {
                set_status(a, "Error", "cannot store the module");
            }
            guard_forget(name, 0);
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
    int n = 0, running = 0, errors = 0, completed = 0;
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
        completed += strcmp(a->status, "Completed") == 0; /* ran to its end: not degraded */
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
                                                        : running + completed == n ? "Healthy" : "Degraded";
    cJSON_AddStringToObject(summary, "status", st);
    if (n == 0) {
        cJSON_AddStringToObject(summary, "info", "No applications");
    }
}
