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
#include <math.h>
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
 * the instance is made, the spec freed and the agent's connection closed):
 * what it takes to open that connection again, send its status and go on.
 * Measured on the five ESP-IDF chips with v3.14-rc10, the block a TLS
 * handshake holds left out (fc_heap_hold.h): 33 to 37 KB after an app's
 * install at regime; with 24 KB, the classic ESP32 running CoreMark failed
 * two status updates and its heap went down to 4 KB. A metrics export
 * (fc_agent.c) and MQTT (mqtt_may_connect) take more, and wait for room
 * above it. */
#ifndef FC_APP_HEAP_FLOOR
#define FC_APP_HEAP_FLOOR (40 * 1024)
#endif
static long s_heap_floor = FC_APP_HEAP_FLOOR;
/* What pthread_create takes beside the stack (ESP-IDF: the task's control
 * block, ~350 bytes), and the heap's headers. */
#define THREAD_SLACK    1024
#define RESTART_DELAY_S 5
/* One string: a pending start held at boot is told by its address. */
static const char s_boot_why[] = "boot";
#define BOOT_WHY        s_boot_why
#ifndef BOOT_APPS_WAIT_MS
#define BOOT_APPS_WAIT_MS (180 * 1000) /* saved apps start without an accepted status after this */
#endif
#define MEM_RETRY_MS    (30 * 1000) /* fc_apps_poll: how often an app short of heap looks again */
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

/* The slot identities and their event locks outlive callbacks. Descriptors
 * only exist for installed apps. Mutations hold s_lifecycle (or boot runs
 * alone); s_lock protects publication and readers on other threads. */
static app_t *s_apps[FC_MAX_APPS];
static app_sync_t s_sync[FC_MAX_APPS];
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
/* The spec, saved starts and a primary restarting after a trap can all
 * stop or unload another slot. Never wait for this lock from a runner
 * being joined by its owner. */
static pthread_mutex_t s_lifecycle = PTHREAD_MUTEX_INITIALIZER;
static unsigned s_list_revision; /* detects an intervening apply after early pruning */
static int s_inited;
static int s_poll_event;       /* runner to spec thread */
static int64_t s_poll_retry;   /* monotonic milliseconds, spec thread only */
static void park_secondary(const char *primary_name, bool keep_metrics);
static void app_release(app_t *a, bool keep_metrics);
static int load_saved(app_t *a);
static int mem_room(const app_t *a);
static size_t wasm_limit(void);
static int run_pending(int boot_only);
static void restore_slots(const char *only);
static const char *saved_primary_name(void);
static cJSON *s_restore; /* saved entries waiting for the primary or for descriptor memory */

static app_t *primary_app(void)
{
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (s_apps[i] && s_apps[i]->primary) {
            return s_apps[i];
        }
    }
    return NULL;
}

static int admits_secondary(app_t *primary)
{
    if (primary == NULL) {
        return 1;
    }
    pthread_mutex_lock(&s_lock);
    int ready = (primary->live && !primary->can_grow) || strcmp(primary->status, "Completed") == 0;
    pthread_mutex_unlock(&s_lock);
    return ready;
}


