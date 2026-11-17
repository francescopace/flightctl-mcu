/*
 * The functions apps call, module "fc": the app interface of
 * wasm-apps/sdk/fc_app.h. Each runs on the calling app's thread. What the
 * firmware does not have yet (MQTT) is there too,
 * returning FC_ENOTSUP, so that an app written against the whole header
 * loads and can tell.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "fc_agent.h"
#include "fc_apps_int.h"
#include "fc_metrics.h"
#include "fc_port.h"
#include "psa/crypto.h"

#define APP_CONFIG_DIR "/etc/fc-app/"

static app_t *app_of(wasm_exec_env_t env)
{
    return wasm_runtime_get_user_data(env);
}

/* Name in FC_ALLOW (space separated). */
static int allowed(const app_t *a, const char *name, size_t nlen)
{
    const char *p = a->allow;
    while (*p) {
        while (*p == ' ') {
            p++;
        }
        const char *e = p;
        while (*e && *e != ' ') {
            e++;
        }
        if ((size_t)(e - p) == nlen && nlen > 0 && memcmp(p, name, nlen) == 0) {
            return 1;
        }
        p = e;
    }
    return 0;
}

/* The board entry an app may open: FC_HW_ENOENT, FC_HW_EPERM, or 0. */
static int lookup(app_t *a, const char *name, int32_t nlen, int kind, fc_board_entry_t **out)
{
    fc_board_entry_t *e = nlen > 0 ? fc_board_find(name, (size_t)nlen) : NULL;
    if (e == NULL || e->kind != kind) {
        return FC_HW_ENOENT;
    }
    if (!allowed(a, name, (size_t)nlen)) {
        return FC_HW_EPERM;
    }
    *out = e;
    return 0;
}

static int new_handle(app_t *a, int kind)
{
    for (int i = 0; i < APP_HANDLES; i++) {
        if (a->h[i].kind == H_FREE) {
            uint32_t serial = a->h[i].serial + 1;
            pthread_mutex_lock(a->ev_lock);
            memset(&a->h[i], 0, sizeof(a->h[i]));
            a->h[i].kind = kind;
            a->h[i].serial = serial;
            pthread_mutex_unlock(a->ev_lock);
            return i;
        }
    }
    return FC_HW_ENOSPC;
}

static app_handle_t *handle(app_t *a, int32_t h, int kind)
{
    return h >= 0 && h < APP_HANDLES && a->h[h].kind == kind ? &a->h[h] : NULL;
}

/* ---- state, log, info --------------------------------------------------- */

static void fc_log(wasm_exec_env_t env, int32_t level, const char *s, int32_t len)
{
    app_t *a = app_of(env);
    fc_log_level_t l = level <= 0 ? FC_LOG_ERROR : level == 1 ? FC_LOG_WARN : level == 2 ? FC_LOG_INFO : FC_LOG_DEBUG;
    fc_port_log(l, "app %s: %.*s", a->name, (int)len, s);
}

static int32_t copy_out(char *buf, int32_t cap, const char *v, size_t n)
{
    if ((size_t)cap < n) {
        return FC_HW_ENOSPC;
    }
    memcpy(buf, v, n);
    return (int32_t)n;
}

static int key_is(const char *k, int32_t klen, const char *want)
{
    return (size_t)klen == strlen(want) && memcmp(k, want, (size_t)klen) == 0;
}

static int32_t fc_info(wasm_exec_env_t env, const char *k, int32_t klen, char *buf, int32_t cap)
{
    app_t *a = app_of(env);
    const char *v = NULL;
    if (key_is(k, klen, "device")) {
        v = fc_apps_device_name();
        if (v == NULL) {
            return FC_HW_EAGAIN; /* not enrolled yet */
        }
    } else if (key_is(k, klen, "app")) {
        v = a->name;
    } else if (key_is(k, klen, "firmware")) {
        v = g_fc_hw && g_fc_hw->firmware ? g_fc_hw->firmware : "";
    } else if (key_is(k, klen, "board")) {
        v = g_fc_hw && g_fc_hw->board ? g_fc_hw->board : "";
    } else {
        return FC_HW_ENOENT;
    }
    return copy_out(buf, cap, v, strlen(v));
}

/* ---- time ----------------------------------------------------------------- */

static int64_t fc_time_ms(wasm_exec_env_t env)
{
    return fc_apps_now_ms();
}

static int64_t fc_time_unix_ms(wasm_exec_env_t env)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1704067200) { /* before 2024: the clock is not set yet */
        return FC_HW_EAGAIN;
    }
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Stops the call when it has spent more than APP_CALL_MS: 1 if so. */
static int over_time(wasm_exec_env_t env, app_t *a)
{
    if (fc_apps_now_ms() - a->call_start_ms > APP_CALL_MS) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "a call took more than 1 s");
        return 1;
    }
    return 0;
}