static void set_status(app_t *a, const char *status, const char *error)
{
    pthread_mutex_lock(&s_lock);
    int changed = strcmp(a->status, status) != 0 || strcmp(a->error, error ? error : "") != 0;
    snprintf(a->status, sizeof(a->status), "%s", status);
    snprintf(a->error, sizeof(a->error), "%s", error ? error : "");
    if (strcmp(status, "Preparing") == 0) {
        a->state[0] = '\0'; /* the previous instance no longer supplies customInfo */
    }
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

int fc_apps_post_locked(app_t *a, int32_t type, int32_t handle, int32_t value, fc_mqtt_msg_t *msg)
{
    if (!a->accepting) {
        return -1;
    }
    /* An edge replaces the one still queued for the same GPIO. */
    for (int k = 0; type == EV_GPIO && k < a->ev_count; k++) {
        app_event_t *e = &a->io->ev[(a->ev_head + k) % APP_EVENTS];
        if (e->type == EV_GPIO && e->handle == handle) {
            e->value = value;
            return 0;
        }
    }
    if (a->ev_count == APP_EVENTS) {
        fc_mqtt_msg_free(a->io->ev[a->ev_head].msg); /* back to the shared pool */
        a->ev_head = (a->ev_head + 1) % APP_EVENTS;
        a->ev_count--;
        a->ev_lost++;
    }
    a->io->ev[(a->ev_head + a->ev_count) % APP_EVENTS] = (app_event_t){(int8_t)type, (int8_t)handle, value, msg};
    a->ev_count++;
    pthread_cond_signal(&a->sync->cond);
    return 0;
}

void fc_apps_post(app_t *a, int32_t type, int32_t handle, int32_t value)
{
    pthread_mutex_lock(&a->sync->lock);
    fc_apps_post_locked(a, type, handle, value, NULL);
    pthread_mutex_unlock(&a->sync->lock);
}

void fc_apps_drop_events(app_t *a, int (*keep)(const app_event_t *e, int arg), int arg)
{
    int kept = 0;
    for (int k = 0; k < a->ev_count; k++) {
        app_event_t e = a->io->ev[(a->ev_head + k) % APP_EVENTS];
        if (keep != NULL && keep(&e, arg)) {
            a->io->ev[(a->ev_head + kept) % APP_EVENTS] = e;
            kept++;
        } else {
            fc_mqtt_msg_free(e.msg);
        }
    }
    a->ev_count = kept;
    if (keep == NULL) {
        a->ev_head = 0;
        a->ev_lost = 0;
    }
}

/* The next event: FC_EV_OVERFLOW first when some were dropped. */
static int next_event(app_t *a, app_event_t *e)
{
    int got = 1;
    pthread_mutex_lock(&a->sync->lock);
    if (a->ev_lost > 0) {
        *e = (app_event_t){EV_OVERFLOW, -1, a->ev_lost, NULL};
        a->ev_lost = 0;
    } else if (a->ev_count > 0) {
        *e = a->io->ev[a->ev_head];
        a->ev_head = (a->ev_head + 1) % APP_EVENTS;
        a->ev_count--;
    } else {
        got = 0;
    }
    pthread_mutex_unlock(&a->sync->lock);
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
    pthread_mutex_lock(&a->sync->lock);
    if (a->ev_count == 0 && a->ev_lost == 0 && !stopping(a)) {
        pthread_cond_timedwait(&a->sync->cond, &a->sync->lock, &ts);
    }
    pthread_mutex_unlock(&a->sync->lock);
}

static void wake(app_t *a)
{
    pthread_mutex_lock(&a->sync->lock);
    pthread_cond_broadcast(&a->sync->cond);
    pthread_mutex_unlock(&a->sync->lock);
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
/* Set before the agent restarts the device for want of an accepted status
 * while the heap was below the floor (fc_apps_note_silent_restart): at the
 * next boot that restart counts as a crash. An ESP32-C5 whose two saved apps
 * left the agent 7 KB restarted that way again and again (v3.14-rc10.1). */
#define KV_SILENT       "apps.silent"
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
    char *silent = NULL;
    size_t slen = 0;
    if (fc_port_kv_get(KV_SILENT, &silent, &slen) == 0) {
        fc_port_kv_del(KV_SILENT);
        crash = 1;
    }
    free(silent);
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
                fc_port_log(FC_LOG_WARN, "app %s was starting when the device crashed or was left without heap (%d in a row)",
                            e->string, n);
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

/* How many of the agent's statuses were accepted (fc_apps_status_ok), 0
 * before the first: without an agent, an app is past its start on time
 * alone. A long, not 64 bits: Zephyr's ESP32-S3 has no 8-byte atomics. */
static long s_status_oks;

void fc_apps_status_ok(void)
{
    __atomic_add_fetch(&s_status_oks, 1, __ATOMIC_RELAXED);
}

/* On the app's thread: once it has been up GUARD_SETTLE_MS, and the agent
 * has had a status accepted since it started, it is past the start. An app
 * that leaves the agent no heap to reach Flight Control stays in its start
 * until the agent restarts the device (fc_apps_note_silent_restart). */
static void guard_tick(app_t *a, int64_t now)
{
    if (a->settled || now < a->settle_at_ms) {
        return;
    }
    long oks = __atomic_load_n(&s_status_oks, __ATOMIC_RELAXED);
    if (oks > 0 && oks == a->begun_oks) {
        a->settle_at_ms = now + 5000; /* not yet: look again */
        return;
    }
    a->settled = 1;
    guard_forget(a->name, 0);
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

/* The block of heap the linear memory that did not fit asked for, 0 unknown:
 * ESP-IDF's os_mmap wrap keeps it (port/espidf/fc_wasm_mem.c). Zephyr's
 * Espressif HAL defines ESP_PLATFORM too, without that file. */
#if defined(ESP_PLATFORM) && !defined(__ZEPHYR__)
long fc_wasm_mem_failed(void);
#define mem_failed_block() fc_wasm_mem_failed()
#else
#define mem_failed_block() 0L
#endif

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
 * the functions named here (check_env_import). Not malloc and the rest: the
 * app has its own (wasm-apps/sdk/libc/stdlib.h), in its linear memory. Of
 * math.h only expf, logf and tanhf, which emlearn's networks need; the others
 * an app needs most are WebAssembly instructions. The device registers them itself
 * (s_libc_natives), WAMR none: each that does work counts its time against
 * the call's limit in the device's functions (LIBC_TIMED); printf, puts and putchar write to the
 * app's log, a line at a time, as fc_log does at INFO (WAMR's write to the
 * serial port only); strtol and strtoul take a NULL endptr (WAMR's would
 * write at the app's address 0); abort takes no argument, as C's; strncmp
 * and strncpy, atoi, ctype and the math are the device's own (below). */
#if WASM_ENABLE_LIBC_BUILTIN
uint32_t get_libc_builtin_export_apis(NativeSymbol **p_libc_builtin_apis);

/* WAMR's snprintf, which formats from the app's va_list in its memory. */
typedef int (*wa_snprintf_t)(wasm_exec_env_t env, char *str, uint32_t size, const char *fmt, void *va);
static wa_snprintf_t s_wa_snprintf;

static void out_flush(app_t *a)
{
    if (a->out_len > 0) {
        fc_port_log(FC_LOG_INFO, "app %s: %.*s", a->name, a->out_len, a->io->out);
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
        a->io->out[a->out_len++] = s[i];
        if (a->out_len == (int)sizeof(a->io->out)) {
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

/* The device's own: WAMR's libc-builtin has no strrchr. */
static uint32_t env_strrchr(wasm_exec_env_t env, const char *s, int32_t c)
{
    int64_t t0 = fc_apps_now_us();
    const char *r = strrchr(s, c);
    fc_apps_native_time(env, wasm_runtime_get_user_data(env), t0);
    return r != NULL ? (uint32_t)wasm_runtime_addr_native_to_app(wasm_runtime_get_module_inst(env), (void *)r) : 0;
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

/* The math, timed as the string functions are: native, outside the
 * instruction budget, so a loop of them would otherwise not be stopped. */
static float env_expf(wasm_exec_env_t env, float x)
{
    LIBC_TIMED(float, expf(x));
}

static float env_logf(wasm_exec_env_t env, float x)
{
    LIBC_TIMED(float, logf(x));
}

static float env_tanhf(wasm_exec_env_t env, float x)
{
    LIBC_TIMED(float, tanhf(x));
}

/* Keep the registered symbols in strcmp order so WAMR can use flash.
 * A build without libc keeps only the names for import diagnostics. */
#define LIBC_NATIVE(name, sig) {#name, (void *)env_##name, sig, NULL}
static const NativeSymbol s_libc_natives[] = {
#else
#define LIBC_NATIVE(name, sig) {#name}
static const struct { const char *symbol; } s_libc_natives[] = {
#endif
    LIBC_NATIVE(abort, "()"),
    LIBC_NATIVE(atoi, "($)i"),
    LIBC_NATIVE(expf, "(f)f"),
    LIBC_NATIVE(isalnum, "(i)i"),
    LIBC_NATIVE(isalpha, "(i)i"),
    LIBC_NATIVE(isdigit, "(i)i"),
    LIBC_NATIVE(isgraph, "(i)i"),
    LIBC_NATIVE(isprint, "(i)i"),
    LIBC_NATIVE(isspace, "(i)i"),
    LIBC_NATIVE(isupper, "(i)i"),
    LIBC_NATIVE(isxdigit, "(i)i"),
    LIBC_NATIVE(logf, "(f)f"),
    LIBC_NATIVE(memchr, "(*ii)i"),
    LIBC_NATIVE(memcmp, "(**~)i"),
    LIBC_NATIVE(memcpy, "(**~)i"),
    LIBC_NATIVE(memmove, "(**~)i"),
    LIBC_NATIVE(memset, "(*ii)i"),
    LIBC_NATIVE(printf, "($*)i"),
    LIBC_NATIVE(putchar, "(i)i"),
    LIBC_NATIVE(puts, "($)i"),
    LIBC_NATIVE(snprintf, "(*~$*)i"),
    LIBC_NATIVE(sprintf, "($$*)i"),
    LIBC_NATIVE(strchr, "($i)i"),
    LIBC_NATIVE(strcmp, "($$)i"),
    LIBC_NATIVE(strcpy, "(*$)i"),
    LIBC_NATIVE(strcspn, "($$)i"),
    LIBC_NATIVE(strlen, "($)i"),
    LIBC_NATIVE(strncasecmp, "($$i)i"),
    LIBC_NATIVE(strncmp, "($$i)i"),
    LIBC_NATIVE(strncpy, "(*$i)i"),
    LIBC_NATIVE(strrchr, "($i)i"),
    LIBC_NATIVE(strspn, "($$)i"),
    LIBC_NATIVE(strstr, "($$)i"),
    LIBC_NATIVE(strtol, "($ii)i"),
    LIBC_NATIVE(strtoul, "($ii)i"),
    LIBC_NATIVE(tanhf, "(f)f"),
    LIBC_NATIVE(tolower, "(i)i"),
    LIBC_NATIVE(toupper, "(i)i"),
};
#undef LIBC_NATIVE

static int libc_function(const char *name)
{
    for (size_t i = 0; i < sizeof(s_libc_natives) / sizeof(s_libc_natives[0]); i++) {
        if (strcmp(name, s_libc_natives[i].symbol) == 0) {
            return 1;
        }
    }
    return 0;
}

#if WASM_ENABLE_LIBC_BUILTIN
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
        if (strcmp(im.module_name, "fc") != 0 && strcmp(im.module_name, "arduino") != 0) {
            snprintf(err, err_len, "imports from module \"%.30s\", which the device does not have", im.module_name);
            return -1;
        }
        if (im.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !im.linked) {
            snprintf(err, err_len, "imports %s.%.40s, which this firmware does not have", im.module_name, im.name);
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

/* What the agent takes of the heap after an app's instance is made: the
 * heap free then (heap_after, instantiate), and the lowest from then until
 * the agent has been idle ENTRY_QUIET_MS, its connection opened again and
 * its status sent (at most HEAP_NOTE_MS), are logged, for the floor
 * (s_heap_floor) to be checked against. The lowest leaves out the block a
 * TLS handshake holds meanwhile (fc_heap_hold.h), which the heap's own low,
 * logged beside it, counts. heap_watch: the stretch + 1, 0 none. */
#define HEAP_NOTE_MS 60000

/* And over the whole run of the last app whose instance was made, until a
 * module is loaded or that app stops: the agent's spec, its certificate
 * renewed, an export, a download, MQTT. Kept for the status
 * (fc_apps_heap_agent), and logged when lower by 1 KB. */
static pthread_mutex_t s_run_lock = PTHREAD_MUTEX_INITIALIZER;
static const app_t *s_run_app;
static int s_run_stretch = -1;
static long s_run_after = -1, s_run_low = -1, s_run_raw = -1, s_run_logged = -1;

static void run_low_end(const app_t *only)
{
    pthread_mutex_lock(&s_run_lock);
    if (s_run_stretch >= 0 && (only == NULL || only == s_run_app)) {
        long raw, low = fc_heap_low_end(s_run_stretch, &raw);
        if (low >= 0) {
            s_run_low = low;
            s_run_raw = raw;
        }
        s_run_stretch = -1;
        s_run_app = NULL;
    }
    pthread_mutex_unlock(&s_run_lock);
}

static void run_low_begin(const app_t *a)
{
    run_low_end(NULL);
    pthread_mutex_lock(&s_run_lock);
    s_run_stretch = fc_heap_low_begin();
    s_run_app = s_run_stretch >= 0 ? a : NULL;
    s_run_after = s_run_stretch >= 0 ? a->heap_after : -1;
    s_run_low = s_run_raw = s_run_logged = -1;
    pthread_mutex_unlock(&s_run_lock);
}

long fc_apps_heap_floor(void)
{
    return s_heap_floor;
}

int fc_apps_heap_agent(char *text, size_t len)
{
    pthread_mutex_lock(&s_run_lock);
    if (s_run_stretch >= 0) {
        long raw, low = fc_heap_low_peek(s_run_stretch, &raw);
        if (low >= 0) {
            s_run_low = low;
            s_run_raw = raw;
        }
    }
    long after = s_run_after, low = s_run_low, raw = s_run_raw;
    int log = low >= 0 && (s_run_logged < 0 || low < s_run_logged - 1024);
    if (log) {
        s_run_logged = low;
    }
    pthread_mutex_unlock(&s_run_lock);
    if (after < 0 || low < 0) {
        return 0;
    }
    if (log) {
        fc_port_log(FC_LOG_INFO, "heap: %ld KB free after the last instance, %ld KB at the lowest since (it took %ld KB; "
                    "%ld KB at the heap's own lowest; floor %ld KB)", after / 1024, low / 1024, (after - low) / 1024,
                    raw / 1024, s_heap_floor / 1024);
    }
    snprintf(text, len, "after=%ld low=%ld raw=%ld floor=%ld", after, low, raw, s_heap_floor);
    return 1;
}

static void heap_note_begin(app_t *a)
{
    a->heap_watch = a->heap_after >= 0 ? fc_heap_low_begin() + 1 : 0;
    a->heap_watch_ms = a->heap_quiet_ms = fc_apps_now_ms();
    if (a->heap_after >= 0) {
        run_low_begin(a);
    }
}

static void heap_note_end(app_t *a)
{
    if (!a->heap_watch) {
        return;
    }
    long raw, low = fc_heap_low_end(a->heap_watch - 1, &raw);
    a->heap_watch = 0;
    if (low >= 0) {
        fc_port_log(FC_LOG_INFO,
                    "app %s (%s): %ld KB of heap free after its instance, %ld KB at the lowest while the agent went on "
                    "(it took %ld KB; %ld KB at the heap's own lowest; floor %ld KB)",
                    a->name, a->start_why ? a->start_why : "restart", a->heap_after / 1024, low / 1024,
                    (a->heap_after - low) / 1024, raw / 1024, s_heap_floor / 1024);
    }
}

static void heap_note_tick(app_t *a, int64_t now)
{
    if (!a->heap_watch) {
        return;
    }
    if (fc_agent_busy()) {
        a->heap_quiet_ms = now;
    } else if (now - a->heap_quiet_ms >= ENTRY_QUIET_MS) {
        heap_note_end(a);
        return;
    }
    if (now - a->heap_watch_ms >= HEAP_NOTE_MS) {
        heap_note_end(a);
    }
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
        heap_note_tick(a, now);
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
    for (int i = 0; i < FC_MAX_APPS; i++) {
        const app_t *a = s_apps[i];
        long n = a ? __atomic_load_n(&a->stack_unused, __ATOMIC_RELAXED) : -1;
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
    pthread_mutex_lock(&a->sync->lock);
    fc_apps_drop_events(a, NULL, 0);
    a->accepting = 1;
    pthread_mutex_unlock(&a->sync->lock);
    fc_appapi_load_config(a);
    /* An app that starts with the broker there hears it, as one that was
     * running when it came. */
    fc_mqtt_stats_t mq;
    fc_mqtt_stats(&mq);
    if (mq.connected && fc_appapi_allowed(a, "mqtt")) {
        fc_apps_post(a, EV_NET, -1, 1);
    }

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
        heap_note_tick(a, now);
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
            app_handle_t *h = &a->io->h[i];
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
                a->msg = e.msg; /* fc_event_data, fc_mqtt_topic */
                ok = call_fc(a, env, on_event, 3, argv, "on_event") == 0;
                a->msg = NULL;
            }
            fc_mqtt_msg_free(e.msg);
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
    pthread_mutex_lock(&a->sync->lock);
    a->accepting = 0;
    fc_apps_drop_events(a, NULL, 0);
    pthread_mutex_unlock(&a->sync->lock);
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
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (s_apps[i] && s_apps[i]->module == m && pthread_equal(pthread_self(), s_apps[i]->thread)) {
            a = s_apps[i]; /* Our runner is joined before its slot is freed. */
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    if (a == NULL) {
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
    a->heap_after = -1;
    long before = fc_port_heap_free();
#ifdef FC_WASM_SRAM_PLACEMENT
    if (before >= 0) {
        before += (long)a->linear.size;
    }
    wasm_module_inst_t inst = fc_wasm_instantiate_reserved(&a->linear, a->module, WASM_STACK, ebuf, (uint32_t)elen);
#else
    wasm_module_inst_t inst = wasm_runtime_instantiate(a->module, WASM_STACK, 0, ebuf, (uint32_t)elen);
#endif
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
        a->mem_cost = before > left ? before - left : 0;
        return NULL;
    }
    if (inst != NULL && left >= 0) {
        fc_port_log(FC_LOG_INFO, "app %s: instance made, %ld KB of heap left for the agent", a->name, left / 1024);
    }
    a->heap_after = inst != NULL ? left : -1;
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
        if (inst == NULL && (short_of_heap || strstr(ebuf, "allocate") != NULL)) {
            if (a->primary && a->start_why && strcmp(a->start_why, "memory") == 0) {
                fc_port_log(FC_LOG_INFO, "app %s: memory retry before the agent settles: %s (%ld B free, largest %ld B)",
                            a->name, ebuf, fc_port_heap_free(), fc_port_heap_largest());
            }
            /* Made while the spec is applied, whose memory (the parsed
             * spec, the status after it) may be in the way: once more when
             * the agent is done. On an ESP32-C5 a restart for new env vars
             * found 58 KB in one piece, and 76 KB five seconds later. */
            __atomic_store_n(&a->made, 1, __ATOMIC_RELEASE);
            wait_quiet(a);
            if (!stopping(a)) {
                inst = instantiate(a, ebuf, sizeof(ebuf), &short_of_heap);
            }
        }
        if (inst != NULL) {
            heap_note_begin(a);
        }
        wasm_exec_env_t env = inst ? wasm_runtime_create_exec_env(inst, WASM_STACK) : NULL;
        const char *entry = env ? entry_of(a, inst) : NULL;
        pthread_mutex_lock(&s_lock);
        a->live = env != NULL;
        a->one_shot = entry != NULL;
        if (a->live) {
            a->relocated = false;
            a->retried_idle = false;
        }
        wasm_memory_inst_t memory = inst && a->primary ? wasm_runtime_get_default_memory(inst) : NULL;
        a->can_grow = memory && wasm_memory_get_cur_page_count(memory) < wasm_memory_get_max_page_count(memory);
        if (a->primary && a->live && !a->can_grow) {
            __atomic_store_n(&s_poll_event, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&s_lock);
        __atomic_store_n(&a->made, 1, __ATOMIC_RELEASE);
        if (inst == NULL || env == NULL) {
            const char *why = inst == NULL ? ebuf : "exec env: out of memory";
            /* Out of memory: how much would fit, to compare with what the
             * module asks for (its memory comes in one block). */
            long largest = fc_port_heap_largest();
            char wbuf[sizeof(ebuf) + 48];
            int no_block = inst == NULL && !short_of_heap && strstr(why, "allocate linear memory") != NULL;
            if (largest >= 0 && (strstr(why, "allocate") != NULL || strstr(why, "out of memory") != NULL)) {
                snprintf(wbuf, sizeof(wbuf), "%s (largest free block %ld KB)", why, largest / 1024);
                why = wbuf;
            }
            fc_port_log(FC_LOG_WARN, "app %s: %s", a->name, why);
            set_status(a, "Error", why);
            if ((short_of_heap || no_block) && !stopping(a)) {
                /* Twice, the second time with the agent idle. Not started
                 * again every few seconds: its module and its thread's stack
                 * stay where they are, so it found the same largest block
                 * each time (on an ESP32-C3, C5 and classic ESP32, 44, 42
                 * and 56 KB for CoreMark's 68), and a refused one would take
                 * the agent's heap each time. fc_apps_poll starts it again
                 * once the heap has room for it, as do a new image, new env
                 * vars, new firmware or a restart. */
                a->mem_wait = 1;
                a->mem_block = no_block ? mem_failed_block() : 0;
                if (!short_of_heap) {
                    a->mem_cost = 0;
                }
                a->mem_largest = largest;
                a->mem_next_ms = fc_apps_now_ms() + MEM_RETRY_MS;
                fc_port_log(FC_LOG_INFO, "app %s: started again when the heap has room for it", a->name);
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
                heap_note_end(a);
                wasm_runtime_destroy_exec_env(env);
                wasm_runtime_deinstantiate(inst);
                fc_appapi_close_all(a); /* what code run as it was instantiated opened */
                pthread_mutex_lock(&s_lock);
                a->live = 0;
                pthread_mutex_unlock(&s_lock);
                goto done;
            }
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
        heap_note_end(a);
        a->inst = NULL;
        if (env) {
            wasm_runtime_destroy_exec_env(env);
        }
        if (inst) {
            wasm_runtime_deinstantiate(inst);
        }
        fc_appapi_close_all(a); /* run_fc's are closed: what code run as it was instantiated opened */
        a->start_why = NULL; /* from now on, a restart */
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
                if (a->primary) {
                    while (!stopping(a)) {
                        if (pthread_mutex_trylock(&s_lifecycle) == 0) {
                            if (!stopping(a)) {
                                park_secondary(a->name, false);
                            }
                            pthread_mutex_unlock(&s_lifecycle);
                            break;
                        }
                        usleep(20 * 1000);
                    }
                }
            }
        }
    }
done:
#ifdef FC_WASM_SRAM_PLACEMENT
    fc_wasm_release_memory(&a->linear); /* stopped before its first instantiate */
#endif
    wasm_runtime_destroy_thread_env();
    __atomic_store_n(&a->runner_done, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_poll_event, 1, __ATOMIC_RELEASE);
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
            fc_heap_unhold(hold);
            hold = fc_heap_hold(THREAD_STACK + THREAD_SLACK);
        }
    }
    return hold;
}

/* Small, long-lived allocations should leave the largest block for WASM. */
static void *alloc_app_state(size_t size)
{
    void *hold = fc_heap_hold(0);
    void *p = calloc(1, size);
    if (hold != NULL) {
        fc_heap_unhold(hold);
        if (p == NULL) {
            p = calloc(1, size);
        }
    }
    return p;
}

/* The runner and hardware callbacks have ended. MQTT can still deliver an
 * old subscription ID, so detach the buffers under the slot's event lock. */
static void release_io(app_t *a)
{
    pthread_mutex_lock(&a->sync->lock);
    app_io_t *io = a->io;
    a->io = NULL;
    pthread_mutex_unlock(&a->sync->lock);
    free(io);
}

static void app_start(app_t *a, const char *why)
{
    if (a->module == NULL) {
        return; /* the status already says why */
    }
    if (!a->relocated) {
        a->retried_idle = false;
    }
#ifdef FC_WASM_SRAM_PLACEMENT
    a->linear = fc_wasm_reserve_memory(a->module);
#endif
    if (a->io == NULL) {
        app_io_t *io = NULL;
#ifdef FC_WASM_SRAM_PLACEMENT
        if (a->linear.data) {
            io = calloc(1, sizeof(*io));
            if (io == NULL) {
                fc_wasm_release_memory(&a->linear);
            }
        }
#endif
        if (io == NULL) {
            io = alloc_app_state(sizeof(*io));
        }
        if (!io) {
            a->mem_wait = 1;
            a->mem_cost = sizeof(*io);
            a->mem_block = 0;
            a->mem_next_ms = fc_apps_now_ms() + MEM_RETRY_MS;
            a->start_pending = NULL;
            set_status(a, "Preparing", "waiting for app handles and event queue memory");
            return;
        }
        pthread_mutex_lock(&a->sync->lock);
        a->io = io;
        pthread_mutex_unlock(&a->sync->lock);
    }
    set_stop(a, 0);
    a->start_why = why;
    a->start_pending = NULL;
    a->mem_wait = 0;
    __atomic_store_n(&a->runner_done, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->stack_unused, -1L, __ATOMIC_RELAXED);
    a->stack_at_ms = 0;
    a->begun_oks = __atomic_load_n(&s_status_oks, __ATOMIC_RELAXED);
    a->settle_at_ms = fc_apps_now_ms() + s_settle_ms;
    a->settled = 0;
    set_status(a, "Preparing", NULL);
    pthread_attr_init(&a->thread_attr);
    __atomic_store_n(&a->made, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->go, 0, __ATOMIC_RELAXED);
    /* With a known linear memory, reserve its block through instantiation.
     * The SDK can place the native stack in any remaining main SRAM.
     * Otherwise protect the largest block just while creating the thread. */
#ifdef FC_WASM_SRAM_PLACEMENT
    void *hold = a->linear.data ? NULL : hold_but_stack();
#else
    void *hold = hold_but_stack();
#endif
    int sized = pthread_attr_setstacksize(&a->thread_attr, THREAD_STACK) == 0;
    a->thread_running = sized && pthread_create(&a->thread, &a->thread_attr, runner, a) == 0;
    bool retry = hold != NULL;
    fc_heap_unhold(hold);
#ifdef FC_WASM_SRAM_PLACEMENT
    if (!a->thread_running && a->linear.data) {
        fc_wasm_release_memory(&a->linear);
        retry = true;
    }
#endif
    if (!a->thread_running && sized && retry) {
        a->thread_running = pthread_create(&a->thread, &a->thread_attr, runner, a) == 0;
    }
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
        release_io(a);
        a->mem_wait = sized;
        a->mem_cost = sizeof(*a->io);
        a->mem_block = THREAD_STACK;
        a->mem_next_ms = fc_apps_now_ms() + MEM_RETRY_MS;
        fc_port_log(FC_LOG_WARN, "app %s: cannot start thread (free %ld B, largest %ld B)", a->name,
                    fc_port_heap_free_now(), fc_port_heap_largest());
        set_status(a, "Error", "cannot start thread");
    }
}

static void join_runner(app_t *a)
{
    pthread_join(a->thread, NULL);
    pthread_attr_destroy(&a->thread_attr);
    a->thread_running = 0;
    __atomic_store_n(&a->stack_unused, -1L, __ATOMIC_RELAXED);
}

static void app_stop(app_t *a)
{
    a->start_pending = NULL;
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
        join_runner(a);
    }
    release_io(a);
    a->mem_wait = 0; /* after the join: its runner may set it until it ends */
    a->relocated = false;
    run_low_end(a);
    set_status(a, "Stopped", NULL);
}

/* The apps saved by a previous run, held at boot (fc_apps_hold_saved) and
 * started by fc_apps_start_saved. Started before the network, their memory
 * was taken while the heap was whole, before WiFi and the agent took
 * theirs: the floor always passed, and an ESP32-C5 with two apps saved was
 * left 7 KB, never reached Flight Control again and could not be given
 * another spec (v3.14-rc10.1). Held, they start once a status is accepted,
 * or after BOOT_APPS_WAIT_MS without one (a device that runs offline), with
 * the heap at regime under the floor's check. */
static int s_hold_saved;
static int64_t s_init_ms;

void fc_apps_hold_saved(int on)
{
    s_hold_saved = on;
}

int fc_apps_saved_delay_s(void)
{
    if (!s_hold_saved) {
        return -1;
    }
    int64_t left = s_init_ms + BOOT_APPS_WAIT_MS - fc_apps_now_ms();
    return __atomic_load_n(&s_status_oks, __ATOMIC_RELAXED) > 0 || left <= 0 ? 0 : (int)((left + 999) / 1000);
}

int fc_apps_start_saved(void)
{
    pthread_mutex_lock(&s_lifecycle);
    s_hold_saved = 0;
    int n = run_pending(1);
    pthread_mutex_unlock(&s_lifecycle);
    return n;
}

/* Before the agent restarts the device for want of an accepted status: if
 * the heap is below the floor now, or went below it since the last app's
 * instance, the restart counts against the apps still in their start
 * (guard_boot), as a crash does. */
void fc_apps_note_silent_restart(void)
{
    long free_now = fc_port_heap_free();
    long low = -1;
    pthread_mutex_lock(&s_run_lock);
    if (s_run_stretch >= 0) {
        low = fc_heap_low_peek(s_run_stretch, NULL);
    } else {
        low = s_run_low;
    }
    pthread_mutex_unlock(&s_run_lock);
    if ((free_now >= 0 && free_now < s_heap_floor) || (low >= 0 && low < s_heap_floor)) {
        fc_port_log(FC_LOG_WARN, "restart with the heap below the agent's floor (%ld KB free, %ld KB at the lowest): "
                    "the apps starting are blamed", free_now / 1024, low / 1024);
        fc_port_kv_set(KV_SILENT, "1", 1);
    }
}

/* Starts made by fc_apps_apply wait for fc_apps_start_pending (the agent):
 * see fc_apps.h. */
static int s_defer_starts;

void fc_apps_defer_starts(int on)
{
    s_defer_starts = on;
}

static void start_or_defer(app_t *a, const char *why)
{
    if (!s_defer_starts) {
        app_start(a, why);
        return;
    }
    a->start_pending = why;
    set_status(a, "Preparing", NULL);
}

static void park_app(app_t *a, const char *why, int64_t retry_at, bool keep_metrics)
{
    char detail[sizeof(a->error)] = "";
    if (retry_at) {
        /* The start failed and its runner, if any, has been joined. Save
         * the allocation error before app_stop changes the status. */
        pthread_mutex_lock(&s_lock);
        snprintf(detail, sizeof(detail), "%s", a->error);
        pthread_mutex_unlock(&s_lock);
    }
    app_release(a, keep_metrics);
    guard_forget(a->name, 0);
    a->parked = true;
    a->start_pending = why;
    a->mem_next_ms = retry_at;
    set_status(a, why ? "Preparing" : "Completed", detail);
}

/* Board-map restarts preserve the secondary’s counters; a primary update
 * or memory retry can reclaim them even if it is already parked. */
static void park_secondary(const char *primary_name, bool keep_metrics)
{
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (!a || strcmp(a->name, primary_name) == 0) {
            continue;
        }
        if (a->module == NULL) {
            if (!keep_metrics) {
                fc_metrics_app_drop(a->name);
            }
        } else {
            pthread_mutex_lock(&s_lock);
            int completed = strcmp(a->status, "Completed") == 0;
            pthread_mutex_unlock(&s_lock);
            park_app(a, completed ? NULL : "after primary", 0, keep_metrics);
        }
    }
}

static int start_pending_app(app_t *a, int boot_only)
{
    const char *why = a->start_pending;
    if (why == NULL || (boot_only && why != BOOT_WHY) || (s_hold_saved && why == BOOT_WHY)) {
        return 0;
    }
    if (a->module == NULL) {
        if (a->mem_next_ms > fc_apps_now_ms()) {
            return 0;
        }
        if (a->relocated && !mem_room(a)) {
            a->mem_next_ms = fc_apps_now_ms() + MEM_RETRY_MS;
            return 0;
        }
        int rc = load_saved(a);
        if (rc != 0) {
            if (rc > 0) {
                a->mem_next_ms = fc_apps_now_ms() + MEM_RETRY_MS;
            } else {
                a->start_pending = NULL;
                a->parked = false;
            }
            return 0;
        }
    }
    a->parked = false;
    app_start(a, why);
    if (!a->thread_running && a->mem_wait && !a->primary && primary_app() != NULL) {
        park_app(a, "memory", fc_apps_now_ms() + MEM_RETRY_MS, false);
        return 0;
    }
    return 1;
}

static int run_pending(int boot_only)
{
    int started = 0;
    const char *name = saved_primary_name();
    if (name != NULL) {
        restore_slots(name);
        if (primary_app() == NULL) {
            return 0;
        }
    }
    app_t *primary = primary_app();
    if (primary != NULL) {
        started += start_pending_app(primary, boot_only);
        if ((boot_only && s_defer_starts) || !admits_secondary(primary)) {
            return started;
        }
    }
    restore_slots(NULL);
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (s_apps[i] && s_apps[i] != primary) {
            started += start_pending_app(s_apps[i], boot_only);
        }
    }
    return started;
}

int fc_apps_start_pending(void)
{
    pthread_mutex_lock(&s_lifecycle);
    int n = run_pending(0);
    pthread_mutex_unlock(&s_lifecycle);
    return n;
}

/* Whether the heap now has room for an app whose instance did not fit.
 * Refused by the floor: what its instance took, its thread's stack and the
 * floor. No block for its memory: that block (or, not known, a larger one
 * than when it failed), and beside it the stack and the floor. */
static int mem_room(const app_t *a)
{
    long largest = fc_port_heap_largest(), free_now = fc_port_heap_free();
    long stack = THREAD_STACK + THREAD_SLACK;
    if (a->mem_cost > 0) {
        return (largest < 0 || largest >= a->mem_block) &&
               (free_now < 0 || free_now >= a->mem_cost + stack + s_heap_floor);
    }
    long block = a->mem_block > 0 ? a->mem_block : a->mem_largest >= 0 ? a->mem_largest + 4096 : 0;
    if (largest >= 0 && largest < block) {
        return 0;
    }
    return free_now < 0 || free_now >= block + stack + s_heap_floor;
}

/* A refusal gets an idle TLS turn. Unloading the module changes heap
 * layout. If it still cannot start, measure that new layout once more
 * without TLS: TLS itself may be occupying the required contiguous block. */
int fc_apps_primary_retry_due(void)
{
    pthread_mutex_lock(&s_lifecycle);
    app_t *a = primary_app();
    int due = a && (!a->thread_running || __atomic_load_n(&a->runner_done, __ATOMIC_ACQUIRE)) &&
              !a->retried_idle && fc_apps_now_ms() >= a->mem_next_ms &&
              (a->mem_wait || (a->relocated && a->parked && a->start_pending));
    pthread_mutex_unlock(&s_lifecycle);
    return due;
}

int fc_apps_poll_due(void)
{
    return __atomic_load_n(&s_poll_event, __ATOMIC_ACQUIRE) ||
           (s_poll_retry && fc_apps_now_ms() >= s_poll_retry);
}

/* While waiting for a stale console annotation, check the completion event
 * more often only when a one-shot can give the heap back to a waiting app. */
int fc_apps_awaiting_primary_completion(void)
{
    pthread_mutex_lock(&s_lifecycle);
    app_t *primary = primary_app();
    pthread_mutex_lock(&s_lock);
    int one_shot = primary && primary->live && primary->one_shot;
    pthread_mutex_unlock(&s_lock);
    int waiting = one_shot && s_restore != NULL;
    for (int i = 0; one_shot && !waiting && i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        waiting = a && a != primary && a->parked && a->start_pending;
    }
    pthread_mutex_unlock(&s_lifecycle);
    return waiting;
}

int fc_apps_poll(int idle_released)
{
    __atomic_exchange_n(&s_poll_event, 0, __ATOMIC_ACQUIRE);
    s_poll_retry = 0;
    pthread_mutex_lock(&s_lifecycle);
    bool restoring = s_restore != NULL;
    bool primary_completed = false;
    app_t *primary = primary_app();
    int64_t now = fc_apps_now_ms();
    if (idle_released && primary && primary->relocated && primary->parked && !primary->module) {
        primary->retried_idle = true;
        fc_port_log(FC_LOG_INFO, "app %s: retrying its saved module (%ld bytes free, largest %ld)",
                    primary->name, fc_port_heap_free(), fc_port_heap_largest());
    }
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (!a || (a->thread_running && !__atomic_load_n(&a->runner_done, __ATOMIC_ACQUIRE))) {
            continue;
        }
        if (a->thread_running) {
            join_runner(a);
            release_io(a);
            if (a == primary && strcmp(a->status, "Completed") == 0) {
                primary_completed = true;
            }
        }
        if (!a->module || !a->mem_wait || (primary && a != primary && !admits_secondary(primary))) {
            continue;
        }
        if (primary && a != primary) {
            park_app(a, "memory", now + MEM_RETRY_MS, false);
            continue;
        }
        if (now < a->mem_next_ms) {
            continue;
        }
        if (a == primary) {
            a->retried_idle |= idle_released;
            park_secondary(a->name, false);
        }
        if (!mem_room(a)) {
            if (a == primary && idle_released && a->mem_block > 0 && !a->relocated) {
                /* The loaded module can itself divide the heap. It has no
                 * running instance. Let its blocks coalesce before loading
                 * the same committed module once more from flash. */
                fc_port_log(FC_LOG_INFO, "app %s: unloading its module after memory refusal (%ld bytes free, largest %ld)",
                            a->name, fc_port_heap_free(), fc_port_heap_largest());
                park_app(a, "memory", now, true);
                a->relocated = true;
                a->retried_idle = false;
            } else {
                a->mem_next_ms = now + MEM_RETRY_MS;
            }
            continue;
        }
        long largest = fc_port_heap_largest();
        if (largest >= 0) {
            fc_port_log(FC_LOG_INFO, "app %s: the heap has room for it again (largest free block %ld KB), starting it",
                        a->name, largest / 1024);
        } else {
            fc_port_log(FC_LOG_INFO, "app %s: the heap has room for it again, starting it", a->name);
        }
        app_start(a, "memory");
    }
    if (primary_completed) {
        /* Finishing a one-shot releases its linear memory. A secondary
         * parked for insufficient heap can recheck on this event once. */
        for (int i = 0; i < FC_MAX_APPS; i++) {
            app_t *a = s_apps[i];
            if (a && a != primary && a->parked && a->start_pending && a->mem_next_ms > now && mem_room(a)) {
                a->mem_next_ms = now;
            }
        }
    }
    if ((primary || restoring) && !s_hold_saved) {
        run_pending(0);
    }
    int ready = saved_primary_name() == NULL && admits_secondary(primary_app());
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (a && (a == primary || ready) &&
            (!a->thread_running || __atomic_load_n(&a->runner_done, __ATOMIC_ACQUIRE)) &&
            (a->mem_wait || a->start_pending) && a->mem_next_ms > 0 &&
            (!s_poll_retry || a->mem_next_ms < s_poll_retry)) {
            s_poll_retry = a->mem_next_ms;
        }
    }
    pthread_mutex_unlock(&s_lifecycle);
    return ready;
}

/* Stops the app and unloads its module (after the runner is joined). */
static void app_release(app_t *a, bool keep_metrics)
{
    app_stop(a);
    if (!keep_metrics) {
        fc_metrics_app_drop(a->name);
    }
    pthread_mutex_lock(&s_lock);
    wasm_module_t module = a->module;
    a->module = NULL;
    pthread_mutex_unlock(&s_lock);
    if (module != NULL) {
        wasm_runtime_unload(module);
    }
}

/* ---- env vars ---------------------------------------------------------------- */

static const char *env_value(const cJSON *env, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(env, key));
    return s ? s : "";
}

static int is_primary(const cJSON *env)
{
    return strcmp(env_value(env, "FC_PRIMARY"), "true") == 0;
}

/* Takes the app's envVars (JSON) and what the device reads in them. */
static void set_env(app_t *a, char *json, const cJSON *env)
{
    free(a->env_json);
    a->env_json = json;
    pthread_mutex_lock(&s_lock); /* mqtt_on_link reads the grants */
    snprintf(a->allow, sizeof(a->allow), "%s", env_value(env, "FC_ALLOW"));
    pthread_mutex_unlock(&s_lock);
    snprintf(a->entry, sizeof(a->entry), "%s", env_value(env, "FC_ENTRY"));
    a->primary = is_primary(env);
}

static char *env_of(const cJSON *app)
{
    const cJSON *e = cJSON_GetObjectItem(app, "envVars");
    return cJSON_IsObject(e) ? cJSON_PrintUnformatted(e) : strdup("{}");
}

/* ---- persistence ----------------------------------------------------------- */

/* Two resident apps share a third flash slot while replacing one module.
 * Catalogs from earlier firmware have no moduleSlot and use app:<name>. */
#define FLASH_SLOTS (FC_MAX_APPS + 1)

static int module_slot(const cJSON *entry)
{
    const cJSON *slot = cJSON_GetObjectItem(entry, "moduleSlot");
    return cJSON_IsNumber(slot) ? slot->valueint : 0;
}