static int32_t fc_sleep_ms(wasm_exec_env_t env, int32_t ms)
{
    app_t *a = app_of(env);
    if (ms < 0) {
        return FC_HW_EINVAL;
    }
    if (ms > APP_WAIT_MS) {
        ms = APP_WAIT_MS;
    }
    for (int32_t left = ms; left > 0 && !fc_apps_stopping(a); left -= 10) {
        usleep((useconds_t)(left < 10 ? left : 10) * 1000);
    }
    if (fc_apps_stopping(a)) {
        wasm_runtime_set_exception(wasm_runtime_get_module_inst(env), "stopped");
        return 0;
    }
    over_time(env, a);
    return 0;
}

static int32_t fc_random(wasm_exec_env_t env, void *buf, int32_t len)
{
    if (psa_crypto_init() != PSA_SUCCESS || psa_generate_random(buf, (size_t)len) != PSA_SUCCESS) {
        return FC_HW_EIO;
    }
    return 0;
}

static int32_t fc_timer_start(wasm_exec_env_t env, int32_t ms, int32_t periodic)
{
    app_t *a = app_of(env);
    if (ms < 1) {
        return FC_HW_EINVAL;
    }
    int n = 0;
    for (int i = 0; i < APP_HANDLES; i++) {
        n += a->h[i].kind == H_TIMER;
    }
    if (n >= APP_TIMERS) {
        return FC_HW_ENOSPC;
    }
    int h = new_handle(a, H_TIMER);
    if (h < 0) {
        return h;
    }
    a->h[h].period_ms = ms;
    a->h[h].periodic = periodic != 0;
    a->h[h].due_ms = fc_apps_now_ms() + ms;
    return h;
}

/* ---- config -------------------------------------------------------------------- */

void fc_appapi_load_config(app_t *a)
{
    char path[80], *text = NULL;
    size_t len = 0;
    snprintf(path, sizeof(path), APP_CONFIG_DIR "%s.json", a->name);
    cJSON_Delete(a->config);
    a->config = NULL;
    if (fc_port_read_file(path, &text, &len) == 0) {
        a->config = cJSON_Parse(text);
        if (a->config != NULL && !cJSON_IsObject(a->config)) {
            cJSON_Delete(a->config);
            a->config = NULL;
        }
        if (a->config == NULL) {
            fc_port_log(FC_LOG_WARN, "app %s: %s is not a JSON object", a->name, path);
        }
        free(text);
    }
}

/* A number as text: an integer as such, otherwise as few digits as give the
 * same double back. */
static void number_text(double v, char *out, size_t len)
{
    if (v == (double)(long long)v && v > -1e15 && v < 1e15) {
        snprintf(out, len, "%lld", (long long)v);
        return;
    }
    for (int digits = 15; digits <= 17; digits++) {
        snprintf(out, len, "%.*g", digits, v);
        if (strtod(out, NULL) == v) {
            return;
        }
    }
}

static int32_t fc_config_get(wasm_exec_env_t env, const char *k, int32_t klen, char *buf, int32_t cap)
{
    app_t *a = app_of(env);
    char key[64], num[32];
    if (klen <= 0 || (size_t)klen >= sizeof(key)) {
        return FC_HW_ENOENT;
    }
    memcpy(key, k, (size_t)klen);
    key[klen] = '\0';
    const cJSON *v = a->config ? cJSON_GetObjectItemCaseSensitive(a->config, key) : NULL;
    const char *text = NULL;
    if (cJSON_IsString(v)) {
        text = v->valuestring;
    } else if (cJSON_IsNumber(v)) {
        number_text(v->valuedouble, num, sizeof(num));
        text = num;
    } else if (cJSON_IsBool(v)) {
        text = cJSON_IsTrue(v) ? "true" : "false";
    } else {
        return FC_HW_ENOENT;
    }
    return copy_out(buf, cap, text, strlen(text));
}

/* ---- storage -------------------------------------------------------------------
 * One record per app: entries of [key length, 1 byte][key][value length, 2
 * bytes little-endian][value], written whole on each change. */

void fc_appapi_kv_key(const char *app_name, char *out, size_t len)
{
    snprintf(out, len, "akv:%s", app_name);
}

typedef struct {
    unsigned char *data;
    size_t len;
} kv_rec_t;

static void kv_load(const app_t *a, kv_rec_t *r)
{
    char key[64];
    fc_appapi_kv_key(a->name, key, sizeof(key));
    char *v = NULL;
    r->data = NULL;
    r->len = 0;
    if (fc_port_kv_get(key, &v, &r->len) == 0) {
        r->data = (unsigned char *)v;
    }
}

/* Finds key in the record: offset of its entry, or -1. *vlen and *voff give
 * the value; *elen the whole entry. Stops at a malformed record. */
static long kv_find(const kv_rec_t *r, const char *k, int32_t klen, size_t *voff, size_t *vlen, size_t *elen)
{
    size_t off = 0;
    while (off + 1 <= r->len) {
        size_t kl = r->data[off];
        if (off + 1 + kl + 2 > r->len) {
            break;
        }
        size_t vl = r->data[off + 1 + kl] | (size_t)r->data[off + 2 + kl] << 8;
        size_t el = 1 + kl + 2 + vl;
        if (off + el > r->len) {
            break;
        }
        if (kl == (size_t)klen && memcmp(r->data + off + 1, k, kl) == 0) {
            *voff = off + 1 + kl + 2;
            *vlen = vl;
            *elen = el;
            return (long)off;
        }
        off += el;
    }
    return -1;
}