static void wasm_key(const char *name, int slot, char *out, size_t len)
{
    if (slot) {
        snprintf(out, len, "wasm.%d", slot);
    } else {
        snprintf(out, len, "app:%s", name);
    }
}

static void delete_module(const char *name, int slot)
{
    char key[64];
    wasm_key(name, slot, key, sizeof(key));
    if (fc_port_kv_del(key) != 0) {
        fc_port_log(FC_LOG_WARN, "cannot reclaim app module %s", key);
    }
}

/* A storage error can arrive after the catalog rename. Leave both modules
 * intact. Before applying another spec, make the running catalog durable
 * again. After a reboot the catalog actually on flash is authoritative. */
static char *s_catalog_rollback;

static int recover_catalog(void)
{
    if (!s_catalog_rollback) {
        return 0;
    }
    if (fc_port_kv_set(KV_APPS, s_catalog_rollback, strlen(s_catalog_rollback)) != 0) {
        return -1;
    }
    free(s_catalog_rollback);
    s_catalog_rollback = NULL;
    return 0;
}

static cJSON *read_catalog(char **previous)
{
    char *s = NULL;
    size_t len = 0;
    int rc = fc_port_kv_get(KV_APPS, &s, &len);
    if (rc == FC_KV_MISSING) {
        s = strdup("[]");
    }
    cJSON *list = s ? cJSON_Parse(s) : NULL;
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(list);
        list = NULL;
    }
    if (list && previous) {
        *previous = s;
    } else {
        free(s);
    }
    return list;
}

static int write_catalog(const cJSON *list, char **previous)
{
    char *s = cJSON_PrintUnformatted(list);
    if (!s) {
        return -1;
    }
    int rc = fc_port_kv_set(KV_APPS, s, strlen(s));
    free(s);
    if (rc != 0) {
        s_catalog_rollback = *previous;
        *previous = NULL;
    }
    return rc;
}

static bool flash_slot_used(const cJSON *list, int slot)
{
    const cJSON *o;
    cJSON_ArrayForEach(o, list) {
        if (module_slot(o) == slot) {
            return true;
        }
    }
    return false;
}

static int free_flash_slot(const cJSON *list)
{
    for (int slot = 1; slot <= FLASH_SLOTS; slot++) {
        if (!flash_slot_used(list, slot)) {
            return slot;
        }
    }
    return -1;
}

/* The old entry and bytes stay selected through every failure before the
 * catalog commit. Keep the catalog's other entries, including parked apps
 * that have no descriptor. Called with s_lifecycle held. */
static int persist_app(const char *name, const char *image, const char *digest, int tick,
                       const char *env, bool primary, unsigned char *code, size_t len,
                       uint8_t *slot_out, char *err, size_t err_len)
{
    char *previous = NULL;
    cJSON *list = read_catalog(&previous);
    cJSON *old = NULL, *o;
    cJSON_ArrayForEach(o, list) {
        if (strcmp(env_value(o, "name"), name) == 0) {
            old = o;
        } else if (primary) {
            cJSON *role = cJSON_GetObjectItem(o, "primary");
            if (cJSON_IsTrue(role)) {
                cJSON_SetBoolValue(role, 0);
            }
        }
    }
    int old_slot = old ? module_slot(old) : -1;
    int slot = code ? free_flash_slot(list) : old_slot;
    int rc = -1;
    bool module_write_failed = false;
    if (!list || slot < 0 || (!old && cJSON_GetArraySize(list) >= FC_MAX_APPS)) {
        goto done;
    }
    cJSON *next = cJSON_CreateObject();
    if (!next || !cJSON_AddStringToObject(next, "name", name) || !cJSON_AddStringToObject(next, "image", image) ||
        !cJSON_AddStringToObject(next, "digest", digest) || !cJSON_AddNumberToObject(next, "tick_ms", tick) ||
        !cJSON_AddBoolToObject(next, "primary", primary) || !cJSON_AddStringToObject(next, "env", env) ||
        !cJSON_AddNumberToObject(next, "moduleSlot", slot)) {
        cJSON_Delete(next);
        goto done;
    }
    if (old) {
        cJSON_ReplaceItemViaPointer(list, old, next);
    } else {
        cJSON_AddItemToArray(list, next);
    }
    bool staged = code != NULL;
    if (staged) {
        char key[64];
        wasm_key(name, slot, key, sizeof(key));
        if (fc_port_kv_set(key, code, len) != 0) {
            module_write_failed = true;
            goto done;
        }
        free(code);
        code = NULL;
    }
    rc = write_catalog(list, &previous);
    if (rc == 0) {
        *slot_out = slot;
        if (staged && old_slot >= 0) {
            delete_module(name, old_slot);
        }
    }
done:
    if (module_write_failed) {
        snprintf(err, err_len, "app %s: cannot store the module (%u bytes)", name, (unsigned)len);
    } else if (rc != 0) {
        snprintf(err, err_len, "app %s: cannot store the app catalog", name);
    }
    cJSON_Delete(list);
    free(previous);
    free(code);
    return rc;
}