/* Bytes the app uses: keys and values. */
static size_t kv_used(const kv_rec_t *r)
{
    size_t off = 0, used = 0;
    while (off + 1 <= r->len) {
        size_t kl = r->data[off];
        if (off + 1 + kl + 2 > r->len) {
            break;
        }
        size_t vl = r->data[off + 1 + kl] | (size_t)r->data[off + 2 + kl] << 8;
        used += kl + vl;
        off += 1 + kl + 2 + vl;
    }
    return used;
}

static int kv_store(const app_t *a, const unsigned char *data, size_t len)
{
    char key[64];
    fc_appapi_kv_key(a->name, key, sizeof(key));
    if (len == 0) {
        fc_port_kv_del(key);
        return 0;
    }
    return fc_port_kv_set(key, data, len) == 0 ? 0 : FC_HW_EIO;
}

static int valid_kv_key(int32_t klen)
{
    return klen >= 1 && klen <= APP_KV_KEY;
}

static int32_t fc_kv_get(wasm_exec_env_t env, const char *k, int32_t klen, void *buf, int32_t cap)
{
    app_t *a = app_of(env);
    if (!valid_kv_key(klen)) {
        return FC_HW_EINVAL;
    }
    kv_rec_t r;
    kv_load(a, &r);
    size_t voff, vlen, elen;
    int32_t rc = FC_HW_ENOENT;
    if (kv_find(&r, k, klen, &voff, &vlen, &elen) >= 0) {
        rc = (size_t)cap < vlen ? FC_HW_ENOSPC : (int32_t)vlen;
        if (rc >= 0) {
            memcpy(buf, r.data + voff, vlen);
        }
    }
    free(r.data);
    return rc;
}

static int32_t fc_kv_set(wasm_exec_env_t env, const char *k, int32_t klen, const void *v, int32_t len)
{
    app_t *a = app_of(env);
    if (!valid_kv_key(klen) || len > APP_KV_QUOTA) {
        return valid_kv_key(klen) ? FC_HW_ENOSPC : FC_HW_EINVAL;
    }
    kv_rec_t r;
    kv_load(a, &r);
    size_t voff, vlen, elen;
    long at = kv_find(&r, k, klen, &voff, &vlen, &elen);
    size_t used = kv_used(&r) - (at >= 0 ? (size_t)klen + vlen : 0);
    if (used + (size_t)klen + (size_t)len > APP_KV_QUOTA) {
        free(r.data);
        return FC_HW_ENOSPC;
    }
    size_t keep = r.len - (at >= 0 ? elen : 0);
    unsigned char *out = malloc(keep + 1 + (size_t)klen + 2 + (size_t)len);
    if (out == NULL) {
        free(r.data);
        return FC_HW_EIO;
    }
    size_t n = 0;
    if (at >= 0) {
        memcpy(out, r.data, (size_t)at);
        memcpy(out + at, r.data + at + elen, r.len - (size_t)at - elen);
    } else if (r.len > 0) {
        memcpy(out, r.data, r.len);
    }
    n = keep;
    out[n++] = (unsigned char)klen;
    memcpy(out + n, k, (size_t)klen);
    n += (size_t)klen;
    out[n++] = (unsigned char)(len & 0xff);
    out[n++] = (unsigned char)(len >> 8);
    memcpy(out + n, v, (size_t)len);
    n += (size_t)len;
    int rc = kv_store(a, out, n);
    free(out);
    free(r.data);
    return rc;
}

static int32_t fc_kv_del(wasm_exec_env_t env, const char *k, int32_t klen)
{
    app_t *a = app_of(env);
    if (!valid_kv_key(klen)) {
        return FC_HW_EINVAL;
    }
    kv_rec_t r;
    kv_load(a, &r);
    size_t voff, vlen, elen;
    long at = kv_find(&r, k, klen, &voff, &vlen, &elen);
    int rc = FC_HW_ENOENT;
    if (at >= 0) {
        memmove(r.data + at, r.data + at + elen, r.len - (size_t)at - elen);
        rc = kv_store(a, r.data, r.len - elen);
    }
    free(r.data);
    return rc;
}

/* ---- handles -------------------------------------------------------------------- */

static int logical(const fc_board_entry_t *e, int level)
{
    return (level != 0) ^ (e->active_low != 0);
}