static app_t *find(const char *name)
{
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (s_apps[i] && strcmp(s_apps[i]->name, name) == 0) {
            return s_apps[i];
        }
    }
    return NULL;
}

static int free_slot(int reserved)
{
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (s_apps[i] == NULL && reserved-- == 0) {
            return i;
        }
    }
    return -1;
}

/* Publish only after the caller has filled in the app's identity. */
static app_t *new_slot(int i)
{
    app_t *a = alloc_app_state(sizeof(*a));
    if (a != NULL) {
        a->slot = i;
        a->stack_unused = -1;
        a->sync = &s_sync[i];
    }
    return a;
}

/* Called after app_release joined its runner and closed every handle. */
static void drop_slot(app_t *a)
{
    pthread_mutex_lock(&s_lock);
    s_apps[a->slot] = NULL;
    pthread_mutex_unlock(&s_lock);
    free(a->env_json);
    free(a);
}

#ifdef FC_WASM_SRAM_PLACEMENT
static _Thread_local void **s_load_hold;

/* Called by WAMR after it has validated the memory declaration, before it
 * allocates the decoded bytecode. Leave the surplus main SRAM available. */
struct WASMModule;
void fc_wasm_before_bytecode(struct WASMModule *module)
{
    if (s_load_hold && *s_load_hold) {
        size_t size = fc_wasm_block_size(fc_wasm_initial_memory((wasm_module_t)module));
        if (size) {
            *s_load_hold = fc_heap_trim_hold(*s_load_hold, size);
        }
    }
}
#endif

/* Loads a module and checks it is an app this firmware runs. */
static wasm_module_t load_app(unsigned char *code, size_t len, const char *entry, char *ebuf, size_t elen)
{
    /* Place the module's pieces around the largest block. Leave that
     * block available for linear memory when loading from flash or OCI. */
    run_low_end(NULL);
    void *hold = fc_heap_hold(0);
#ifdef FC_WASM_SRAM_PLACEMENT
    s_load_hold = &hold;
#endif
    wasm_module_t m = load_module(code, len, ebuf, elen);
#ifdef FC_WASM_SRAM_PLACEMENT
    s_load_hold = NULL;
#endif
    if (hold != NULL) {
        fc_heap_unhold(hold);
        if (m == NULL) {
            m = load_module(code, len, ebuf, elen);
        }
    }
    if (m != NULL && check_module(m, entry, ebuf, elen) != 0) {
        wasm_runtime_unload(m);
        m = NULL;
    }
    return m;
}

static int load_saved(app_t *a)
{
    char key[64], ebuf[96] = "cannot read the saved module";
    char *code = NULL;
    size_t len = 0;
    size_t limit = !a->primary && primary_app() ? wasm_limit() : SIZE_MAX;
    if (limit == 0) {
        return 1;
    }
    wasm_key(a->name, a->module_slot, key, sizeof(key));
    void *hold = a->relocated ? fc_heap_hold(0) : NULL;
    int read_rc = fc_port_kv_get(key, &code, &len);
    if (hold != NULL) {
        fc_heap_unhold(hold);
        if (read_rc == -1) {
            read_rc = fc_port_kv_get(key, &code, &len);
        }
    }
    if (read_rc == 0) {
        if (len > limit) {
            free(code);
            return 1;
        }
        guard_begin(a->name, a->image, a->env_json, 1);
        wasm_module_t module = load_app((unsigned char *)code, len, a->entry, ebuf, sizeof(ebuf));
        pthread_mutex_lock(&s_lock);
        a->module = module;
        pthread_mutex_unlock(&s_lock);
        guard_loaded();
        free(code);
    }
    if (a->module == NULL) {
        guard_forget(a->name, 0);
        if ((a->relocated && read_rc == -1) ||
            ((limit != SIZE_MAX || a->relocated) && (strstr(ebuf, "allocate") || strstr(ebuf, "out of memory")))) {
            return 1;
        }
        set_status(a, "Error", ebuf);
        return -1;
    }
    return 0;
}

static void restore_slot(app_t *a, const cJSON *o)
{
    const char *name = env_value(o, "name");
    const char *image = env_value(o, "image");
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->image, sizeof(a->image), "%s", image);
    snprintf(a->digest, sizeof(a->digest), "%s", env_value(o, "digest"));
    a->module_slot = module_slot(o);
    const cJSON *tick = cJSON_GetObjectItem(o, "tick_ms");
    a->tick_ms = cJSON_IsNumber(tick) && tick->valueint >= 10 && tick->valueint <= 60000 ? tick->valueint : 100;
    cJSON *saved_env = cJSON_GetObjectItem(o, "env");
    char *json = cJSON_IsString(saved_env) ? saved_env->valuestring : strdup("{}");
    if (cJSON_IsString(saved_env)) {
        saved_env->valuestring = NULL; /* transfer the catalog string to the slot */
    }
    cJSON *env = cJSON_Parse(json);
    if (env == NULL) {
        free(json);
        set_status(a, "Error", "cannot read saved env vars");
        return;
    }
    set_env(a, json, env);
    const cJSON *role = cJSON_GetObjectItem(o, "primary");
    if (cJSON_IsBool(role)) {
        a->primary = cJSON_IsTrue(role);
    }
    cJSON_Delete(env);
    if (guard_is_bad(a->name, a->image, a->env_json)) {
        /* Kept, so the status says why; the spec does not start it again. */
        set_status(a, "Error", GUARD_ERROR);
        return;
    }
    a->start_pending = BOOT_WHY;
    set_status(a, "Preparing", NULL);
}

/* Keep an entry in the catalog if allocating its descriptor failed. It can
 * still be removed by a spec, and the next poll can restore it offline. */
static const cJSON *waiting_slot(const char *name)
{
    const cJSON *o;
    cJSON_ArrayForEach(o, s_restore) {
        if (strcmp(env_value(o, "name"), name) == 0) {
            return o;
        }
    }
    return NULL;
}

static const char *saved_primary_name(void)
{
    const cJSON *o;
    cJSON_ArrayForEach(o, s_restore) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(o, "primary"))) {
            return env_value(o, "name");
        }
    }
    return NULL;
}

static void restore_slots(const char *only)
{
    if (!s_restore) {
        return;
    }
    bool single = only != NULL;
    for (cJSON *o = s_restore->child, *next; o != NULL; o = next) {
        next = o->next;
        const char *name = env_value(o, "name");
        if (only && strcmp(name, only) != 0) {
            continue;
        }
        int i = free_slot(0);
        app_t *a = NULL;
        if (*name && strlen(name) < sizeof(s_apps[0]->name) && *env_value(o, "image") && i >= 0 && !find(name)) {
            a = new_slot(i);
            if (a == NULL) {
                break;
            }
            restore_slot(a, o);
        }
        pthread_mutex_lock(&s_lock);
        if (a) {
            s_apps[i] = a;
        }
        cJSON_DetachItemViaPointer(s_restore, o);
        pthread_mutex_unlock(&s_lock);
        cJSON_Delete(o);
        if (single) {
            break;
        }
    }
    pthread_mutex_lock(&s_lock);
    if (s_restore->child == NULL) {
        cJSON_Delete(s_restore);
        s_restore = NULL;
    }
    pthread_mutex_unlock(&s_lock);
}

int fc_apps_init(void)
{
    if (s_inited) {
        return 0;
    }
    s_init_ms = fc_apps_now_ms();
    static int sync_inited;
    if (!sync_inited) {
        for (int i = 0; i < FC_MAX_APPS; i++) {
            pthread_mutex_init(&s_sync[i].lock, NULL);
            pthread_cond_init(&s_sync[i].cond, NULL);
        }
        sync_inited = 1;
    }
    s_restore = read_catalog(NULL);
    if (!s_restore) {
        return -1;
    }
    RuntimeInitArgs init;
    memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init)) {
        cJSON_Delete(s_restore);
        s_restore = NULL;
        fc_port_log(FC_LOG_ERROR, "WAMR init failed");
        return -1;
    }
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
    int n_fc = 0;
    NativeSymbol *fc = fc_appapi_natives(&n_fc);
    wasm_runtime_register_natives("fc", fc, (uint32_t)n_fc);
    NativeSymbol *ard = fc_arduino_natives(&n_fc); /* wasm3-arduino's examples (fc_arduino.c) */
    wasm_runtime_register_natives("arduino", ard, (uint32_t)n_fc);
    wasm_runtime_set_instantiation_hook(instantiation_hook);
    wasm_runtime_register_natives_raw("env", (NativeSymbol *)s_env_natives,
                                      sizeof(s_env_natives) / sizeof(s_env_natives[0]));
    libc_init();
    fc_board_load(g_fc_hw);
    s_list_revision++;
    s_inited = 1;
    guard_boot();

    /* Reclaim staging bytes left by loss of power on either side of a
     * catalog commit. Never touch a slot referenced by a saved app. */
    for (int slot = 1; slot <= FLASH_SLOTS; slot++) {
        if (!flash_slot_used(s_restore, slot)) {
            delete_module(NULL, slot);
        }
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, s_restore) {
        if (module_slot(entry)) {
            delete_module(env_value(entry, "name"), 0);
        }
    }
    /* Leave a saved secondary in the catalog until the primary has its
     * instance. This works regardless of the order at the last shutdown. */
    restore_slots(saved_primary_name());
    /* Legacy specs load modules before the network. With a primary, only
     * its module may occupy the heap before its instance has its memory. */
    app_t *primary = primary_app();
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (s_hold_saved && a && a->start_pending && (!primary || a == primary) && load_saved(a) != 0) {
            a->start_pending = NULL;
        }
    }
    if (!s_hold_saved) {
        fc_apps_start_saved();
    }
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
    *module = load_app(*out, *len, entry, ebuf, sizeof(ebuf));
    if (*module == NULL) {
        snprintf(err, err_len, "%s: not a loadable WASM module: %s", image, ebuf);
        free(*out);
        *out = NULL;
        return 1; /* permanent */
    }
    return 0;
}