static void close_handle(app_t *a, int i)
{
    app_handle_t *h = &a->h[i];
    const fc_hw_t *hw = g_fc_hw;
    if (h->kind == H_GPIO) {
        int pin = fc_board_pin(h->res);
        if (h->irq && hw && hw->gpio_irq) {
            hw->gpio_irq(pin, 0, NULL, NULL);
        }
        int out = h->mode == FC_HW_GPIO_OUT || h->mode == FC_HW_GPIO_OUT_OD;
        if (out && h->res->safe >= 0 && hw && hw->gpio_write) {
            hw->gpio_write(pin, logical(h->res, h->res->safe));
        } else if (hw && hw->gpio_release) {
            hw->gpio_release(pin);
        }
        fc_board_release(h->res);
    } else if (h->kind == H_LED) {
        if (hw && hw->led_close) {
            hw->led_close(&h->res->led);
        }
        fc_board_release(h->res);
    } else if (h->kind == H_I2C || h->kind == H_ADC || h->kind == H_PWM || h->kind == H_SPI ||
               h->kind == H_UART) {
        if (h->kind == H_SPI && hw && hw->spi_close) {
            hw->spi_close(h->port); /* ends a kept chip select too */
        } else if (h->kind == H_UART && hw && hw->uart_close) {
            hw->uart_close(h->port); /* no more data callbacks once it returns */
        } else if (h->kind == H_I2C && hw && hw->i2c_close) {
            hw->i2c_close(h->port);
        } else if (h->kind == H_ADC && hw && hw->adc_close) {
            hw->adc_close(h->port);
        } else if (h->kind == H_PWM && hw && hw->pwm_close) {
            hw->pwm_close(h->port); /* low, as an app's PWM is when it stops */
        }
        fc_board_release(h->res);
    }
    /* Events of this handle still queued are dropped with it. */
    pthread_mutex_lock(a->ev_lock);
    uint32_t serial = h->serial;
    memset(h, 0, sizeof(*h));
    h->serial = serial;
    int kept = 0;
    for (int k = 0; k < a->ev_count; k++) {
        app_event_t e = a->ev[(a->ev_head + k) % APP_EVENTS];
        if (!((e.type == EV_GPIO || e.type == EV_TIMER || e.type == EV_UART) && e.handle == i)) {
            a->ev[(a->ev_head + kept) % APP_EVENTS] = e;
            kept++;
        }
    }
    a->ev_count = kept;
    pthread_mutex_unlock(a->ev_lock);
}

void fc_appapi_close_all(app_t *a)
{
    for (int i = 0; i < APP_HANDLES; i++) {
        if (a->h[i].kind != H_FREE) {
            close_handle(a, i);
        }
    }
    cJSON_Delete(a->config);
    a->config = NULL;
}

static int32_t fc_close(wasm_exec_env_t env, int32_t h)
{
    app_t *a = app_of(env);
    if (h < 0 || h >= APP_HANDLES || a->h[h].kind == H_FREE) {
        return FC_HW_EINVAL;
    }
    close_handle(a, h);
    return 0;
}

/* ---- GPIO ---------------------------------------------------------------------------- */

static int32_t fc_gpio_open(wasm_exec_env_t env, const char *name, int32_t nlen, int32_t mode)
{
    app_t *a = app_of(env);
    fc_board_entry_t *e;
    int rc = lookup(a, name, nlen, FC_RES_GPIO, &e);
    if (rc != 0) {
        return rc;
    }
    if (mode < FC_HW_GPIO_IN || mode > FC_HW_GPIO_OUT_OD) {
        return FC_HW_EINVAL;
    }
    const fc_hw_t *hw = g_fc_hw;
    if (hw == NULL || hw->gpio_setup == NULL || fc_board_pin(e) < 0) {
        return FC_HW_ENOTSUP;
    }
    if ((rc = fc_board_claim(e, a->slot)) != 0) {
        return rc;
    }
    int h = new_handle(a, H_GPIO);
    if (h < 0) {
        fc_board_release(e);
        return h;
    }
    int out = mode == FC_HW_GPIO_OUT || mode == FC_HW_GPIO_OUT_OD;
    if (out && hw->gpio_write) {
        hw->gpio_write(fc_board_pin(e), logical(e, 0)); /* starts inactive */
    }
    if ((rc = hw->gpio_setup(fc_board_pin(e), mode)) != 0) {
        a->h[h].kind = H_FREE;
        fc_board_release(e);
        return rc < 0 ? rc : FC_HW_EIO;
    }
    if (out && hw->gpio_write) {
        hw->gpio_write(fc_board_pin(e), logical(e, 0));
    }
    a->h[h].res = e;
    a->h[h].mode = mode;
    return h;
}

static int32_t fc_gpio_read(wasm_exec_env_t env, int32_t h)
{
    app_handle_t *g = handle(app_of(env), h, H_GPIO);
    if (g == NULL) {
        return FC_HW_EINVAL;
    }
    if (g_fc_hw->gpio_read == NULL) {
        return FC_HW_ENOTSUP;
    }
    int v = g_fc_hw->gpio_read(fc_board_pin(g->res));
    return v < 0 ? v : logical(g->res, v);
}

static int32_t fc_gpio_write(wasm_exec_env_t env, int32_t h, int32_t level)
{
    app_handle_t *g = handle(app_of(env), h, H_GPIO);
    if (g == NULL || (g->mode != FC_HW_GPIO_OUT && g->mode != FC_HW_GPIO_OUT_OD)) {
        return FC_HW_EINVAL;
    }
    if (g_fc_hw->gpio_write == NULL) {
        return FC_HW_ENOTSUP;
    }
    return g_fc_hw->gpio_write(fc_board_pin(g->res), logical(g->res, level));
}