static int spec_contains(const cJSON *applications, const char *name)
{
    const cJSON *app;
    cJSON_ArrayForEach(app, applications) {
        if (strcmp(env_value(app, "name"), name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void forget_saved(const char *name, int slot)
{
    fc_port_log(FC_LOG_INFO, "removing app %s", name);
    guard_forget(name, 1);
    char key[64];
    delete_module(name, slot);
    fc_appapi_kv_key(name, key, sizeof(key));
    fc_port_kv_del(key);
}

static int remove_absent(const cJSON *applications)
{
    bool absent = false;
    for (int i = 0; i < FC_MAX_APPS; i++) {
        absent |= s_apps[i] && !spec_contains(applications, s_apps[i]->name);
    }
    const cJSON *entry;
    cJSON_ArrayForEach(entry, s_restore) {
        absent |= !spec_contains(applications, env_value(entry, "name"));
    }
    if (!absent) {
        return 0;
    }
    char *previous = NULL;
    cJSON *list = read_catalog(&previous);
    if (!list) {
        return -1;
    }
    for (cJSON *o = list->child, *next; o; o = next) {
        next = o->next;
        if (!spec_contains(applications, env_value(o, "name"))) {
            cJSON_Delete(cJSON_DetachItemViaPointer(list, o));
        }
    }
    int rc = write_catalog(list, &previous);
    free(previous);
    cJSON_Delete(list);
    if (rc != 0) {
        return -1;
    }
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (a && !spec_contains(applications, a->name)) {
            app_release(a, false);
            forget_saved(a->name, a->module_slot);
            drop_slot(a);
        }
    }
    for (cJSON *o = s_restore ? s_restore->child : NULL, *next; o; o = next) {
        next = o->next;
        const char *name = env_value(o, "name");
        if (!spec_contains(applications, name)) {
            forget_saved(name, module_slot(o));
            pthread_mutex_lock(&s_lock);
            cJSON_DetachItemViaPointer(s_restore, o);
            pthread_mutex_unlock(&s_lock);
            cJSON_Delete(o);
        }
    }
    return 0;
}

static int apply_app(const cJSON *app, const cJSON *primary, char *err, size_t err_len, int *retry)
{
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(app, "name"));
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(app, "appType"));
    const char *image = cJSON_GetStringValue(cJSON_GetObjectItem(app, "image"));
    if (name == NULL || type == NULL || strcmp(type, "container") != 0 || image == NULL) {
        snprintf(err, err_len, "app %s: only container apps with a WASM image are supported",
                 name ? name : "?");
        return -1;
    }
    app_t *a = find(name);
    int tick = tick_ms_of(app);
    char *env = env_of(app);
    if (env == NULL) {
        snprintf(err, err_len, "app %s: out of memory reading envVars", name);
        *retry = 1;
        return -1;
    }
    const cJSON *waiting = waiting_slot(name);
    if (waiting != NULL) {
        if (primary && app != primary && !admits_secondary(primary_app())) {
            if (strcmp(image, env_value(waiting, "image")) == 0 &&
                strcmp(env, env_value(waiting, "env")) == 0) {
                free(env); /* This exact version is already installed. */
                return 0;
            }
            goto wait_primary;
        }
        restore_slots(name);
        a = find(name);
        if (a == NULL) {
            goto no_slot_memory;
        }
    }
    if (guard_is_bad(name, image, env)) {
        /* Neither downloaded nor loaded again: an error in the status,
         * not retried until the image, the env vars or the firmware
         * change. A version that runs, if any, keeps running. */
        snprintf(err, err_len, "app %s: %s", name, GUARD_ERROR);
        int slot = free_slot(cJSON_GetArraySize(s_restore));
        if (a == NULL && slot >= 0 && (a = new_slot(slot)) != NULL) {
            pthread_mutex_lock(&s_lock);
            s_apps[a->slot] = a;
            snprintf(a->name, sizeof(a->name), "%s", name);
            snprintf(a->image, sizeof(a->image), "%s", image);
            pthread_mutex_unlock(&s_lock);
            set_env(a, env, cJSON_GetObjectItem(app, "envVars"));
            env = NULL;
        }
        if (a != NULL && a->module == NULL) {
            set_status(a, "Error", GUARD_ERROR);
        }
        free(env);
        return -1;
    }
    /* Same image: nothing to do, unless its saved module failed to load
     * at boot, in which case it is downloaded again. */
    int same_image = a && strcmp(a->image, image) == 0 && (a->module || a->start_pending || a->parked);
    int same_env = a && a->primary == (app == primary) && strcmp(env, a->env_json ? a->env_json : "{}") == 0;
    if (app == primary && !(same_image && same_env)) {
        park_secondary(name, false);
    }
    if (same_image) {
        if (!same_env) {
            if (persist_app(name, image, a->digest, tick, env, app == primary, NULL, 0,
                            &a->module_slot, err, err_len) != 0) {
                free(env);
                *retry = 1;
                return -1;
            }
            /* What it may open, or how often it ticks: start again. */
            fc_port_log(FC_LOG_INFO, "app %s: new env vars, restarting it", a->name);
            app_stop(a);
            set_env(a, env, cJSON_GetObjectItem(app, "envVars"));
            env = NULL;
            a->tick_ms = tick;
            if (a->module != NULL) {
                guard_begin(a->name, a->image, a->env_json, 0);
                start_or_defer(a, "new env vars");
            } else {
                a->start_pending = "new env vars";
                set_status(a, "Preparing", NULL);
            }
        }
        free(env);
        return 0;
    }
    /* Let the primary take its block with the spec freed. The next poll
     * delivers the secondary image, preserving a stored older version. */
    if (primary && app != primary && !admits_secondary(primary_app())) {
        goto wait_primary;
    }
    int slot = a ? a->slot : free_slot(cJSON_GetArraySize(s_restore));
    if (slot < 0) {
        snprintf(err, err_len, "app %s: too many apps (max %d)", name, FC_MAX_APPS);
        free(env);
        return -1;
    }
    bool installed = a != NULL;
    if (!installed && (a = new_slot(slot)) == NULL) {
        goto no_slot_memory;
    }
    /* A version with no instance (it could not start, waits to restart,
     * waits to run once, or ran to its end) is released before the new
     * one is loaded: kept, it would split the heap's largest block, where
     * the new module goes, and a 64 KB linear memory then no longer fit
     * on a classic ESP32 (it failed at every restart until the app was
     * removed and given again). One that runs keeps running until the
     * new one is ready. */
    if (a->module != NULL) {
        pthread_mutex_lock(&s_lock);
        int idle = !a->live;
        pthread_mutex_unlock(&s_lock);
        if (idle) {
            fc_port_log(FC_LOG_INFO, "app %s: not running, released before %s is loaded", name, image);
            app_release(a, false);
        }
    }
    unsigned char *code = NULL;
    size_t len = 0;
    wasm_module_t module = NULL;
    char digest[72], ferr[160];
    guard_begin(name, image, env, 1); /* a module that crashes as it loads is this app's */
    const char *entry = env_value(cJSON_GetObjectItem(app, "envVars"), "FC_ENTRY");
    int f = fetch_module(image, entry, &code, &len, &module, digest, sizeof(digest), ferr, sizeof(ferr));
    guard_loaded();
    if (f != 0) {
        guard_forget(name, 0);
        snprintf(err, err_len, "app %s: %s", name, ferr);
        if (!installed) {
            free(a);
        } else if (a->module == NULL) {
            set_status(a, "Error", ferr); /* nothing runs: say why */
        }
        if (f < 0) {
            *retry = 1;
        }
        free(env);
        return -1; /* the running version, if any, keeps running */
    }
    if (persist_app(name, image, digest, tick, env, app == primary, code, len,
                    &a->module_slot, err, err_len) != 0) {
        fc_port_log(FC_LOG_WARN, "%s", err);
        if (!installed) {
            free(a);
        } else if (a->module == NULL) {
            set_status(a, "Error", err);
        }
        guard_forget(name, 0);
        wasm_runtime_unload(module);
        free(env);
        *retry = 1;
        return -1; /* the running version, if any, keeps running */
    }
    if (installed) {
        fc_port_log(FC_LOG_INFO, "updating app %s: %s -> %s", name, a->image, image);
        app_release(a, false);
    } else {
        fc_port_log(FC_LOG_INFO, "installing app %s (%s, %u bytes)", name, image, (unsigned)len);
    }
    pthread_mutex_lock(&s_lock);
    s_apps[a->slot] = a;
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->image, sizeof(a->image), "%s", image);
    snprintf(a->digest, sizeof(a->digest), "%s", digest);
    a->state[0] = '\0';
    a->restarts = 0;
    a->module = module;
    pthread_mutex_unlock(&s_lock);
    a->parked = false;
    a->tick_ms = tick;
    set_env(a, env, cJSON_GetObjectItem(app, "envVars"));
    start_or_defer(a, "install");
    return 0;

wait_primary:
    snprintf(err, err_len, "app %s: waiting for the primary app's memory", name);
    if (*retry == 0) {
        *retry = FC_APPS_WAIT_PRIMARY;
    }
    free(env);
    return -1;

no_slot_memory:
    snprintf(err, err_len, "app %s: out of memory for its slot", name);
    *retry = 1;
    free(env);
    return -1;
}

/* Validate the names before they decide which saved apps get removed. */
static int check_list(const cJSON *apps, const cJSON **primary)
{
    *primary = NULL;
    if (apps != NULL && !cJSON_IsArray(apps)) {
        return -1;
    }
    const cJSON *app;
    cJSON_ArrayForEach(app, apps) {
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(app, "name"));
        const char *type = env_value(app, "appType");
        if (name == NULL || !*name || strlen(name) >= sizeof(s_apps[0]->name) || !*type ||
            (strcmp(type, "container") == 0 && !*env_value(app, "image"))) {
            return -1;
        }
        if (is_primary(cJSON_GetObjectItem(app, "envVars"))) {
            if (*primary != NULL) {
                return -1;
            }
            *primary = app;
        }
        for (const cJSON *other = apps->child; other != app; other = other->next) {
            if (strcmp(name, env_value(other, "name")) == 0) {
                return -1;
            }
        }
    }
    return 0;
}