/* From the port's task: an edge on a GPIO with an interrupt. */
static void gpio_edge(void *arg)
{
    app_irq_ctx_t *c = arg;
    app_t *a = c->app;
    int64_t now = fc_apps_now_ms();
    pthread_mutex_lock(a->ev_lock);
    app_handle_t *h = &a->h[c->handle];
    int ok = a->accepting && h->kind == H_GPIO && h->irq && now - h->last_edge_ms >= h->debounce_ms;
    fc_board_entry_t *res = h->res;
    if (ok) {
        h->last_edge_ms = now;
    }
    pthread_mutex_unlock(a->ev_lock);
    if (ok) {
        int v = g_fc_hw->gpio_read ? g_fc_hw->gpio_read(fc_board_pin(res)) : 0;
        fc_apps_post(a, EV_GPIO, c->handle, v < 0 ? 0 : logical(res, v));
    }
}

static int32_t fc_gpio_irq(wasm_exec_env_t env, int32_t h, int32_t edge, int32_t debounce_ms)
{
    app_t *a = app_of(env);
    app_handle_t *g = handle(a, h, H_GPIO);
    if (g == NULL || edge < 0 || edge > 3 || debounce_ms < 0 || debounce_ms > 1000 ||
        g->mode == FC_HW_GPIO_OUT || g->mode == FC_HW_GPIO_OUT_OD) {
        return FC_HW_EINVAL;
    }
    if (g_fc_hw->gpio_irq == NULL) {
        return FC_HW_ENOTSUP;
    }
    /* Logical edges: an active-low pin rises when the level falls. */
    int hw_edge = g->res->active_low && (edge == 1 || edge == 2) ? 3 - edge : edge;
    pthread_mutex_lock(a->ev_lock);
    g->irq = edge != 0;
    g->debounce_ms = debounce_ms;
    g->last_edge_ms = -1000000;
    pthread_mutex_unlock(a->ev_lock);
    a->irq_ctx[h].app = a;
    a->irq_ctx[h].handle = h;
    int rc = g_fc_hw->gpio_irq(fc_board_pin(g->res), hw_edge, edge ? gpio_edge : NULL, edge ? &a->irq_ctx[h] : NULL);
    if (rc != 0) {
        pthread_mutex_lock(a->ev_lock);
        g->irq = 0;
        pthread_mutex_unlock(a->ev_lock);
    }
    return rc;
}

/* ---- LED ------------------------------------------------------------------------------------ */

static int32_t fc_led_open(wasm_exec_env_t env, const char *name, int32_t nlen)
{
    app_t *a = app_of(env);
    fc_board_entry_t *e;
    int rc = lookup(a, name, nlen, FC_RES_LED, &e);
    if (rc != 0) {
        return rc;
    }
    if (g_fc_hw == NULL || g_fc_hw->led_open == NULL || g_fc_hw->led_rgb == NULL) {
        return FC_HW_ENOTSUP;
    }
    if ((rc = fc_board_claim(e, a->slot)) != 0) {
        return rc;
    }
    int h = new_handle(a, H_LED);
    if (h < 0 || (rc = g_fc_hw->led_open(&e->led)) != 0) {
        if (h >= 0) {
            a->h[h].kind = H_FREE;
        }
        fc_board_release(e);
        return h < 0 ? h : rc < 0 ? rc : FC_HW_EIO;
    }
    a->h[h].res = e;
    return h;
}

static uint8_t byte(int32_t v)
{
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static int32_t fc_led_rgb(wasm_exec_env_t env, int32_t h, int32_t r, int32_t g, int32_t b)
{
    app_handle_t *l = handle(app_of(env), h, H_LED);
    if (l == NULL) {
        return FC_HW_EINVAL;
    }
    return g_fc_hw->led_rgb(&l->res->led, byte(r), byte(g), byte(b));
}

/* ---- the chip's sensors ----------------------------------------------------------------------- */

static int32_t fc_sensor_read(wasm_exec_env_t env, const char *name, int32_t nlen, int32_t value_at)
{
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    /* A lone pointer is checked for one byte only: this one is four. */
    if (!wasm_runtime_validate_app_addr(inst, (uint64_t)(uint32_t)value_at, 4)) {
        return 0; /* the exception is set: the app traps */
    }
    char n[24];
    if (nlen <= 0 || (size_t)nlen >= sizeof(n)) {
        return FC_HW_ENOENT;
    }
    memcpy(n, name, (size_t)nlen);
    n[nlen] = '\0';
    if (g_fc_hw == NULL || g_fc_hw->sensor_read == NULL) {
        return FC_HW_ENOTSUP;
    }
    int32_t v = 0;
    int rc = g_fc_hw->sensor_read(n, &v);
    if (rc != 0) {
        return rc;
    }
    int32_t *out = wasm_runtime_addr_app_to_native(inst, (uint64_t)(uint32_t)value_at);
    memcpy(out, &v, sizeof(v));
    return 0;
}

/* ---- I2C, ADC and PWM ---------------------------------------------------------------------------- */

/* The longest I2C transfer: 1 KB takes about 90 ms at 100 kHz, within one
 * wait (APP_WAIT_MS). */
#define I2C_MAX_LEN 1024

/* Opens a board entry of `kind` through the port's opener (which returns
 * the port's handle), as a handle of the app's of kind hkind. */
static int32_t open_entry(app_t *a, const char *name, int32_t nlen, int kind, int hkind, int supported,
                          int (*opener)(fc_board_entry_t *e, int arg), int arg)
{
    fc_board_entry_t *e;
    int rc = lookup(a, name, nlen, kind, &e);
    if (rc != 0) {
        return rc;
    }
    if (!supported) {
        return FC_HW_ENOTSUP;
    }
    if ((rc = fc_board_claim(e, a->slot)) != 0) {
        return rc;
    }
    int h = new_handle(a, hkind);
    if (h < 0) {
        fc_board_release(e);
        return h;
    }
    int port = opener(e, arg);
    if (port < 0) {
        a->h[h].kind = H_FREE;
        fc_board_release(e);
        return port;
    }
    a->h[h].res = e;
    a->h[h].port = port;
    return h;
}

static int i2c_opener(fc_board_entry_t *e, int arg) { return g_fc_hw->i2c_open(&e->bus); }
static int adc_opener(fc_board_entry_t *e, int arg) { return fc_board_pin(e) < 0 ? FC_HW_ENOTSUP : g_fc_hw->adc_open(fc_board_pin(e)); }
static int pwm_opener(fc_board_entry_t *e, int hz)
{
    return fc_board_pin(e) < 0 ? FC_HW_ENOTSUP : g_fc_hw->pwm_open(fc_board_pin(e), hz > 0 ? hz : e->hz > 0 ? e->hz : 1000);
}

static int32_t fc_i2c_open(wasm_exec_env_t env, const char *n, int32_t l)
{
    const fc_hw_t *hw = g_fc_hw;
    return open_entry(app_of(env), n, l, FC_RES_I2C, H_I2C, hw && hw->i2c_open && hw->i2c_transfer, i2c_opener, 0);
}

static int32_t i2c_do(wasm_exec_env_t env, int32_t h, int32_t addr, const void *w, int32_t wl, void *r, int32_t rl)
{
    app_t *a = app_of(env);
    app_handle_t *b = handle(a, h, H_I2C);
    if (b == NULL || addr < 0 || addr > 127 || wl < 0 || rl < 0 || wl > I2C_MAX_LEN || rl > I2C_MAX_LEN ||
        wl + rl == 0) {
        return FC_HW_EINVAL;
    }
    int rc = g_fc_hw->i2c_transfer(b->port, addr, w, (size_t)wl, r, (size_t)rl, APP_WAIT_MS);
    over_time(env, a);
    return rc;
}

static int32_t fc_i2c_write(wasm_exec_env_t env, int32_t h, int32_t addr, const void *d, int32_t len)
{
    return i2c_do(env, h, addr, d, len, NULL, 0);
}
static int32_t fc_i2c_read(wasm_exec_env_t env, int32_t h, int32_t addr, void *b, int32_t len)
{
    return i2c_do(env, h, addr, NULL, 0, b, len);
}
static int32_t fc_i2c_write_read(wasm_exec_env_t env, int32_t h, int32_t addr, const void *w, int32_t wl, void *r,
                                 int32_t rl)
{
    if (wl <= 0 || rl <= 0) {
        return FC_HW_EINVAL;
    }
    return i2c_do(env, h, addr, w, wl, r, rl);
}

static int32_t fc_adc_open(wasm_exec_env_t env, const char *n, int32_t l)
{
    const fc_hw_t *hw = g_fc_hw;
    return open_entry(app_of(env), n, l, FC_RES_ADC, H_ADC, hw && hw->adc_open && hw->adc_read_mv, adc_opener, 0);
}

static int32_t fc_adc_read_mv(wasm_exec_env_t env, int32_t h)
{
    app_handle_t *c = handle(app_of(env), h, H_ADC);
    return c == NULL ? FC_HW_EINVAL : g_fc_hw->adc_read_mv(c->port);
}

static int32_t fc_pwm_open(wasm_exec_env_t env, const char *n, int32_t l, int32_t hz)
{
    const fc_hw_t *hw = g_fc_hw;
    if (hz < 0) {
        return FC_HW_EINVAL;
    }
    return open_entry(app_of(env), n, l, FC_RES_PWM, H_PWM, hw && hw->pwm_open && hw->pwm_duty, pwm_opener, hz);
}

static int32_t fc_pwm_duty(wasm_exec_env_t env, int32_t h, int32_t duty)
{
    app_handle_t *c = handle(app_of(env), h, H_PWM);
    if (c == NULL || duty < 0 || duty > 65535) {
        return FC_HW_EINVAL;
    }
    return g_fc_hw->pwm_duty(c->port, (uint32_t)duty);
}

/* ---- SPI ---------------------------------------------------------------------------------- */

#define SPI_MAX_LEN 4096 /* FC_SPI_MAX */

static int spi_opener(fc_board_entry_t *e, int arg) { return g_fc_hw->spi_open(&e->bus); }

static int32_t fc_spi_open(wasm_exec_env_t env, const char *n, int32_t l)
{
    const fc_hw_t *hw = g_fc_hw;
    return open_entry(app_of(env), n, l, FC_RES_SPI, H_SPI, hw && hw->spi_open && hw->spi_transfer, spi_opener, 0);
}

/* The app's buffer at offset at (0: none) for len bytes, or NULL: the
 * signature passes plain offsets, since either may be 0. */
static void *app_buf(wasm_exec_env_t env, int32_t at, int32_t len, int *bad)
{
    if (at == 0) {
        return NULL;
    }
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(env);
    if (!wasm_runtime_validate_app_addr(inst, (uint64_t)(uint32_t)at, (uint64_t)(uint32_t)len)) {
        *bad = 1; /* the exception is set: the app traps */
        return NULL;
    }
    return wasm_runtime_addr_app_to_native(inst, (uint64_t)(uint32_t)at);
}

static int32_t fc_spi_transfer(wasm_exec_env_t env, int32_t h, int32_t tx, int32_t rx, int32_t len, int32_t flags)
{
    app_t *a = app_of(env);
    app_handle_t *d = handle(a, h, H_SPI);
    if (d == NULL || len <= 0 || len > SPI_MAX_LEN || (flags & ~1) != 0) {
        return FC_HW_EINVAL;
    }
    int bad = 0;
    const uint8_t *t = app_buf(env, tx, len, &bad);
    uint8_t *r = app_buf(env, rx, len, &bad);
    if (bad) {
        return 0;
    }
    int keep = flags & 1;
    int rc = g_fc_hw->spi_transfer(d->port, t, r, (size_t)len, keep, APP_WAIT_MS);
    d->kept = rc == 0 && keep;
    over_time(env, a);
    return rc;
}

void fc_appapi_end_call(app_t *a)
{
    for (int i = 0; i < APP_HANDLES; i++) {
        if (a->h[i].kind == H_SPI && a->h[i].kept) {
            if (g_fc_hw->spi_release) {
                g_fc_hw->spi_release(a->h[i].port);
            }
            a->h[i].kept = 0;
        }
    }
}

/* ---- UART -------------------------------------------------------------------------------- */

/* From the port's task: data has come. One FC_EV_UART until the app reads. */
static void uart_data(void *arg, int n)
{
    app_irq_ctx_t *c = arg;
    app_t *a = c->app;
    pthread_mutex_lock(a->ev_lock);
    app_handle_t *h = &a->h[c->handle];
    int post = a->accepting && h->kind == H_UART && !h->ev_pending;
    if (post) {
        h->ev_pending = 1;
    }
    pthread_mutex_unlock(a->ev_lock);
    if (post) {
        fc_apps_post(a, EV_UART, c->handle, n);
    }
}

static int32_t fc_uart_open(wasm_exec_env_t env, const char *name, int32_t nlen, int32_t baud, int32_t format)
{
    app_t *a = app_of(env);
    const fc_hw_t *hw = g_fc_hw;
    fc_board_entry_t *e;
    int rc = lookup(a, name, nlen, FC_RES_UART, &e);
    if (rc != 0) {
        return rc;
    }
    if (baud < 0 || format < FC_HW_UART_8N1 || format > FC_HW_UART_8N2) {
        return FC_HW_EINVAL;
    }
    if (hw == NULL || hw->uart_open == NULL || hw->uart_read == NULL || hw->uart_write == NULL) {
        return FC_HW_ENOTSUP;
    }
    if ((rc = fc_board_claim(e, a->slot)) != 0) {
        return rc;
    }
    int h = new_handle(a, H_UART);
    if (h < 0) {
        fc_board_release(e);
        return h;
    }
    a->irq_ctx[h].app = a;
    a->irq_ctx[h].handle = h;
    a->h[h].res = e;
    int port = hw->uart_open(&e->bus, baud > 0 ? baud : e->bus.baud > 0 ? e->bus.baud : 115200, format, uart_data,
                             &a->irq_ctx[h]);
    if (port < 0) {
        a->h[h].kind = H_FREE;
        fc_board_release(e);
        return port;
    }
    a->h[h].port = port;
    return h;
}

static int32_t fc_uart_write(wasm_exec_env_t env, int32_t h, const void *d, int32_t len)
{
    app_t *a = app_of(env);
    app_handle_t *u = handle(a, h, H_UART);
    if (u == NULL || len < 0) {
        return FC_HW_EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    int rc = g_fc_hw->uart_write(u->port, d, (size_t)len, APP_WAIT_MS);
    over_time(env, a);
    return rc;
}

static int32_t fc_uart_read(wasm_exec_env_t env, int32_t h, void *b, int32_t cap, int32_t timeout)
{
    app_t *a = app_of(env);
    app_handle_t *u = handle(a, h, H_UART);
    if (u == NULL || cap < 0 || timeout < 0) {
        return FC_HW_EINVAL;
    }
    /* What comes after this read raises FC_EV_UART again. */
    pthread_mutex_lock(a->ev_lock);
    u->ev_pending = 0;
    pthread_mutex_unlock(a->ev_lock);
    if (cap == 0) {
        return 0;
    }
    int rc = g_fc_hw->uart_read(u->port, b, (size_t)cap, timeout > APP_WAIT_MS ? APP_WAIT_MS : timeout);
    over_time(env, a);
    return rc;
}

static int32_t grant_then_notsup(wasm_exec_env_t env, const char *grant)
{
    return allowed(app_of(env), grant, strlen(grant)) ? FC_HW_ENOTSUP : FC_HW_EPERM;
}
/* A value for the Telemetry Gateway, kept until the next export (fc_metrics.h). */
static int32_t fc_metric(wasm_exec_env_t env, const char *n, int32_t l, int32_t kind, double v)
{
    app_t *a = app_of(env);
    if (!allowed(a, "metrics", 7)) {
        return FC_HW_EPERM;
    }
    return l < 0 ? FC_HW_EINVAL : fc_metrics_app_set(a->name, n, (size_t)l, kind, v);
}
static int32_t fc_mqtt_publish(wasm_exec_env_t env, const char *t, int32_t tl, const void *p, int32_t pl, int32_t qos,
                               int32_t retain)
{
    return grant_then_notsup(env, "mqtt");
}
static int32_t fc_mqtt_subscribe(wasm_exec_env_t env, int32_t scope, const char *t, int32_t tl, int32_t qos)
{
    return grant_then_notsup(env, "mqtt");
}
static int32_t fc_mqtt_topic(wasm_exec_env_t env, char *b, int32_t cap) { return FC_HW_EAGAIN; }
static int32_t fc_event_data(wasm_exec_env_t env, void *b, int32_t cap) { return 0; }

/* ---- the table ----------------------------------------------------------------------------------- */

/* const, in flash: in strcmp order of the names, so WAMR does not sort it
 * (third_party/patches/wamr-const-natives.patch; test_apps.c checks the
 * order, as a table out of order would be written to at start). */
static const NativeSymbol s_natives[] = {
    {"adc_open", (void *)fc_adc_open, "(*~)i", NULL},
    {"adc_read_mv", (void *)fc_adc_read_mv, "(i)i", NULL},
    {"close", (void *)fc_close, "(i)i", NULL},
    {"config_get", (void *)fc_config_get, "(*~*~)i", NULL},
    {"event_data", (void *)fc_event_data, "(*~)i", NULL},
    {"gpio_irq", (void *)fc_gpio_irq, "(iii)i", NULL},
    {"gpio_open", (void *)fc_gpio_open, "(*~i)i", NULL},
    {"gpio_read", (void *)fc_gpio_read, "(i)i", NULL},
    {"gpio_write", (void *)fc_gpio_write, "(ii)i", NULL},
    {"i2c_open", (void *)fc_i2c_open, "(*~)i", NULL},
    {"i2c_read", (void *)fc_i2c_read, "(ii*~)i", NULL},
    {"i2c_write", (void *)fc_i2c_write, "(ii*~)i", NULL},
    {"i2c_write_read", (void *)fc_i2c_write_read, "(ii*~*~)i", NULL},
    {"info", (void *)fc_info, "(*~*~)i", NULL},
    {"kv_del", (void *)fc_kv_del, "(*~)i", NULL},
    {"kv_get", (void *)fc_kv_get, "(*~*~)i", NULL},
    {"kv_set", (void *)fc_kv_set, "(*~*~)i", NULL},
    {"led_open", (void *)fc_led_open, "(*~)i", NULL},
    {"led_rgb", (void *)fc_led_rgb, "(iiii)i", NULL},
    {"log", (void *)fc_log, "(i*~)", NULL},
    {"metric", (void *)fc_metric, "(*~iF)i", NULL},
    {"mqtt_publish", (void *)fc_mqtt_publish, "(*~*~ii)i", NULL},
    {"mqtt_subscribe", (void *)fc_mqtt_subscribe, "(i*~i)i", NULL},
    {"mqtt_topic", (void *)fc_mqtt_topic, "(*~)i", NULL},
    {"pwm_duty", (void *)fc_pwm_duty, "(ii)i", NULL},
    {"pwm_open", (void *)fc_pwm_open, "(*~i)i", NULL},
    {"random", (void *)fc_random, "(*~)i", NULL},
    {"sensor_read", (void *)fc_sensor_read, "(*~i)i", NULL},
    {"set_state", (void *)fc_apps_set_state_native, "(*~)", NULL},
    {"sleep_ms", (void *)fc_sleep_ms, "(i)i", NULL},
    {"spi_open", (void *)fc_spi_open, "(*~)i", NULL},
    {"spi_transfer", (void *)fc_spi_transfer, "(iiiii)i", NULL},
    {"time_ms", (void *)fc_time_ms, "()I", NULL},
    {"time_unix_ms", (void *)fc_time_unix_ms, "()I", NULL},
    {"timer_start", (void *)fc_timer_start, "(ii)i", NULL},
    {"uart_open", (void *)fc_uart_open, "(*~ii)i", NULL},
    {"uart_read", (void *)fc_uart_read, "(i*~i)i", NULL},
    {"uart_write", (void *)fc_uart_write, "(i*~)i", NULL},
};

NativeSymbol *fc_appapi_natives(int *count)
{
    *count = (int)(sizeof(s_natives) / sizeof(s_natives[0]));
    return (NativeSymbol *)s_natives;
}