int fc_apps_prepare(const cJSON *applications, fc_apps_plan_t *plan)
{
    if (check_list(applications, &plan->primary) != 0) {
        return -1;
    }
    pthread_mutex_lock(&s_lifecycle);
    plan->applications = applications;
    plan->revision = s_list_revision;
    int rc = recover_catalog();
    if (s_inited && rc == 0) {
        rc = remove_absent(applications);
    }
    pthread_mutex_unlock(&s_lifecycle);
    return rc == 0 ? 0 : FC_APPS_CATALOG_ERROR;
}

static int apply_list(const cJSON *applications, const fc_apps_plan_t *plan, char *err, size_t err_len, int *retry)
{
    *retry = 0;
    if (!s_inited) {
        snprintf(err, err_len, "WASM runtime not available");
        return -1;
    }
    const cJSON *primary, *app;
    if (plan != NULL) {
        primary = plan->primary;
    } else if (check_list(applications, &primary) != 0) {
        snprintf(err, err_len, "invalid application list");
        return -1;
    }
    pthread_mutex_lock(&s_lifecycle);
    if (recover_catalog() != 0 ||
        ((plan == NULL || plan->revision != s_list_revision) && remove_absent(applications) != 0)) {
        snprintf(err, err_len, "cannot store the app catalog");
        *retry = 1;
        pthread_mutex_unlock(&s_lifecycle);
        return -1;
    }
    s_list_revision++;
    int rc = primary ? apply_app(primary, primary, err, err_len, retry) : 0;
    if (primary && rc == 0) {
        /* A handover takes effect even if the other app's new image fails
         * to download. Its old image and env remain restartable. */
        const char *name = env_value(primary, "name");
        for (int i = 0; i < FC_MAX_APPS; i++) {
            app_t *old = s_apps[i];
            if (old && old->primary && strcmp(old->name, name) != 0) {
                old->primary = false; /* joined by park_secondary */
            }
        }
        const cJSON *pending;
        cJSON_ArrayForEach(pending, s_restore) {
            cJSON *role = cJSON_GetObjectItem(pending, "primary");
            if (cJSON_IsTrue(role)) {
                cJSON_SetBoolValue(role, 0);
            }
        }
    }
    if (rc == 0) {
        cJSON_ArrayForEach(app, applications) {
            if (app != primary && apply_app(app, primary, err, err_len, retry) != 0) {
                rc = -1;
                if (s_catalog_rollback) {
                    break;
                }
            }
        }
    }
    if (!s_defer_starts) {
        run_pending(0);
    }
    pthread_mutex_unlock(&s_lifecycle);
    return rc;
}

int fc_apps_apply(const cJSON *applications, char *err, size_t err_len, int *retry)
{
    return apply_list(applications, NULL, err, err_len, retry);
}

int fc_apps_apply_prepared(const fc_apps_plan_t *plan, char *err, size_t err_len, int *retry)
{
    return apply_list(plan->applications, plan, err, err_len, retry);
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
        pthread_mutex_lock(&s_lifecycle);
        /* The map changes only while no app runs. */
        int restart[FC_MAX_APPS] = {0};
        for (int i = 0; i < FC_MAX_APPS; i++) {
            app_t *a = s_apps[i];
            if (a && a->thread_running) {
                pthread_mutex_lock(&s_lock);
                int completed = strcmp(a->status, "Completed") == 0;
                pthread_mutex_unlock(&s_lock);
                if (completed) {
                    join_runner(a);
                    release_io(a);
                } else {
                    app_stop(a);
                    restart[i] = 1;
                }
            }
        }
        app_t *primary = primary_app();
        if (primary && restart[primary->slot]) {
            park_secondary(primary->name, true);
        }
        fc_board_load(g_fc_hw);
        fc_port_log(FC_LOG_INFO, "new board map%s", restart[0] || restart[1] ? ": restarting the apps" : "");
        for (int i = 0; i < FC_MAX_APPS; i++) {
            if (restart[i]) {
                s_apps[i]->start_pending = "new board map";
            }
        }
        run_pending(0);
        pthread_mutex_unlock(&s_lifecycle);
        return;
    }
    size_t dl = strlen(APP_CONFIG_DIR), pl = strlen(path);
    if (pl > dl + 5 && strncmp(path, APP_CONFIG_DIR, dl) == 0 && strcmp(path + pl - 5, ".json") == 0) {
        char name[FC_APP_NAME_MAX + 1];
        snprintf(name, sizeof(name), "%.*s", (int)(pl - dl - 5), path + dl);
        pthread_mutex_lock(&s_lock);
        app_t *a = find(name);
        if (a != NULL) {
            fc_apps_post(a, EV_CONFIG, -1, 0);
        }
        pthread_mutex_unlock(&s_lock);
    }
}

static void report_app(cJSON *apps, cJSON *custom, const char *name, const char *status,
                       int up, int restarts, const char *state)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddStringToObject(o, "appType", "container");
    cJSON_AddStringToObject(o, "status", status);
    cJSON_AddStringToObject(o, "ready", up ? "1/1" : "0/1");
    cJSON_AddNumberToObject(o, "restarts", restarts);
    cJSON_AddBoolToObject(o, "embedded", 0);
    cJSON_AddItemToArray(apps, o);
    char key[64];
    snprintf(key, sizeof(key), "app.%s", name);
    cJSON_AddStringToObject(custom, key, state);
}

void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info)
{
    int n = 0, running = 0, errors = 0, completed = 0;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (!a) {
            continue;
        }
        n++;
        int up = strcmp(a->status, "Running") == 0;
        running += up;
        errors += strcmp(a->status, "Error") == 0;
        completed += strcmp(a->status, "Completed") == 0; /* ran to its end: not degraded */
        report_app(apps, custom_info, a->name, a->status, up, a->restarts,
                   a->error[0] ? a->error : a->state[0] ? a->state : a->status);
    }
    const cJSON *o;
    cJSON_ArrayForEach(o, s_restore) {
        report_app(apps, custom_info, env_value(o, "name"), "Preparing", 0, 0, "Preparing");
        n++;
    }
    pthread_mutex_unlock(&s_lock);
    const char *st = n == 0 ? "NoApplications" : errors ? (running ? "Degraded" : "Error")
                                                        : running + completed == n ? "Healthy" : "Degraded";
    cJSON_AddStringToObject(summary, "status", st);
    if (n == 0) {
        cJSON_AddStringToObject(summary, "info", "No applications");
    }
}

/* ---- MQTT (fc_mqtt.h) --------------------------------------------------------------- */

/* A TLS handshake for the broker needs a block as large as the metrics
 * export's (fc_agent.c); the connection then keeps up to FC_MQTT_CONN_COST
 * (its TLS context and buffers, the packet buffers), which must leave the
 * agent its floor. The handshake's own peak comes while the agent is idle,
 * its connection open. */
#ifndef FC_MQTT_HEAP_MIN
#define FC_MQTT_HEAP_MIN (24 * 1024)
#endif
#ifndef FC_MQTT_CONN_COST
#define FC_MQTT_CONN_COST (12 * 1024)
#endif

static int (*s_agent_idle)(int claim);
static void (*s_agent_connected)(void);
static int64_t s_mqtt_heap_logged;

/* The agent is asked twice: first whether it is idle, then, once the heap
 * has room, to be held for the handshake (claim) until mqtt_connect_end. */
static int mqtt_may_connect(void)
{
    if (s_agent_idle != NULL && !s_agent_idle(0)) {
        return 0;
    }
    long largest = fc_port_heap_largest(), free_now = fc_port_heap_free_now();
    int room = (largest < 0 || largest >= FC_MQTT_HEAP_MIN) && (free_now < 0 || free_now >= s_heap_floor + FC_MQTT_CONN_COST);
    int64_t now = fc_apps_now_ms();
    if (!room && (s_mqtt_heap_logged == 0 || now - s_mqtt_heap_logged >= 60000)) {
        s_mqtt_heap_logged = now;
        fc_port_log(FC_LOG_WARN,
                    "mqtt: %ld bytes free (largest block %ld): waiting for %d KB more than the agent's %ld KB, and a "
                    "block of %d KB",
                    free_now, largest, FC_MQTT_CONN_COST / 1024, s_heap_floor / 1024, FC_MQTT_HEAP_MIN / 1024);
    }
    return room && (s_agent_idle == NULL || s_agent_idle(1));
}

static void mqtt_connect_end(void)
{
    if (s_agent_connected != NULL) {
        s_agent_connected();
    }
}

/* The link came or went: FC_EV_NET to the apps that may use it. */
static void mqtt_on_link(int up)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < FC_MAX_APPS; i++) {
        app_t *a = s_apps[i];
        if (a && fc_appapi_allowed(a, "mqtt")) {
            fc_apps_post(a, EV_NET, -1, up ? 1 : 0);
        }
    }
    pthread_mutex_unlock(&s_lock);
}

/* A delivery may already be in flight when an app unsubscribes. The
 * owner's slot lock has a permanent address; subscription IDs carry a
 * generation, so a new occupant cannot receive a previous app's message. */
static int mqtt_on_message(void *owner, int sub, fc_mqtt_msg_t *m)
{
    int rc = -1;
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (owner == &s_sync[i] && s_apps[i] != NULL) {
            rc = fc_appapi_mqtt_message(s_apps[i], sub, m);
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

void fc_apps_mqtt_hooks(int (*agent_idle)(int claim), void (*agent_connected)(void))
{
    s_agent_idle = agent_idle;
    s_agent_connected = agent_connected;
    fc_mqtt_hooks_t h = {.on_message = mqtt_on_message,
                         .on_link = mqtt_on_link,
                         .may_connect = mqtt_may_connect,
                         .connect_end = mqtt_connect_end};
    fc_mqtt_set_hooks(&h);
}
