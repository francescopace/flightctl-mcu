/*
 * WASM apps (fc_apps.c, fc_appapi.c, fc_board.c) with WAMR and the in-memory
 * port: install, update, a module that cannot be stored or loaded, the apps
 * coming back after a reboot, removal, an app that never returns; and the
 * interface against a fake board (below): imports, timers, config, storage,
 * GPIO and LED, grants, the board map, and the example apps of wasm-apps/.
 * Downloads are replaced by fc_oci_fetch_layer below; the modules are in
 * wasm_vectors.h (tests/unit/gen-wasm-vectors.sh).
 */
#include <stdlib.h>
#include "../../core/src/fc_apps_int.h"

/* Fail descriptor allocations in the app manager only. */
static int f_slots_left = -1, f_no_io;
static void *app_calloc(size_t n, size_t size)
{
    if (size == sizeof(app_io_t) && f_no_io) {
        return NULL;
    }
    if (size == sizeof(app_t) && f_slots_left >= 0) {
        if (f_slots_left == 0) {
            return NULL;
        }
        f_slots_left--;
    }
    return calloc(n, size);
}
/* Stop the transaction at the catalog write. The module write succeeds. */
#include "fc_port.h"
#include <string.h>
static bool f_catalog_failure, f_catalog_visible;
static int app_kv_set(const char *key, const void *data, size_t len)
{
    if (f_catalog_failure && strcmp(key, "apps") == 0) {
        if (f_catalog_visible) {
            fc_port_kv_set(key, data, len); /* rename succeeded but its sync failed */
        }
        return -1;
    }
    return fc_port_kv_set(key, data, len);
}
#define fc_port_kv_set app_kv_set
#define calloc app_calloc
#include "../../core/src/fc_apps.c"
#undef calloc
#undef fc_port_kv_set

#ifdef __APPLE__
#include <malloc/malloc.h>
#define usable_size malloc_size
#else
#include <malloc.h>
#define usable_size malloc_usable_size
#endif

#include "port_stub.h"
#include "test.h"
#include "wasm_vectors.h"
#include "arduino_vectors.h"

/* An app that sets its state to "v1" (tests/unit/wasm/t_state.c); "v2" is the
 * same module with that string changed (make_v2). */
#define s_v1 w_t_state
static unsigned char s_v2[sizeof(s_v1)];

/* What removing or updating an app that runs may take: it stops at its next
 * branch, so this bound, far past that on any host, only tells it from one
 * never stopped, or stopped at the end of a wait (30 s in test_entry_waits). */
#define STOP_MS 5000

/* A module whose start section loops forever, and that exports tick(): clang
 * does not write a start section, so by hand:
 *   (module (type (func)) (func $s (loop (br 0))) (func $t)
 *           (export "tick" (func $t)) (start $s)) */
static const unsigned char s_startloop[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,                   /* magic, version */
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                               /* type: () -> () */
    0x03, 0x03, 0x02, 0x00, 0x00,                                     /* two functions of it */
    0x07, 0x08, 0x01, 0x04, 't', 'i', 'c', 'k', 0x00, 0x01,           /* export "tick": 1 */
    0x08, 0x01, 0x00,                                                 /* start: 0 */
    0x0a, 0x0c, 0x02, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b, /* code: loop br 0 */
    0x02, 0x00, 0x0b,                                                 /*       and an empty one */
};

/* (module (memory 1 2) (func (export "tick") (drop (memory.grow (i32.const 0))))) */
static const unsigned char s_growing[] = {
    0, 'a', 's', 'm', 1, 0, 0, 0,
    1, 4, 1, 0x60, 0, 0,
    3, 2, 1, 0,
    5, 4, 1, 1, 1, 2,
    7, 8, 1, 4, 't', 'i', 'c', 'k', 0, 0,
    10, 9, 1, 7, 0, 0x41, 0, 0x40, 0, 0x1a, 0x0b,
};

static void make_v2(void)
{
    memcpy(s_v2, s_v1, sizeof(s_v1));
    for (size_t i = 0; i + 1 < sizeof(s_v2); i++) {
        if (s_v2[i] == 'v' && s_v2[i + 1] == '1' && i + 2 < sizeof(s_v2) && s_v2[i + 2] == 0) {
            s_v2[i + 1] = '2';
            return;
        }
    }
    fprintf(stderr, "no \"v1\" in the test module\n");
    exit(1);
}

void fc_agent_notify_status(void) {}
/* The agent's work on hand: test_entry_waits sets it. */
static int f_busy;
int fc_agent_busy(void) { return __atomic_load_n(&f_busy, __ATOMIC_ACQUIRE); }

/* The registry: three images. */
static int f_fetches; /* downloads asked for */

int fc_oci_fetch_layer(const char *image, const char *arch, const char *layer_type, size_t max_size,
                       unsigned char **out, size_t *out_len, char *digest, size_t digest_len, char *err,
                       size_t err_len)
{
    static const unsigned char junk[] = "\0asm but not really";
    static const struct {
        const char *image;
        const unsigned char *code;
        size_t len;
    } reg[] = {
        {"reg/app:v1", s_v1, sizeof(s_v1)},
        {"reg/app:v2", s_v2, sizeof(s_v2)},
        {"reg/app:bad", junk, sizeof(junk)},
        {"reg/app:loop", w_loop, sizeof(w_loop)},
        {"reg/t_startloop", s_startloop, sizeof(s_startloop)},
        {"reg/growing", s_growing, sizeof(s_growing)},
#define V(n) {"reg/" #n, w_##n, sizeof(w_##n)}
        V(t_imports), V(t_timer), V(t_config), V(t_kv), V(t_gpio), V(t_perm), V(t_sleep), V(t_oob),
        V(t_badsig), V(t_unknown), V(t_mixed), V(t_events), V(t_neglen), V(t_info), V(app_alarm_v1), V(app_alarm_v2), V(app_trend), V(app_hwcheck), V(app_mqttdemo), V(app_tinyml), V(app_tinymlp), V(t_env),
        V(t_entry), V(t_start), V(t_tail), V(t_envtype), V(t_envother), V(t_bus), V(t_serial),
        V(t_libc), V(t_libcoob_1), V(t_libcoob_2), V(t_libcoob_3), V(t_libcoob_4), V(t_libcoob_5), V(t_libcloop), V(t_libcmix), V(t_libcmath), V(t_malloc), V(t_ctorloop), V(t_ctorfc), V(t_mqtt),
        V(t_arduino), V(t_arduinotick), V(arduino_rust), V(arduino_cpp), V(arduino_assemblyscript), V(arduino_tinygo),
#undef V
    };
    f_fetches++;
    const unsigned char *src = NULL;
    size_t len = 0;
    for (size_t i = 0; i < sizeof(reg) / sizeof(reg[0]); i++) {
        if (strcmp(image, reg[i].image) == 0) {
            src = reg[i].code;
            len = reg[i].len;
        }
    }
    if (src == NULL) {
        snprintf(err, err_len, "%s: not found", image);
        return -1;
    }
    if (len > max_size) { /* as fc_oci.c: not downloaded */
        snprintf(err, err_len, "%s: layer is %ld bytes (max %u)", image, (long)len, (unsigned)max_size);
        return FC_OCI_REFUSED;
    }
    *out = malloc(len);
    memcpy(*out, src, len);
    *out_len = len;
    snprintf(digest, digest_len, "sha256:%s", image);
    return 0;
}

static int apply(const char *image, char *err, size_t err_len, int *retry)
{
    cJSON *apps = cJSON_CreateArray();
    if (image != NULL) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", "app");
        cJSON_AddStringToObject(a, "appType", "container");
        cJSON_AddStringToObject(a, "image", image);
        cJSON_AddItemToArray(apps, a);
    }
    err[0] = '\0';
    int rc = fc_apps_apply(apps, err, err_len, retry);
    cJSON_Delete(apps);
    return rc;
}

/* ---- a fake board ------------------------------------------------------------- */

static int f_level[64], f_mode[64], f_released[64];
static void (*f_cb[64])(void *);
static void *f_ctx[64];
static int f_led_open, f_led_writes, f_led_on_writes, f_led_off_writes;
static uint8_t f_led[3];
static int32_t f_temp = 41000;
static int32_t f_temp_step; /* added to die_temp at each read */
static int32_t f_temp_reads = -1; /* reads of die_temp left before they fail, or -1 */
static pthread_mutex_t f_lock = PTHREAD_MUTEX_INITIALIZER;

static char *f_defaults(void)
{
    return strdup("{\"gpio\": {\"button0\": {\"pin\": 9, \"active_low\": true}, \"relay1\": {\"pin\": 5, \"safe\": 0}},"
                  " \"led\": {\"led0\": {\"pin\": 8}}, \"i2c\": {\"i2c0\": {\"sda\": 1, \"scl\": 2}},"
                  " \"adc\": {\"level\": {\"pin\": 3}}, \"pwm\": {\"fan\": {\"pin\": 4, \"hz\": 25000}},"
                  " \"spi\": {\"flash\": {\"sck\": 20, \"mosi\": 21, \"miso\": 22, \"cs\": 23, \"mode\": 3, \"hz\": 8000000},"
                  " \"disp\": {\"sck\": 20, \"mosi\": 21, \"miso\": 22, \"cs\": 24}},"
                  " \"uart\": {\"rs485\": {\"tx\": 25, \"rx\": 26, \"de\": 27, \"baud\": 9600}}}");
}
static int f_reserved(int pin) { return pin == 11 || pin >= 40; }
static int f_setup(int pin, int mode)
{
    pthread_mutex_lock(&f_lock);
    f_mode[pin] = mode;
    f_released[pin] = 0;
    pthread_mutex_unlock(&f_lock);
    return 0;
}
static int f_read(int pin)
{
    pthread_mutex_lock(&f_lock);
    int v = f_level[pin];
    pthread_mutex_unlock(&f_lock);
    return v;
}
static int f_write(int pin, int level)
{
    pthread_mutex_lock(&f_lock);
    f_level[pin] = level;
    pthread_mutex_unlock(&f_lock);
    return 0;
}
static int f_irq(int pin, int edge, void (*cb)(void *), void *ctx)
{
    pthread_mutex_lock(&f_lock);
    f_cb[pin] = edge ? cb : NULL;
    f_ctx[pin] = edge ? ctx : NULL;
    pthread_mutex_unlock(&f_lock);
    return 0;
}
static void f_release(int pin)
{
    pthread_mutex_lock(&f_lock);
    f_released[pin] = 1;
    f_mode[pin] = FC_HW_GPIO_IN;
    pthread_mutex_unlock(&f_lock);
}
static int f_led_open_fn(const fc_hw_led_t *l)
{
    f_led_open = 1;
    return 0;
}
static int f_led_rgb(const fc_hw_led_t *l, uint8_t r, uint8_t g, uint8_t b)
{
    pthread_mutex_lock(&f_lock);
    f_led[0] = r;
    f_led[1] = g;
    f_led[2] = b;
    f_led_writes++;
    *(r || g || b ? &f_led_on_writes : &f_led_off_writes) += 1;
    pthread_mutex_unlock(&f_lock);
    return 0;
}
static void f_led_close(const fc_hw_led_t *l)
{
    f_led_open = 0;
    f_led[0] = f_led[1] = f_led[2] = 0;
}
static int f_sensor(const char *name, int32_t *v)
{
    if (strcmp(name, "die_temp") != 0) {
        return FC_HW_ENOENT;
    }
    if (__atomic_load_n(&f_temp_reads, __ATOMIC_ACQUIRE) == 0) {
        return FC_HW_EIO;
    }
    if (__atomic_load_n(&f_temp_reads, __ATOMIC_ACQUIRE) > 0) {
        __atomic_fetch_sub(&f_temp_reads, 1, __ATOMIC_ACQ_REL);
    }
    *v = __atomic_fetch_add(&f_temp, __atomic_load_n(&f_temp_step, __ATOMIC_ACQUIRE), __ATOMIC_ACQ_REL);
    return 0;
}

/* An I2C bus with one device at 0x48: four registers, the first byte
 * written is the register, then data from there; a read goes on from it. */
static uint8_t f_reg[4], f_reg_at;
static int f_i2c_open, f_i2c_hz = -1;
static int f_i2c_open_fn(const fc_hw_bus_t *b)
{
    f_i2c_open++;
    f_i2c_hz = b->hz;
    return 7; /* the port's handle, given back */
}
static int f_i2c_transfer(int bus, int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, int timeout_ms)
{
    if (bus != 7 || timeout_ms <= 0 || timeout_ms > 100) {
        return FC_HW_EINVAL;
    }
    if (addr != 0x48) {
        return FC_HW_EIO;
    }
    for (size_t i = 0; i < wl; i++) {
        if (i == 0) {
            f_reg_at = w[0] & 3;
        } else {
            f_reg[f_reg_at] = w[i];
            f_reg_at = (f_reg_at + 1) & 3;
        }
    }
    for (size_t i = 0; i < rl; i++) {
        r[i] = f_reg[f_reg_at];
        f_reg_at = (f_reg_at + 1) & 3;
    }
    return 0;
}
static void f_i2c_close(int bus) { f_i2c_open--; }
static int f_adc_pin = -1;
static int f_adc_open(int pin)
{
    f_adc_pin = pin;
    return 0;
}
static int f_adc_read(int ch) { return 1234; }
static void f_adc_close(int ch) { f_adc_pin = -1; }
static int f_pwm_pin = -1, f_pwm_hz, f_pwm_duty = -1;
static int f_pwm_open(int pin, int hz)
{
    f_pwm_pin = pin;
    f_pwm_hz = hz;
    f_pwm_duty = 0;
    return 3;
}
static int f_pwm_set(int ch, uint32_t duty)
{
    f_pwm_duty = ch == 3 ? (int)duty : -2;
    return 0;
}
static void f_pwm_close(int ch)
{
    f_pwm_pin = -1;
    f_pwm_duty = -1;
}

/* An SPI device whose MISO is its MOSI: what it is sent comes back. */
static int f_spi_mode = -1, f_spi_hz, f_spi_cs, f_spi_kept, f_spi_releases, f_spi_open_n;
static int f_spi_open(const fc_hw_bus_t *d)
{
    f_spi_mode = d->mode;
    f_spi_hz = d->hz;
    f_spi_cs = d->cs;
    f_spi_open_n++;
    return 5;
}
static int f_spi_transfer(int dev, const uint8_t *tx, uint8_t *rx, size_t len, int keep, int timeout_ms)
{
    for (size_t i = 0; rx != NULL && i < len; i++) {
        rx[i] = tx != NULL ? tx[i] : 0xFF;
    }
    f_spi_kept = keep;
    return dev == 5 ? 0 : FC_HW_EINVAL;
}
static void f_spi_release(int dev)
{
    f_spi_kept = 0;
    f_spi_releases++;
}
static void f_spi_close(int dev)
{
    f_spi_kept = 0;
    f_spi_open_n--;
}
/* A UART wired back to itself: what is written is read, and announced. */
static int f_uart_baud, f_uart_format = -1, f_uart_de = -2;
static uint8_t f_uart_buf[64];
static size_t f_uart_len;
static void (*f_uart_cb)(void *, int);
static void *f_uart_ctx;
static int f_uart_open(const fc_hw_bus_t *u, int baud, int format, void (*cb)(void *, int), void *ctx)
{
    f_uart_baud = baud;
    f_uart_format = format;
    f_uart_de = u->de;
    f_uart_cb = cb;
    f_uart_ctx = ctx;
    f_uart_len = 0;
    return 2;
}
static int f_uart_write(int h, const uint8_t *d, size_t len, int timeout_ms)
{
    size_t n = len < sizeof(f_uart_buf) - f_uart_len ? len : sizeof(f_uart_buf) - f_uart_len;
    memcpy(f_uart_buf + f_uart_len, d, n);
    f_uart_len += n;
    if (f_uart_cb && n > 0) {
        f_uart_cb(f_uart_ctx, (int)f_uart_len);
    }
    return (int)n;
}
static int f_uart_read(int h, uint8_t *b, size_t cap, int timeout_ms)
{
    size_t n = cap < f_uart_len ? cap : f_uart_len;
    memcpy(b, f_uart_buf, n);
    memmove(f_uart_buf, f_uart_buf + n, f_uart_len - n);
    f_uart_len -= n;
    return (int)n;
}
static void f_uart_close(int h)
{
    f_uart_cb = NULL;
    f_uart_baud = 0;
}

/* How the fake device last restarted: 1 a crash, 0 not. */
static int f_crash;
static int f_restart_was_crash(void)
{
    return f_crash;
}

static fc_hw_t s_fake = {
    .board = "fake",
    .firmware = "test",
    .board_defaults = f_defaults,
    .pin_reserved = f_reserved,
    .gpio_setup = f_setup,
    .gpio_read = f_read,
    .gpio_write = f_write,
    .gpio_irq = f_irq,
    .gpio_release = f_release,
    .led_open = f_led_open_fn,
    .led_rgb = f_led_rgb,
    .led_close = f_led_close,
    .sensor_read = f_sensor,
    .restart_was_crash = f_restart_was_crash,
    .i2c_open = f_i2c_open_fn,
    .i2c_transfer = f_i2c_transfer,
    .i2c_close = f_i2c_close,
    .adc_open = f_adc_open,
    .adc_read_mv = f_adc_read,
    .adc_close = f_adc_close,
    .pwm_open = f_pwm_open,
    .pwm_duty = f_pwm_set,
    .pwm_close = f_pwm_close,
    .spi_open = f_spi_open,
    .spi_transfer = f_spi_transfer,
    .spi_release = f_spi_release,
    .spi_close = f_spi_close,
    .uart_open = f_uart_open,
    .uart_write = f_uart_write,
    .uart_read = f_uart_read,
    .uart_close = f_uart_close,
};

/* An edge on a pin, as the port's task reports it. */
static void f_edge(int pin, int level)
{
    pthread_mutex_lock(&f_lock);
    f_level[pin] = level;
    void (*cb)(void *) = f_cb[pin];
    void *ctx = f_ctx[pin];
    pthread_mutex_unlock(&f_lock);
    if (cb) {
        cb(ctx);
    }
}

/* Applies a spec of apps: "name image env" triples (env: JSON or NULL). */
static int apply_apps(const char *const *apps, char *err, size_t err_len)
{
    cJSON *list = cJSON_CreateArray();
    for (; apps[0] != NULL; apps += 3) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", apps[0]);
        cJSON_AddStringToObject(a, "appType", "container");
        cJSON_AddStringToObject(a, "image", apps[1]);
        if (apps[2] != NULL) {
            cJSON_AddItemToObject(a, "envVars", cJSON_Parse(apps[2]));
        }
        cJSON_AddItemToArray(list, a);
    }
    int retry;
    err[0] = '\0';
    int rc = fc_apps_apply(list, err, err_len, &retry);
    cJSON_Delete(list);
    return rc;
}

static int apply_one(const char *name, const char *image, const char *env, char *err, size_t err_len)
{
    const char *apps[] = {name, image, env, NULL};
    return apply_apps(apps, err, err_len);
}

/* "status state" of an app, or "none". */
static const char *state_of(const char *name)
{
    static char out[200];
    app_t *a = find(name);
    pthread_mutex_lock(&s_lock);
    snprintf(out, sizeof(out), "%s %s", a ? a->status : "none", a ? (a->error[0] ? a->error : a->state) : "");
    pthread_mutex_unlock(&s_lock);
    return a ? out : "none";
}

static const char *wait_state_of(const char *name, const char *want, int ms)
{
    for (int i = 0; i < ms / 10 && strcmp(state_of(name), want) != 0; i++) {
        usleep(10 * 1000);
    }
    return state_of(name);
}

/* "status state image" of the app, or "none". */
static const char *app_state(void)
{
    static char out[300];
    app_t *a = find("app");
    pthread_mutex_lock(&s_lock);
    snprintf(out, sizeof(out), "%s %s %s", a ? a->status : "none", a ? a->state : "", a ? a->image : "");
    pthread_mutex_unlock(&s_lock);
    return a ? out : "none";
}

/* Waits up to 3 s for the app to reach `want` (see app_state). */
static const char *wait_state(const char *want)
{
    for (int i = 0; i < 300 && strcmp(app_state(), want) != 0; i++) {
        usleep(10 * 1000);
    }
    return app_state();
}

/* What is stored for the app is exactly the downloaded module. */
static int stored_is(const unsigned char *m)
{
    char *v = NULL;
    size_t len = 0;
    char key[64];
    wasm_key("app", find("app")->module_slot, key, sizeof(key));
    int same = fc_port_kv_get(key, &v, &len) == 0 && len == sizeof(s_v1) && memcmp(v, m, len) == 0;
    free(v);
    return same;
}

/* Stops everything as a power cut would, then boots: fc_apps_init again. */
static void power_cycle(void)
{
    for (int i = 0; i < FC_MAX_APPS; i++) {
        if (s_apps[i]) {
            app_release(s_apps[i], false);
            drop_slot(s_apps[i]);
        }
    }
    cJSON_Delete(s_restore);
    s_restore = NULL;
    wasm_runtime_destroy();
    free(s_catalog_rollback);
    s_catalog_rollback = NULL;
    s_inited = 0;
    CHECK_INT(fc_apps_init(), 0);
}

static void test_install_and_update(void)
{
    char err[160];
    int retry;
    CHECK_INT(apply("reg/app:v1", err, sizeof(err), &retry), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    CHECK(stored_is(s_v1)); /* loading left the downloaded bytes as they were */

    CHECK_INT(apply("reg/app:v2", err, sizeof(err), &retry), 0);
    CHECK_STR(wait_state("Running v2 reg/app:v2"), "Running v2 reg/app:v2");
    CHECK(stored_is(s_v2));
}

static void test_cannot_store(void)
{
    char err[160];
    int retry;
    stub_kv_set_fails = 1;
    CHECK_INT(apply("reg/app:v1", err, sizeof(err), &retry), -1);
    stub_kv_set_fails = 0;
    CHECK(strstr(err, "cannot store the module") != NULL);
    CHECK_INT(retry, 1);
    /* The running version is untouched, in RAM and in storage. */
    CHECK_STR(app_state(), "Running v2 reg/app:v2");
    CHECK(stored_is(s_v2));
}

static void test_not_loadable(void)
{
    char err[160];
    int retry;
    CHECK_INT(apply("reg/app:bad", err, sizeof(err), &retry), -1);
    CHECK(strstr(err, "not a loadable WASM module") != NULL);
    CHECK_INT(retry, 0);
    CHECK_STR(app_state(), "Running v2 reg/app:v2");
    CHECK(stored_is(s_v2));
}

static void test_reboot(void)
{
    power_cycle();
    CHECK_STR(wait_state("Running v2 reg/app:v2"), "Running v2 reg/app:v2");
}

static void test_saved_module_broken(void)
{
    /* A module that firmware with the earlier interface saved: refused at
     * boot too, as an error in the status, and the device boots. */
    char key[64];
    wasm_key("app", find("app")->module_slot, key, sizeof(key));
    fc_port_kv_set(key, (const char *)w_t_env, sizeof(w_t_env));
    power_cycle();
    CHECK(strncmp(app_state(), "Error", 5) == 0);
    fc_port_kv_set(key, "junk", 4);
    power_cycle();
    CHECK(strncmp(app_state(), "Error", 5) == 0);
    /* The same image in the spec downloads it again instead of skipping it. */
    char err[160];
    int retry;
    CHECK_INT(apply("reg/app:v2", err, sizeof(err), &retry), 0);
    CHECK_STR(wait_state("Running v2 reg/app:v2"), "Running v2 reg/app:v2");
    CHECK(stored_is(s_v2));
}

/* An app whose tick never returns traps on its instruction budget, and can
 * be removed: before metering, removing it waited for the tick forever. */
static void test_endless_loop(void)
{
    char err[160];
    int retry;
    s_call_budget = 2000000;
    CHECK_INT(apply("reg/app:loop", err, sizeof(err), &retry), 0);
    for (int i = 0; i < 300 && strncmp(app_state(), "Error", 5) != 0; i++) {
        usleep(10 * 1000);
    }
    CHECK_STR(app_state(), "Error looping reg/app:loop");
    pthread_mutex_lock(&s_lock);
    CHECK(strstr(find("app")->error, "instruction limit exceeded") != NULL);
    pthread_mutex_unlock(&s_lock);
    /* Removed while it may be in its loop again (restarted after the delay). */
    CHECK_INT(apply(NULL, err, sizeof(err), &retry), 0);
    CHECK_STR(app_state(), "none");
    s_call_budget = FC_APP_CALL_BUDGET;
}

static void remove_all(void)
{
    char err[160];
    const char *none[] = {NULL};
    CHECK_INT(apply_apps(none, err, sizeof(err)), 0);
}

static void test_fc_imports(void)
{
    char err[160];
    CHECK_INT(apply_one("i", "reg/t_imports", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("i", "Running all linked", 3000), "Running all linked");
    remove_all();
}

/* The C library: its results, and printf, puts and putchar in the app's
 * log a line at a time, the last part line at the end of the call. */
static void test_libc(void)
{
    char err[160];
    CHECK_INT(apply_one("l", "reg/t_libc", NULL, err, sizeof(err)), 0);
    const char *want = "Running copied 7 -42 255 ff 3.14 xyz 0 A bad=0";
    CHECK_STR(wait_state_of("l", want, 3000), want);
    CHECK(stub_log_has("app l: hello"));
    CHECK(stub_log_has("app l: n=5 more"));
    CHECK(stub_log_has("app l: ok"));
    CHECK(stub_log_has("app l: line"));
    CHECK(stub_log_has("app l: partial"));
    remove_all();
}

/* Addresses outside the app's memory given to the C library stop the app:
 * memcpy's destination, a %s, snprintf's size, strtol's endptr; and abort. */
static void test_libc_oob(void)
{
    static const char *const img[] = {"reg/t_libcoob_1", "reg/t_libcoob_2", "reg/t_libcoob_3", "reg/t_libcoob_4"};
    char err[160];
    for (size_t i = 0; i < sizeof(img) / sizeof(img[0]); i++) {
        CHECK_INT(apply_one("o", img[i], NULL, err, sizeof(err)), 0);
        for (int j = 0; j < 300 && strncmp(state_of("o"), "Error", 5) != 0; j++) {
            usleep(10 * 1000);
        }
        if (strstr(state_of("o"), "out of bounds") == NULL) {
            fprintf(stderr, "%s: %s\n", img[i], state_of("o"));
        }
        CHECK(strstr(state_of("o"), "out of bounds") != NULL);
        remove_all();
    }
    CHECK_INT(apply_one("o", "reg/t_libcoob_5", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("o", "Error Exception: abort()", 3000), "Error Exception: abort()");
    remove_all();
}

/* A loop of the C library's native calls is stopped by the limit on the time
 * spent in them, though it runs few of the app's instructions. No budget at
 * all, so that only that limit can stop it, however fast or slow the host;
 * the limit is 50 ms instead of a second, to be quick. */
static void test_libc_loop(void)
{
    char err[160];
    s_call_budget = -1;
    s_native_limit_us = 50 * 1000;
    CHECK_INT(apply_one("p", "reg/t_libcloop", NULL, err, sizeof(err)), 0);
    const char *want = "Error Exception: a call spent more than 1 s in the device's functions";
    CHECK_STR(wait_state_of("p", want, 30000), want);
    remove_all();
    s_call_budget = FC_APP_CALL_BUDGET;
    s_native_limit_us = (int64_t)APP_CALL_MS * 1000;
}

/* Only the time spent in the C library counts against its limit: a call of
 * the app's own instructions with a C library call now and then runs past
 * that limit in time and is not stopped. Before, the whole call's time
 * counted, and on the slower chips such a loop was stopped after a second,
 * before its budget. No budget at all, so that the call lasts as long as the
 * test waits, however fast or slow the host: three times the limit. */
static void test_libc_mixed(void)
{
    char err[160];
    s_native_limit_us = 50 * 1000;
    s_call_budget = -1;
    CHECK_INT(apply_one("m", "reg/t_libcmix", NULL, err, sizeof(err)), 0);
    int64_t ms = 0, native_us = 0;
    for (int i = 0; i < 30000 && strncmp(state_of("m"), "Error", 5) != 0; i++) {
        pthread_mutex_lock(&s_lock);
        const app_t *a = find("m");
        ms = a->run_env != NULL ? fc_apps_now_ms() - a->call_start_ms : 0; /* tick() in progress: it never returns */
        native_us = __atomic_load_n(&a->native_us, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&s_lock);
        if (ms * 1000 > 3 * s_native_limit_us) {
            break;
        }
        usleep(1000);
    }
    CHECK_STR(state_of("m"), "Running ");
    if (ms * 1000 <= 3 * s_native_limit_us || native_us > s_native_limit_us) {
        fprintf(stderr, "the call ran %ld ms, %ld us of them in the C library\n", (long)ms, (long)native_us);
    }
    CHECK(ms * 1000 > 3 * s_native_limit_us);
    CHECK(native_us <= s_native_limit_us);
    remove_all();
    s_call_budget = FC_APP_CALL_BUDGET;
    s_native_limit_us = (int64_t)APP_CALL_MS * 1000;
}

/* math.h: expf, logf and tanhf, the device's, give C's values within a few
 * ulps and at the edges; the instructions too. */
static void test_libc_math(void)
{
    char err[160];
    CHECK_INT(apply_one("f", "reg/t_libcmath", NULL, err, sizeof(err)), 0);
    const char *want = "Running bad=0 3.8735";
    CHECK_STR(wait_state_of("f", want, 3000), want);
    remove_all();
}

/* stdlib.h's malloc, in the app's own memory: no import, and the device
 * loads it as any app (WAMR still shrinks its memory, which has no
 * memory.grow). The churn's counts are the module's, the same on any host. */
static void test_malloc(void)
{
    char err[160];
    CHECK_INT(apply_one("a", "reg/t_malloc", NULL, err, sizeof(err)), 0);
    const char *want = "Running bad=0 allocs=1827 nulls=357";
    CHECK_STR(wait_state_of("a", want, 3000), want);
    remove_all();
}

static void test_fc_info(void)
{
    char err[160];
    CHECK_INT(apply_one("n", "reg/t_info", NULL, err, sizeof(err)), 0);
    /* Not enrolled yet: the device's name is FC_EAGAIN; an unknown key FC_ENOENT. */
    CHECK_STR(wait_state_of("n", "Running n fake test -9 -2", 3000), "Running n fake test -9 -2");
    remove_all();
    fc_apps_set_device("dev-1234");
    CHECK_INT(apply_one("n", "reg/t_info", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("n", "Running n fake test dev-1234 -2", 3000), "Running n fake test dev-1234 -2");
    remove_all();
    fc_apps_set_device(NULL);
}

/* The native tables are const, so WAMR must find them in order: one out of
 * order would be sorted in place at start, a write to flash on a chip. */
static void check_sorted(const char *what, const NativeSymbol *t, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        if (strcmp(t[i - 1].symbol, t[i].symbol) > 0) {
            fprintf(stderr, "%s: \"%s\" comes before \"%s\"\n", what, t[i - 1].symbol, t[i].symbol);
            CHECK(0);
        }
    }
}

static void test_native_tables_sorted(void)
{
    int n_fc = 0;
    const NativeSymbol *fc = fc_appapi_natives(&n_fc);
    check_sorted("fc", fc, (size_t)n_fc);
    check_sorted("env (libc)", s_libc_natives, sizeof(s_libc_natives) / sizeof(s_libc_natives[0]));
    check_sorted("env (raw)", s_env_natives, sizeof(s_env_natives) / sizeof(s_env_natives[0]));
    int n_ard = 0;
    const NativeSymbol *ard = fc_arduino_natives(&n_ard);
    check_sorted("arduino", ard, (size_t)n_ard);
    CHECK_INT(sizeof(s_libc_natives) / sizeof(s_libc_natives[0]), 38);
}

static void test_fc_refused(void)
{
    char err[160];
    CHECK_INT(apply_one("r", "reg/t_badsig", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "tick() has the wrong signature") != NULL);
    CHECK_INT(apply_one("r", "reg/t_unknown", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "imports fc.teleport, which this firmware does not have") != NULL);
    CHECK_INT(apply_one("r", "reg/t_mixed", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "built for the earlier app interface") != NULL);
    CHECK_INT(apply_one("r", "reg/t_env", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "built for the earlier app interface (imports env.set_state)") != NULL);
    CHECK_INT(apply_one("r", "reg/t_envtype", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "imports env.clock_ms with a type other than () -> i32 or i64") != NULL);
    CHECK_INT(apply_one("r", "reg/t_envother", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "imports env.malloc, which this firmware does not have") != NULL);
    CHECK_INT(apply_one("r", "reg/t_entry", NULL, err, sizeof(err)), -1); /* run() only, and no FC_ENTRY */
    CHECK(strstr(err, "exports neither tick() nor on_event() nor _start(), and has no FC_ENTRY") != NULL);
    CHECK_INT(apply_one("r", "reg/t_entry", "{\"FC_ENTRY\": \"walk\"}", err, sizeof(err)), -1);
    CHECK(strstr(err, "FC_ENTRY: exports no walk() without parameters") != NULL);
    CHECK_INT(apply_one("r", "reg/app:v1", "{\"FC_ENTRY\": \"tick\"}", err, sizeof(err)), -1);
    CHECK(strstr(err, "runs once (FC_ENTRY or _start) and imports from \"fc\": not supported yet") != NULL);
    CHECK_STR(state_of("r"), "none");
}

/* A module built for no device in particular, as CoreMark is: FC_ENTRY's
 * export runs once, past the budget of a call, reads env.clock_ms, and its
 * result is the app's state. */
static void test_entry(void)
{
    char err[160];
    s_call_budget = 200000; /* run() takes 300 ms: far more than this */
    CHECK_INT(apply_one("e", "reg/t_entry", "{\"FC_ENTRY\": \"run\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("e", "Completed run() returned 1234.568", 3000), "Completed run() returned 1234.568");
    usleep(200 * 1000); /* not run again */
    CHECK_STR(state_of("e"), "Completed run() returned 1234.568");
    pthread_mutex_lock(&s_lock);
    CHECK_INT(find("e")->restarts, 0);
    pthread_mutex_unlock(&s_lock);
    cJSON *apps = cJSON_CreateArray(), *summary = cJSON_CreateObject(), *info = cJSON_CreateObject();
    fc_apps_status(apps, summary, info);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(summary, "status")), "Healthy");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(info, "app.e")), "run() returned 1234.568");
    cJSON_Delete(apps);
    cJSON_Delete(summary);
    cJSON_Delete(info);
    remove_all();
    s_call_budget = FC_APP_CALL_BUDGET;
}

/* _start() that never returns, in a module with neither tick() nor
 * on_event(): removing it stops the call in progress. */
static void test_entry_stopped(void)
{
    char err[160];
    CHECK_INT(apply_one("s", "reg/t_start", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("s", "Running ", 3000), "Running ");
    usleep(100 * 1000);
    int64_t t0 = fc_apps_now_ms();
    remove_all();
    int64_t ms = fc_apps_now_ms() - t0;
    CHECK(ms < STOP_MS);
    CHECK_STR(state_of("s"), "none");
    /* Updated (new env vars) while in its loop: stopped and run again. */
    CHECK_INT(apply_one("s", "reg/t_start", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("s", "Running ", 3000), "Running ");
    t0 = fc_apps_now_ms();
    CHECK_INT(apply_one("s", "reg/t_start", "{\"TICK_MS\": \"50\"}", err, sizeof(err)), 0);
    CHECK(fc_apps_now_ms() - t0 < STOP_MS);
    CHECK_STR(wait_state_of("s", "Running ", 3000), "Running ");
    remove_all();
}

/* A module run once starts when the agent has had nothing on hand for
 * ENTRY_QUIET_MS; or after s_entry_wait_ms in any case. That wait is 30 s
 * here, far past what a slow host takes for the quiet one and run(). */
static void test_entry_waits(void)
{
    char err[160];
    s_entry_wait_ms = 30000;
    __atomic_store_n(&f_busy, 1, __ATOMIC_RELEASE);
    CHECK_INT(apply_one("e", "reg/t_entry", "{\"FC_ENTRY\": \"run\"}", err, sizeof(err)), 0);
    usleep(1500 * 1000); /* run() takes 300 ms: it has not started */
    CHECK_STR(state_of("e"), "Preparing ");
    pthread_mutex_lock(&s_lock);
    CHECK_INT(find("e")->live, 1); /* its instance, and memory, made before the wait */
    pthread_mutex_unlock(&s_lock);
    int64_t t0 = fc_apps_now_ms();
    __atomic_store_n(&f_busy, 0, __ATOMIC_RELEASE);
    CHECK_STR(wait_state_of("e", "Completed run() returned 1234.568", 20000), "Completed run() returned 1234.568");
    int64_t ms = fc_apps_now_ms() - t0;
    CHECK(ms >= ENTRY_QUIET_MS);
    remove_all();
    /* The agent never quiet: it starts anyway. */
    s_entry_wait_ms = 300;
    __atomic_store_n(&f_busy, 1, __ATOMIC_RELEASE);
    CHECK_INT(apply_one("e", "reg/t_entry", "{\"FC_ENTRY\": \"run\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("e", "Completed run() returned 1234.568", 3000), "Completed run() returned 1234.568");
    remove_all();
    /* Removed while it waits: gone at once. */
    s_entry_wait_ms = 30000;
    CHECK_INT(apply_one("e", "reg/t_entry", "{\"FC_ENTRY\": \"run\"}", err, sizeof(err)), 0);
    usleep(200 * 1000);
    t0 = fc_apps_now_ms();
    remove_all();
    CHECK(fc_apps_now_ms() - t0 < STOP_MS);
    CHECK_STR(state_of("e"), "none");
    __atomic_store_n(&f_busy, 0, __ATOMIC_RELEASE);
    s_entry_wait_ms = 0;
}

/* Until the app's runner has let its instance go (live 0), which it does
 * after the status that says why ("Completed", "Error"), or 30 s. */
static void wait_idle(const char *name)
{
    for (int i = 0; i < 3000; i++) {
        pthread_mutex_lock(&s_lock);
        int live = find(name)->live;
        pthread_mutex_unlock(&s_lock);
        if (!live) {
            return;
        }
        usleep(10 * 1000);
    }
}

/* An app that does not run is released before its new image is loaded, and
 * says why when the new one cannot be had. */
static void test_replace_idle(void)
{
    char err[160];
    CHECK_INT(apply_one("r", "reg/t_entry", "{\"FC_ENTRY\": \"run\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("r", "Completed run() returned 1234.568", 3000), "Completed run() returned 1234.568");
    wait_idle("r"); /* "Completed" comes a moment before */
    app_t *finished = find("r");
    while (!__atomic_load_n(&finished->runner_done, __ATOMIC_ACQUIRE)) {
        usleep(10000);
    }
    fc_apps_poll(0);
    CHECK(!finished->thread_running && !finished->io);
    fc_apps_file_changed(FC_BOARD_PATH);
    CHECK_STR(state_of("r"), "Completed run() returned 1234.568");
    CHECK(apply_one("r", "reg/nothing", NULL, err, sizeof(err)) != 0);
    CHECK(find("r")->module == NULL); /* released: it had ended */
    CHECK_STR(state_of("r"), "Error reg/nothing: not found");
    /* After a restart the stored module, the old one, is what there is. */
    power_cycle();
    CHECK_STR(wait_state_of("r", "Completed run() returned 1234.568", 3000), "Completed run() returned 1234.568");
    /* The new image, once it can be had. */
    CHECK_INT(apply_one("r", "reg/t_tail", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("r", "Completed _start() returned 100000", 3000), "Completed _start() returned 100000");
    remove_all();
    /* One that waits to restart (it trapped) has no instance: released. */
    CHECK_INT(apply_one("r", "reg/app:loop", NULL, err, sizeof(err)), 0);
    for (int i = 0; i < 3000 && strncmp(state_of("r"), "Error", 5) != 0; i++) {
        usleep(10 * 1000);
    }
    wait_idle("r");
    CHECK(apply_one("r", "reg/nothing", NULL, err, sizeof(err)) != 0);
    CHECK(find("r")->module == NULL);
    remove_all();
    /* One that runs keeps running when its new image cannot be had. */
    CHECK_INT(apply_one("r", "reg/t_start", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("r", "Running ", 3000), "Running ");
    CHECK(apply_one("r", "reg/nothing", NULL, err, sizeof(err)) != 0);
    CHECK(find("r")->module != NULL);
    CHECK_STR(state_of("r"), "Running ");
    remove_all();
}

/* return_call: 100000 calls deep with WASM_STACK of 2 KB. */
static void test_tail_calls(void)
{
    char err[160];
    CHECK_INT(apply_one("t", "reg/t_tail", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("t", "Completed _start() returned 100000", 3000), "Completed _start() returned 100000");
    remove_all();
}

static void test_fc_timer(void)
{
    char err[160];
    CHECK_INT(apply_one("t", "reg/t_timer", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("t", "Running timer 3", 3000), "Running timer 3");
    usleep(150 * 1000); /* a closed timer fires no more */
    CHECK_STR(state_of("t"), "Running timer 3");
    remove_all();
}

static void test_fc_config(void)
{
    char err[160];
    const char *path = "/etc/fc-app/c.json";
    fc_port_write_file(path, "{\"greeting\": \"hello\"}", 21);
    CHECK_INT(apply_one("c", "reg/t_config", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("c", "Running hello", 3000), "Running hello");
    /* A new file reaches the running app, numbers as their shortest text. */
    const char *next = "{\"greeting\": 40.1}";
    fc_port_write_file(path, next, strlen(next));
    fc_apps_file_changed(path);
    CHECK_STR(wait_state_of("c", "Running 40.1", 3000), "Running 40.1");
    remove_all();
}

/* Call the registered KV imports on an isolated instance so each storage
 * failure can be injected between two calls, independently of app startup. */
static void kv_storage_errors(app_t *app)
{
    int count;
    const NativeSymbol *api = fc_appapi_natives(&count);
    int32_t (*get)(wasm_exec_env_t, const char *, int32_t, void *, int32_t) = NULL;
    int32_t (*set)(wasm_exec_env_t, const char *, int32_t, const void *, int32_t) = NULL;
    int32_t (*del)(wasm_exec_env_t, const char *, int32_t) = NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(api[i].symbol, "kv_get") == 0) {
            get = api[i].func_ptr;
        } else if (strcmp(api[i].symbol, "kv_set") == 0) {
            set = api[i].func_ptr;
        } else if (strcmp(api[i].symbol, "kv_del") == 0) {
            del = api[i].func_ptr;
        }
    }
    CHECK(get && set && del);
    if (!get || !set || !del) return;
    char err[128], value;
    wasm_module_inst_t inst = wasm_runtime_instantiate(app->module, WASM_STACK, 0, err, sizeof(err));
    CHECK(inst != NULL);
    if (inst == NULL) return;
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, WASM_STACK);
    CHECK(env != NULL);
    if (env == NULL) {
        wasm_runtime_deinstantiate(inst);
        return;
    }
    app_t owner = {.name = "kv-test"};
    wasm_runtime_set_user_data(env, &owner);

    CHECK_INT(set(env, "x", 1, "a", 1), 0);
    stub_kv_set_fails = 1;
    CHECK_INT(set(env, "x", 1, "a", 1), 0);
    CHECK_INT(set(env, "x", 1, "b", 1), FC_HW_EIO);
    stub_kv_set_fails = 0;
    stub_kv_get_fails = 1;
    CHECK_INT(get(env, "x", 1, &value, 1), FC_HW_EIO);
    CHECK_INT(set(env, "y", 1, "b", 1), FC_HW_EIO);
    CHECK_INT(del(env, "x", 1), FC_HW_EIO);
    stub_kv_get_fails = 0;
    stub_kv_del_fails = 1;
    CHECK_INT(del(env, "x", 1), FC_HW_EIO);
    stub_kv_del_fails = 0;
    CHECK_INT(get(env, "x", 1, &value, 1), 1);
    CHECK_INT(value, 'a');
    CHECK_INT(del(env, "x", 1), 0);
    CHECK_INT(get(env, "x", 1, &value, 1), FC_HW_ENOENT);

    const unsigned char truncated[] = {1, 'x', 3, 0, 'a'};
    CHECK_INT(fc_port_kv_set("akv:kv-test", truncated, sizeof(truncated)), 0);
    CHECK_INT(set(env, "x", 1, "b", 1), FC_HW_EIO);
    CHECK(memcmp(stub_kv("akv:kv-test"), truncated, sizeof(truncated)) == 0);
    fc_port_kv_del("akv:kv-test");
    wasm_runtime_destroy_exec_env(env);
    wasm_runtime_deinstantiate(inst);
}

static void test_fc_kv(void)
{
    char err[160];
    CHECK_INT(apply_one("k", "reg/t_kv", NULL, err, sizeof(err)), 0);
    /* count 1; a 34-character key EINVAL; 4097 bytes ENOSPC; a missing key
     * ENOENT; 4000 bytes fit with "count". */
    CHECK_STR(wait_state_of("k", "Running c 1 -1 -7 -2 0", 3000), "Running c 1 -1 -7 -2 0");
    power_cycle();
    CHECK_STR(wait_state_of("k", "Running c 2 -1 -7 -2 0", 3000), "Running c 2 -1 -7 -2 0");
    CHECK(stub_kv("akv:k") != NULL);
    kv_storage_errors(find("k"));
    remove_all();
    CHECK(stub_kv("akv:k") == NULL); /* removed with the app */
}

static void test_fc_gpio(void)
{
    char err[160];
    f_level[9] = 1; /* button0, active low: not pressed */
    CHECK_INT(apply_one("g", "reg/t_gpio", "{\"FC_ALLOW\": \"relay1 button0 led0\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("g", "Running t 41000 0", 3000), "Running t 41000 0");
    CHECK_INT(f_read(5), 1);
    CHECK_INT(f_mode[5], FC_HW_GPIO_OUT);
    CHECK_INT(f_mode[9], FC_HW_GPIO_IN_PULLUP);
    CHECK(f_led_open && f_led[0] == 1 && f_led[1] == 2 && f_led[2] == 255);
    f_edge(9, 0); /* pressed: logical 1 */
    CHECK_STR(wait_state_of("g", "Running edge 1", 3000), "Running edge 1");
    f_edge(9, 1);
    CHECK_STR(wait_state_of("g", "Running edge 0", 3000), "Running edge 0");
    /* Another app cannot take what it holds. */
    const char *two[] = {"g", "reg/t_gpio", "{\"FC_ALLOW\": \"relay1 button0 led0\"}",
                         "p", "reg/t_perm", "{\"FC_ALLOW\": \"relay1 i2c0\"}", NULL};
    CHECK_INT(apply_apps(two, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("p", "Running -4 -2 0 -3 -4", 3000), "Running -4 -2 0 -3 -4");
    /* Stopped: the relay to its safe level, the button released, the LED off. */
    remove_all();
    CHECK_INT(f_read(5), 0);
    CHECK(f_released[9]);
    CHECK(f_cb[9] == NULL);
    CHECK(!f_led_open);
}

/* "metrics" granted: fc_metric keeps the value for the gateway, until the
 * app goes. */
static void test_fc_metric(void)
{
    char err[160];
    CHECK_INT(apply_one("m", "reg/t_perm", "{\"FC_ALLOW\": \"i2c0 metrics\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("m", "Running -3 -2 0 0 -3", 3000), "Running -3 -2 0 0 -3");
    fc_metric_point_t p[FC_METRICS_APPS_MAX];
    CHECK_INT(fc_metrics_app_points(p, FC_METRICS_APPS_MAX), 1);
    CHECK_STR(p[0].name, "app.m.x");
    CHECK(p[0].d == 1.0 && !p[0].sum);
    remove_all();
    CHECK_INT(fc_metrics_app_points(p, FC_METRICS_APPS_MAX), 0);
}

/* I2C, ADC and PWM through the port; closed with the app, the PWM low. */
static void test_fc_bus(void)
{
    char err[160];
    CHECK_INT(apply_one("b", "reg/t_bus", "{\"FC_ALLOW\": \"i2c0 level fan\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("b", "Running i2c 42 nack -5 long -1 adc 1234 pwm -1", 3000),
              "Running i2c 42 nack -5 long -1 adc 1234 pwm -1");
    CHECK_INT(f_i2c_open, 1);
    CHECK_INT(f_i2c_hz, 0); /* the map gives none: the port's default */
    CHECK_INT(f_adc_pin, 3);
    CHECK_INT(f_pwm_pin, 4);
    CHECK_INT(f_pwm_hz, 25000); /* the board map's: the app gave 0 */
    CHECK_INT(f_pwm_duty, 32768);
    remove_all();
    CHECK_INT(f_i2c_open, 0);
    CHECK_INT(f_adc_pin, -1);
    CHECK_INT(f_pwm_duty, -1);
}

/* SPI and UART: a kept chip select ends with the call; UART data raises an
 * event, read in it. */
static void test_fc_serial(void)
{
    char err[160];
    CHECK_INT(apply_one("s", "reg/t_serial", "{\"FC_ALLOW\": \"flash rs485\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("s", "Running uart 2 hi", 3000), "Running uart 2 hi");
    CHECK_INT(f_spi_mode, 3);
    CHECK_INT(f_spi_hz, 8000000);
    CHECK_INT(f_spi_cs, 23);
    CHECK_INT(f_spi_kept, 0);      /* released at the end of init */
    CHECK_INT(f_spi_releases, 1);  /* once: the second transfer ended the first hold */
    CHECK_INT(f_uart_baud, 9600);  /* the board map's: the app gave 0 */
    CHECK_INT(f_uart_format, FC_HW_UART_8E1);
    CHECK_INT(f_uart_de, 27);
    remove_all();
    CHECK_INT(f_spi_open_n, 0);
    CHECK(f_uart_cb == NULL);
}

static void test_fc_grants(void)
{
    char err[160];
    /* Nothing granted: EPERM, but a name the map lacks is ENOENT. */
    CHECK_INT(apply_one("p", "reg/t_perm", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("p", "Running -3 -2 -3 -3 -3", 3000), "Running -3 -2 -3 -3 -3");
    /* New env vars restart it with the new grants: relay1 opens, once, and
     * the I2C bus. */
    CHECK_INT(apply_one("p", "reg/t_perm", "{\"FC_ALLOW\": \"relay1 i2c0\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("p", "Running 0 -2 1 -3 -4", 3000), "Running 0 -2 1 -3 -4");
    CHECK(strstr(stub_kv("apps"), "relay1 i2c0") != NULL);
    /* After a reboot, before the spec comes, it has the same grants. */
    power_cycle();
    CHECK_STR(wait_state_of("p", "Running 0 -2 1 -3 -4", 3000), "Running 0 -2 1 -3 -4");
    remove_all();
}

/* The apps' waits (fc_appapi_wait): how many, and the shortest. */
static int s_ard_sleeps;
static useconds_t s_ard_sleep_min;

static int count_usleep(useconds_t us)
{
    pthread_mutex_lock(&f_lock);
    s_ard_sleeps++;
    s_ard_sleep_min = s_ard_sleep_min == 0 || us < s_ard_sleep_min ? us : s_ard_sleep_min;
    pthread_mutex_unlock(&f_lock);
    return usleep(us);
}

static int ard_sleeps(useconds_t *min)
{
    pthread_mutex_lock(&f_lock);
    int n = s_ard_sleeps;
    *min = s_ard_sleep_min;
    s_ard_sleeps = 0;
    s_ard_sleep_min = 0;
    pthread_mutex_unlock(&f_lock);
    return n;
}

static void test_fc_limits(void)
{
    char err[160];
    /* Stopped at its second, a step of 10 ms at a time: at most 101 waits. */
    useconds_t min;
    fc_appapi_usleep = count_usleep;
    ard_sleeps(&min);
    CHECK_INT(apply_one("s", "reg/t_sleep", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("s", "Error Exception: a call spent more than 1 s in the device's functions", 3000),
              "Error Exception: a call spent more than 1 s in the device's functions");
    int n = ard_sleeps(&min);
    if (n < 1 || n > 101 || min != 10000) {
        fprintf(stderr, "  t_sleep: %d waits, the shortest %u us\n", n, (unsigned)min);
        t_failures++;
    }
    fc_appapi_usleep = usleep;
    CHECK_INT(apply_one("s", "reg/t_oob", NULL, err, sizeof(err)), 0);
    const char *st = wait_state_of("s", "", 0);
    for (int i = 0; i < 300 && strncmp(st, "Error", 5) != 0; i++) {
        usleep(10 * 1000);
        st = state_of("s");
    }
    CHECK(strstr(st, "Error") == st && strstr(st, "out of bounds") != NULL);
    CHECK_INT(apply_one("s", "reg/t_neglen", NULL, err, sizeof(err)), 0);
    for (int i = 0; i < 300 && strncmp(state_of("s"), "Error", 5) != 0; i++) {
        usleep(10 * 1000);
    }
    CHECK(strstr(state_of("s"), "out of bounds") != NULL);
    remove_all();
}

/* A burst of events beyond the queue: the app hears how many were lost.
 * Events for an app that is not running are dropped. */
static void test_fc_events(void)
{
    char err[160];
    CHECK_INT(apply_one("e", "reg/t_events", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("e", "Running ", 3000), "Running ");
    app_t *a = find("e");
    for (int i = 0; i < APP_EVENTS + 8; i++) {
        fc_apps_post(a, EV_CONFIG, -1, 0);
    }
    for (int i = 0; i < 300 && strncmp(state_of("e"), "Running lost ", 13) != 0; i++) {
        usleep(10 * 1000);
    }
    CHECK(strncmp(state_of("e"), "Running lost ", 13) == 0);
    app_stop(a);
    fc_apps_post(a, EV_CONFIG, -1, 0);
    CHECK_INT(a->ev_count, 0);
    remove_all();
    fc_apps_file_changed("/etc/fc-app/e.json"); /* removed: no longer an event target */
    CHECK(find("e") == NULL);
}

/* ---- MQTT: an app against a fake broker on a local TCP port ----------------------- */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static int b_fd = -1;

static int b_read(unsigned char *b, size_t n)
{
    while (n > 0) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(b_fd, &rf);
        struct timeval tv = {.tv_sec = 3};
        if (select(b_fd + 1, &rf, NULL, NULL, &tv) != 1) {
            return -1;
        }
        ssize_t r = read(b_fd, b, n);
        if (r <= 0) {
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* One packet from the device: its first byte (-1 none in 3 s), its body in b. */
static int b_packet(unsigned char *b, size_t cap, size_t *len)
{
    unsigned char first, x;
    size_t rem = 0, mult = 1;
    if (b_read(&first, 1) != 0) {
        return -1;
    }
    do {
        if (b_read(&x, 1) != 0) {
            return -1;
        }
        rem += (x & 0x7fu) * mult;
        mult *= 128;
    } while (x & 0x80);
    if (rem > cap || b_read(b, rem) != 0) {
        return -1;
    }
    *len = rem;
    return first;
}

/* Expects a packet of type `first` whose topic or filter (after a packet id
 * for (UN)SUBSCRIBE) is `topic`; for a PUBLISH, its payload is `payload`. */
static void b_expect(int first, const char *topic, const char *payload)
{
    unsigned char b[512];
    size_t n = 0;
    int t = b_packet(b, sizeof(b), &n);
    CHECK_INT(t, first);
    if (t != first) {
        return;
    }
    size_t at = (first >> 4) == 3 ? 0 : 2;
    size_t tl = (size_t)b[at] << 8 | b[at + 1];
    CHECK(tl == strlen(topic) && memcmp(b + at + 2, topic, tl) == 0);
    if (tl != strlen(topic)) {
        fprintf(stderr, "  topic \"%.*s\"\n", (int)tl, b + at + 2);
    }
    if ((first >> 4) == 8) {
        unsigned char ack[5] = {0x90, 3, b[0], b[1], b[at + 2 + tl]};
        CHECK_INT(write(b_fd, ack, sizeof(ack)), 5);
    }
    if (payload != NULL) {
        size_t off = at + 2 + tl + ((first & 0x06) ? 2 : 0);
        CHECK(n - off == strlen(payload) && memcmp(b + off, payload, n - off) == 0);
        if ((first & 0x06) != 0) {
            unsigned char ack[4] = {0x40, 2, b[at + 2 + tl], b[at + 3 + tl]};
            CHECK_INT(write(b_fd, ack, sizeof(ack)), 4);
        }
    }
}

static void b_publish(const char *topic, const char *payload, int qos)
{
    unsigned char b[300];
    size_t tl = strlen(topic), pl = strlen(payload), n = 0;
    b[n++] = (unsigned char)(0x30 | qos << 1);
    b[n++] = (unsigned char)(2 + tl + (qos ? 2 : 0) + pl);
    b[n++] = 0;
    b[n++] = (unsigned char)tl;
    memcpy(b + n, topic, tl);
    n += tl;
    if (qos) {
        b[n++] = 0;
        b[n++] = 9;
    }
    memcpy(b + n, payload, pl);
    n += pl;
    CHECK_INT(write(b_fd, b, n), (long)n);
    if (qos) {
        unsigned char ack[4];
        size_t len;
        CHECK_INT(b_packet(ack, sizeof(ack), &len), 0x40);
    }
}

/* The agent: idle, and held from a claim to the end of the handshake. */
static int s_mqtt_claims, s_mqtt_ends;
static int mqtt_idle(int claim)
{
    s_mqtt_claims += claim;
    return 1;
}
static void mqtt_connected(void) { s_mqtt_ends++; }

/* The broker's handshake waits for room above the agent's floor, read with
 * fc_port_heap_free_now: also where the port keeps the apps' own heap
 * checks off (fc_port_heap_free -1), as Zephyr does. */
static void test_mqtt_waits_for_heap(void)
{
    fc_apps_mqtt_hooks(mqtt_idle, mqtt_connected);
    int claims = s_mqtt_claims;
    stub_heap_free = -1;
    stub_heap_free_now = s_heap_floor + FC_MQTT_CONN_COST - 1;
    CHECK_INT(mqtt_may_connect(), 0);
    CHECK_INT(s_mqtt_claims, claims); /* the agent not held */
    stub_heap_free_now = s_heap_floor + FC_MQTT_CONN_COST;
    CHECK_INT(mqtt_may_connect(), 1);
    CHECK_INT(s_mqtt_claims, claims + 1);
    mqtt_connect_end();
    stub_heap_free_now = -1; /* a port that cannot tell: not held back */
    CHECK_INT(mqtt_may_connect(), 1);
    mqtt_connect_end();
    stub_heap_free_now = -2;
    s_mqtt_claims = s_mqtt_ends = 0; /* test_fc_mqtt counts its own */
}

static void test_fc_mqtt(void)
{
    char err[160];
    /* The broker: a local port. */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t sl = sizeof(sa);
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(ls, 2) == 0 &&
          getsockname(ls, (struct sockaddr *)&sa, &sl) == 0);
    char json[200];
    snprintf(json, sizeof(json), "{\"broker\": \"mqtt://127.0.0.1:%d\", \"fleetPrefix\": \"fleet/f/{app}/\"}",
             ntohs(sa.sin_port));
    fc_port_write_file(FC_MQTT_CONFIG, json, strlen(json));
    fc_apps_mqtt_hooks(mqtt_idle, mqtt_connected);
    CHECK_INT(fc_mqtt_configure("dev1"), 1);
    b_fd = accept(ls, NULL, NULL);
    CHECK(b_fd >= 0);
    unsigned char pk[512];
    size_t n;
    CHECK_INT(b_packet(pk, sizeof(pk), &n), 0x10); /* CONNECT */
    unsigned char connack[4] = {0x20, 2, 0, 0};
    CHECK_INT(write(b_fd, connack, sizeof(connack)), 4);
    for (int i = 0; i < 300; i++) {
        fc_mqtt_stats_t st;
        fc_mqtt_stats(&st);
        if (st.connected) {
            break;
        }
        usleep(10 * 1000);
    }
    CHECK_INT(s_mqtt_claims, 1); /* the agent held for the handshake, */
    CHECK_INT(s_mqtt_ends, 1);   /* and let go before the link was up */

    /* Not granted: FC_EPERM. */
    CHECK_INT(apply_one("n", "reg/t_mqtt", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("n", "Running nosub -3", 3000), "Running nosub -3");
    remove_all();

    /* Granted: its subscriptions on the wire, FC_EV_NET at once (the link is
     * up), and its first message. */
    CHECK_INT(apply_one("m", "reg/t_mqtt", "{\"FC_ALLOW\": \"mqtt\"}", err, sizeof(err)), 0);
    b_expect(0x82, "flightctl/dev1/m/in/#", NULL);
    b_expect(0x82, "fleet/f/m/cmd", NULL);
    b_expect(0x30, "flightctl/dev1/m/up", "hi");
    CHECK_STR(wait_state_of("m", "Running net 1 0", 3000), "Running net 1 0");

    /* A message: its topic relative, its payload, its length; a buffer too
     * small is FC_ENOSPC; the app's answer. */
    b_publish("flightctl/dev1/m/in/x", "hello", 1);
    b_expect(0x32, "flightctl/dev1/m/ack", "in/x hello");
    CHECK_STR(wait_state_of("m", "Running msg in/x hello 5 -7", 3000), "Running msg in/x hello 5 -7");
    /* On the fleet's prefix: "close" ends its first subscription. */
    b_publish("fleet/f/m/cmd", "close", 0);
    b_expect(0xa2, "flightctl/dev1/m/in/#", NULL);
    b_expect(0x32, "flightctl/dev1/m/ack", "cmd close");
    CHECK_STR(wait_state_of("m", "Running msg cmd close 5 -7", 3000), "Running msg cmd close 5 -7");
    b_publish("flightctl/dev1/m/in/y", "gone", 0); /* no subscription now: not delivered */
    usleep(100 * 1000);
    CHECK_STR(state_of("m"), "Running msg cmd close 5 -7");

    fc_mqtt_stats_t st;
    fc_mqtt_stats(&st);
    CHECK_INT(st.pool_used, 0); /* each message back in the pool once handled */

    /* The link lost: FC_EV_NET 0. */
    close(b_fd);
    CHECK_STR(wait_state_of("m", "Running net 0", 5000), "Running net 0");
    /* Back: subscribed again, told again. */
    b_fd = accept(ls, NULL, NULL);
    CHECK_INT(b_packet(pk, sizeof(pk), &n), 0x10);
    CHECK_INT(write(b_fd, connack, sizeof(connack)), 4);
    b_expect(0x82, "fleet/f/m/cmd", NULL);
    b_expect(0x30, "flightctl/dev1/m/up", "hi");
    CHECK_STR(wait_state_of("m", "Running net 1 0", 3000), "Running net 1 0");

    /* Removed: its subscription goes with it. */
    b_publish("fleet/f/m/cmd", "x", 0);
    b_expect(0x32, "flightctl/dev1/m/ack", "cmd x");
    remove_all();
    b_expect(0xa2, "fleet/f/m/cmd", NULL);
    fc_mqtt_stats(&st);
    CHECK_INT(st.pool_used, 0);

    fc_port_remove_file(FC_MQTT_CONFIG);
    CHECK_INT(fc_mqtt_configure("dev1"), 0);
    close(b_fd);
    close(ls);
}

static void test_board_map(void)
{
    char err[160];
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\": {\"x\": {\"pin\": 11}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "pin 11 is used by the firmware") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\": {\"x\": {\"pin\": 8}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "led0 and x are both on pin 8") != NULL || strstr(err, "x and led0") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"sensors\": {}}", err, sizeof(err)), -1);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\": {\"mqtt\": {\"pin\": 3}}}", err, sizeof(err)), -1);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"led\": {\"l\": {\"pin\": 3, \"order\": \"rgx\"}}}", err,
                                 sizeof(err)),
              -1);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"i2c\": {\"b\": {\"sda\": 6}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "b: an I2C bus needs sda and scl") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"i2c\": {\"b\": {\"sda\": 6, \"scl\": 5}}}", err,
                                 sizeof(err)),
              -1);
    CHECK(strstr(err, "relay1 and b are both on pin 5") != NULL || strstr(err, "b and relay1") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"i2c\": {\"b\": {\"sda\": 6, \"scl\": 6}}}", err,
                                 sizeof(err)),
              -1);
    CHECK(strstr(err, "b uses pin 6 twice") != NULL);
    /* SPI devices share a bus's lines, not a chip select; a bus is all three
     * lines or none. */
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH,
                                 "{\"spi\": {\"x\": {\"sck\": 20, \"mosi\": 21, \"miso\": 22, \"cs\": 24}}}", err,
                                 sizeof(err)),
              -1);
    CHECK(strstr(err, "on pin 24") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH,
                                 "{\"spi\": {\"x\": {\"sck\": 20, \"mosi\": 30, \"cs\": 31}}}", err, sizeof(err)),
              -1);
    CHECK(strstr(err, "on pin 20") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH,
                                 "{\"spi\": {\"x\": {\"sck\": 20, \"mosi\": 21, \"miso\": 22, \"cs\": 31}}}", err,
                                 sizeof(err)),
              0);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"spi\": {\"x\": {\"sck\": 30, \"mosi\": 31}}}", err, sizeof(err)),
              -1);
    CHECK(strstr(err, "x: an SPI device needs sck, cs, and mosi or miso") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"uart\": {\"x\": {\"de\": 30}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "x: a UART needs tx or rx") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"spi\": {\"x\": {\"sck\": 30, \"mosi\": 31, \"cs\": 32, \"mode\": 4}}}",
                                 err, sizeof(err)),
              -1);
    /* The firmware's bus at a new speed keeps its pins. */
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"i2c\": {\"i2c0\": {\"hz\": 400000}}}", err, sizeof(err)), 0);
    CHECK_INT(fc_apps_check_file("/etc/other.json", "not json", err, sizeof(err)), 0);

    /* relay1 moves to pin 6: the app restarts and drives it there. */
    CHECK_INT(apply_one("g", "reg/t_gpio", "{\"FC_ALLOW\": \"relay1 button0 led0\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("g", "Running t 41000 0", 3000), "Running t 41000 0");
    const char *map = "{\"gpio\": {\"relay1\": {\"pin\": 6, \"safe\": 1}}}";
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, map, err, sizeof(err)), 0);
    fc_port_write_file(FC_BOARD_PATH, map, strlen(map));
    f_level[6] = 0;
    fc_apps_file_changed(FC_BOARD_PATH);
    CHECK_STR(wait_state_of("g", "Running t 41000 0", 3000), "Running t 41000 0");
    for (int i = 0; i < 300 && f_read(6) != 1; i++) {
        usleep(10 * 1000);
    }
    CHECK_INT(f_read(6), 1);
    CHECK_INT(f_read(5), 0); /* the old pin went to its safe level */
    remove_all();
    CHECK_INT(f_read(6), 1); /* safe: 1 */
    /* pin -1 keeps the firmware's pin: a template changes the colour order
     * without knowing it. */
    map = "{\"led\": {\"led0\": {\"pin\": -1, \"order\": \"rgb\"}}}";
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, map, err, sizeof(err)), 0);
    fc_port_write_file(FC_BOARD_PATH, map, strlen(map));
    fc_apps_file_changed(FC_BOARD_PATH);
    fc_board_entry_t *led = fc_board_find("led0", 4);
    CHECK(led != NULL && led->led.pin == 8 && strcmp(led->led.order, "rgb") == 0);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"led\": {\"new\": {\"pin\": -1}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "new: no pin") != NULL);
    fc_port_write_file(FC_BOARD_PATH, "{}", 2);
    fc_apps_file_changed(FC_BOARD_PATH);
}

/* A port that names its hardware in a devicetree (Zephyr): the ids happen
 * to be the fake board's pins, so the GPIO app drives the same ones. */
static int f_dt_find(int kind, const char *name)
{
    static const struct {
        int kind;
        const char *name;
        int id;
    } nodes[] = {{FC_HW_KIND_GPIO, "relay_node", 5}, {FC_HW_KIND_GPIO, "button_node", 9},
                 {FC_HW_KIND_GPIO, "spare_node", 6}, {FC_HW_KIND_LED, "led_node", 8}};
    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        if (nodes[i].kind == kind && strcmp(nodes[i].name, name) == 0) {
            return nodes[i].id;
        }
    }
    return -1;
}
static char *f_dt_defaults(void)
{
    return strdup("{\"gpio\": {\"relay1\": {\"dt\": \"relay_node\", \"safe\": 0},"
                  " \"button0\": {\"dt\": \"button_node\", \"active_low\": true}, "
                  "\"spare_node\": {\"dt\": \"spare_node\", \"active_low\": true, \"safe\": 0}},"
                  " \"led\": {\"led0\": {\"dt\": \"led_node\"}}}");
}

static void test_board_dt(void)
{
    char err[160];
    s_fake.dt_find = f_dt_find;
    s_fake.board_defaults = f_dt_defaults;
    fc_port_write_file(FC_BOARD_PATH, "{}", 2);
    fc_apps_file_changed(FC_BOARD_PATH);
    /* Pins mean nothing here, and nodes must exist, once each. */
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\": {\"x\": {\"pin\": 6}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "x: this board names its hardware in its devicetree") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\": {\"x\": {\"dt\": \"nope\"}}}", err, sizeof(err)), -1);
    CHECK(strstr(err, "x: the devicetree has no gpio node \"nope\"") != NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"adc\": {\"x\": {\"dt\": \"spare_node\"}}}", err, sizeof(err)),
              -1); /* a GPIO node is not an ADC channel */
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\": {\"x\": {\"dt\": \"relay_node\"}}}", err, sizeof(err)),
              -1);
    CHECK(strstr(err, "both the devicetree's relay_node") != NULL);
    /* A real Zephyr default uses the node's own name. Only that implicit
     * name can be replaced; explicit duplicate grants are still rejected. */
    const char *alias = "{\"gpio\":{\"pump\":{\"dt\":\"spare_node\"}}}";
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, alias, err, sizeof(err)), 0);
    CHECK_INT(fc_port_write_file(FC_BOARD_PATH, alias, strlen(alias)), 0);
    fc_apps_file_changed(FC_BOARD_PATH);
    fc_board_entry_t *pump = fc_board_find("pump", 4);
    CHECK(pump && pump->dt_id == 6 && pump->active_low && pump->safe == 0);
    CHECK(fc_board_find("spare_node", 10) == NULL);
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, "{\"gpio\":{\"x\":{\"dt\":\"spare_node\"},"
                                "\"y\":{\"dt\":\"spare_node\"}}}", err, sizeof(err)), -1);
    /* The GPIO app on the nodes: relay1 is node 5, button0 node 9. */
    f_level[9] = 1;
    CHECK_INT(apply_one("g", "reg/t_gpio", "{\"FC_ALLOW\": \"relay1 button0 led0\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("g", "Running t 41000 0", 3000), "Running t 41000 0");
    CHECK_INT(f_read(5), 1);
    CHECK_INT(f_mode[9], FC_HW_GPIO_IN_PULLUP);
    /* An alias: the fleet moves relay1 to another node. */
    const char *map = "{\"gpio\": {\"relay1\": {\"dt\": \"spare_node\", \"active_low\": false, \"safe\": 0}}}";
    CHECK_INT(fc_apps_check_file(FC_BOARD_PATH, map, err, sizeof(err)), 0);
    fc_port_write_file(FC_BOARD_PATH, map, strlen(map));
    fc_apps_file_changed(FC_BOARD_PATH);
    for (int i = 0; i < 300 && f_read(6) != 1; i++) {
        usleep(10 * 1000);
    }
    CHECK_INT(f_read(6), 1);
    fc_board_entry_t *e = fc_board_find("relay1", 6);
    CHECK(e != NULL && e->pin == -1 && e->dt_id == 6 && fc_board_pin(e) == 6);
    remove_all();
    s_fake.dt_find = NULL;
    s_fake.board_defaults = f_defaults;
    fc_port_write_file(FC_BOARD_PATH, "{}", 2);
    fc_apps_file_changed(FC_BOARD_PATH);
}

static void test_example_apps(void)
{
    char err[160];
    const char *cfg = "{\"threshold_c\": 40.5, \"sample_ms\": 100}";
    fc_port_write_file("/etc/fc-app/alarm.json", cfg, strlen(cfg));
    __atomic_store_n(&f_temp, 41000, __ATOMIC_RELEASE);
    const char *apps[] = {"alarm", "reg/app_alarm_v2", "{\"FC_ALLOW\": \"led0\"}", "trend", "reg/app_trend", NULL,
                          NULL};
    CHECK_INT(apply_apps(apps, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("alarm", "Running v2 alarm (blinking blue)", 3000), "Running v2 alarm (blinking blue)");
    CHECK_STR(wait_state_of("trend", "Running steady (41.0 C)", 3000), "Running steady (41.0 C)");
    int before = f_led_writes;
    usleep(600 * 1000);
    CHECK(f_led_writes - before >= 2); /* blinking */
    __atomic_store_n(&f_temp, 39000, __ATOMIC_RELEASE); /* within 2 C of the threshold */
    CHECK_STR(wait_state_of("alarm", "Running v2 pre-alarm (dim blue)", 3000), "Running v2 pre-alarm (dim blue)");
    CHECK(f_led[2] == 8);
    __atomic_store_n(&f_temp, 30000, __ATOMIC_RELEASE);
    CHECK_STR(wait_state_of("alarm", "Running v2 ok", 3000), "Running v2 ok");
    CHECK(f_led[2] == 0);
    remove_all();
    /* hwcheck: the fake board has an I2C bus with one device, and none of
     * the other names it looks for. */
    CHECK_INT(apply_one("hw", "reg/app_hwcheck", "{\"FC_ALLOW\": \"i2c0 spi0 uart0 adc0 pwm0\", \"TICK_MS\": \"100\"}",
                        err, sizeof(err)),
              0);
    const char *hw = "Running i2c 1 spi -2 uart -2 adc -2 pwm -2";
    CHECK_STR(wait_state_of("hw", hw, 3000), hw);
    remove_all();
    /* mqttdemo, with no broker configured: offline until one comes. */
    CHECK_INT(apply_one("md", "reg/app_mqttdemo", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("md", "Running needs mqtt in FC_ALLOW", 3000), "Running needs mqtt in FC_ALLOW");
    remove_all();
    CHECK_INT(apply_one("md", "reg/app_mqttdemo", "{\"FC_ALLOW\": \"mqtt led0\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("md", "Running offline", 3000), "Running offline");
    remove_all();
}

/* tinyml: emlearn's model, as WebAssembly. A temperature that does not move
 * is steady; one that moves by the same step at each read (0.5 C, 6 C a
 * minute at the model's 5 s) is warming or cooling, whenever the app reads
 * it. */
static void tinyml_with_step(int32_t step, const char *want)
{
    char err[160];
    __atomic_store_n(&f_temp, 41000, __ATOMIC_RELEASE);
    __atomic_store_n(&f_temp_step, step, __ATOMIC_RELEASE);
    CHECK_INT(apply_one("ml", "reg/app_tinyml", NULL, err, sizeof(err)), 0);
    for (int i = 0; i < 3000 && strncmp(state_of("ml"), want, strlen(want)) != 0; i++) {
        usleep(10 * 1000);
    }
    CHECK_CONTAINS(state_of("ml"), want);
    remove_all();
    __atomic_store_n(&f_temp_step, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&f_temp, 41000, __ATOMIC_RELEASE);
}

static void test_tinyml(void)
{
    const char *cfg = "{\"sample_ms\": 20}";
    fc_port_write_file("/etc/fc-app/ml.json", cfg, strlen(cfg));
    /* The window fills, one sample at a time, with the same temperature:
     * the reads fail after the sixth (init's two, then four samples), so
     * the state stays at 5 of 12. */
    char err[160];
    __atomic_store_n(&f_temp, 41000, __ATOMIC_RELEASE);
    __atomic_store_n(&f_temp_reads, 6, __ATOMIC_RELEASE);
    CHECK_INT(apply_one("ml", "reg/app_tinyml", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("ml", "Running sampling 5/12 (41.0 C)", 30000), "Running sampling 5/12 (41.0 C)");
    remove_all();
    __atomic_store_n(&f_temp_reads, -1, __ATOMIC_RELEASE);
    tinyml_with_step(0, "Running steady (41.0 C)");
    tinyml_with_step(500, "Running warming (");
    tinyml_with_step(-500, "Running cooling (");
    fc_port_remove_file("/etc/fc-app/ml.json");
}

/* tinymlp: emlearn's network, with the device's expf, logf and tanhf. The
 * temperature as for tinyml; the state has the class and how sure the
 * network is of it. */
static void test_tinymlp(void)
{
    static const struct {
        int32_t step;
        const char *want;
    } runs[] = {{0, "Running steady "}, {500, "Running warming "}, {-500, "Running cooling "}};
    const char *cfg = "{\"sample_ms\": 20}";
    fc_port_write_file("/etc/fc-app/mlp.json", cfg, strlen(cfg));
    for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
        char err[160];
        __atomic_store_n(&f_temp, 41000, __ATOMIC_RELEASE);
        __atomic_store_n(&f_temp_step, runs[r].step, __ATOMIC_RELEASE);
        CHECK_INT(apply_one("mlp", "reg/app_tinymlp", NULL, err, sizeof(err)), 0);
        const char *want = runs[r].want;
        for (int i = 0; i < 3000 && strncmp(state_of("mlp"), want, strlen(want)) != 0; i++) {
            usleep(10 * 1000);
        }
        const char *st = state_of("mlp");
        CHECK_CONTAINS(st, want);
        int pct = -1;
        CHECK(sscanf(st + strlen(want), "%d%%", &pct) == 1 && pct > 25 && pct <= 100);
        if (r == 0) {
            CHECK_CONTAINS(st, "% (41.0 C)");
        }
        remove_all();
    }
    __atomic_store_n(&f_temp_step, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&f_temp, 41000, __ATOMIC_RELEASE);
    fc_port_remove_file("/etc/fc-app/mlp.json");
}


static int gpio_level(int pin)
{
    pthread_mutex_lock(&f_lock);
    int v = f_level[pin];
    pthread_mutex_unlock(&f_lock);
    return v;
}

/* The module "arduino" (fc_arduino.c): pins are FC_ALLOW's names (relay1 is
 * GPIO 5, led0 an LED), a delay longer than one of fc's waits, the greeting,
 * print to the log, a pin not granted. */
static void test_arduino(void)
{
    char err[160];
    const char *env = "{\"FC_ENTRY\": \"run\", \"FC_ALLOW\": \"relay1 led0\"}";
    stub_log_clear();
    CHECK_INT(apply_one("a", "reg/t_arduino", env, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("a", "Completed run() returned 63", 30000), "Completed run() returned 63");
    CHECK(stub_log_has("app a: greeting: Hello from Flight Control"));
    CHECK_INT(stub_log_count("app a: arduino pin 7: no such name in FC_ALLOW (-3)"), 1);
    CHECK_INT(stub_log_count("app a: arduino pin 16: no such name in FC_ALLOW (-3)"), 1);
    CHECK_INT(stub_log_count("app a: arduino pin 0: written before pinMode(OUTPUT) (-1)"), 1);
    CHECK(!stub_log_has("app a: arduino pin 7: written before pinMode(OUTPUT) (-1)"));
    CHECK(!stub_log_has("app a: arduino pin 17: written before pinMode(OUTPUT) (-1)"));
    wait_idle("a"); /* The runner closes its handles after reporting Completed. */
    CHECK_INT(gpio_level(5), 0); /* closed when run() returned: relay1's safe level */
    CHECK(!f_led_open);
    remove_all();
    /* Waiting in delay() with relay1 high: removing the app stops it there,
     * and relay1 goes back to its safe level. */
    env = "{\"FC_ENTRY\": \"hold\", \"FC_ALLOW\": \"relay1\"}";
    CHECK_INT(apply_one("a", "reg/t_arduino", env, err, sizeof(err)), 0);
    for (int i = 0; i < 3000 && gpio_level(5) != 1; i++) {
        usleep(10 * 1000);
    }
    CHECK_INT(gpio_level(5), 1);
    int64_t t0 = fc_apps_now_ms();
    remove_all();
    CHECK(fc_apps_now_ms() - t0 < STOP_MS);
    CHECK_INT(gpio_level(5), 0);
    /* delay() waits a tick at a time, never less, so that the agent's
     * threads run on a chip with one core (ESP-IDF's usleep spins below a
     * tick): delay(0) and delay(1) once each, delay(105) 11 times. */
    useconds_t min;
    fc_appapi_usleep = count_usleep;
    ard_sleeps(&min);
    CHECK_INT(apply_one("a", "reg/t_arduino", "{\"FC_ENTRY\": \"delays\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("a", "Completed delays() returned", 30000), "Completed delays() returned");
    CHECK_INT(ard_sleeps(&min), 13);
    CHECK_INT((int)min, 10000);
    remove_all();
    /* In an app with tick(), a delay counts against the call's second in the
     * device's functions: 150 ms is fine, 1500 stops the app, at its second:
     * 15 waits for each of the first three, at most 101 for the fourth. */
    CHECK_INT(apply_one("a", "reg/t_arduinotick", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("a", "Running waited 3", 30000), "Running waited 3");
    const char *want = "Error Exception: a call spent more than 1 s in the device's functions";
    CHECK_STR(wait_state_of("a", want, 30000), want);
    int n = ard_sleeps(&min);
    if (n < 45 + 1 || n > 45 + 101) {
        fprintf(stderr, "  t_arduinotick: %d waits in delay()\n", n);
        t_failures++;
    }
    remove_all();
    fc_appapi_usleep = usleep;
}

/* The blink of wasm3-arduino, its four builds as published: each lights led0
 * and turns it off again and again, until it is removed. */
static void test_arduino_blink(void)
{
    static const struct {
        const char *image, *line;
    } builds[] = {
        {"reg/arduino_rust", "app b: Rust is running \xf0\x9f\x98\x8e"},
        {"reg/arduino_cpp", "app b: Greeting: Hello from Flight Control"},
        {"reg/arduino_assemblyscript", "app b: Greeting: Hello from Flight Control"},
        {"reg/arduino_tinygo", "app b: Greeting: Hello from Flight Control"},
    };
    for (size_t i = 0; i < sizeof(builds) / sizeof(builds[0]); i++) {
        char err[160];
        pthread_mutex_lock(&f_lock);
        f_led_on_writes = f_led_off_writes = 0;
        pthread_mutex_unlock(&f_lock);
        CHECK_INT(apply_one("b", builds[i].image, "{\"FC_ALLOW\": \"led0\"}", err, sizeof(err)), 0);
        int on = 0, off = 0;
        for (int j = 0; j < 3000 && (on < 2 || off < 2); j++) {
            usleep(10 * 1000);
            pthread_mutex_lock(&f_lock);
            on = f_led_on_writes;
            off = f_led_off_writes;
            pthread_mutex_unlock(&f_lock);
        }
        if (on < 2 || off < 2) {
            fprintf(stderr, "  %s: led0 on %d times, off %d (%s)\n", builds[i].image, on, off, state_of("b"));
            t_failures++;
        }
        CHECK_STR(state_of("b"), "Running ");
        if (!stub_log_has(builds[i].line)) {
            fprintf(stderr, "  %s: no log line \"%s\"\n", builds[i].image, builds[i].line);
            t_failures++;
        }
        int64_t t0 = fc_apps_now_ms();
        remove_all();
        CHECK(fc_apps_now_ms() - t0 < STOP_MS);
        CHECK(!f_led_open);
    }
}

/* A restart: a crash (1) or not (0). */
static void restart(int crash)
{
    f_crash = crash;
    power_cycle();
    f_crash = 0;
}

static int bad(const char *name, const char *image, const char *env)
{
    return guard_is_bad(name, image, env);
}

/* Saved apps held at boot start once a status is accepted, or after a
 * while without one: not with the heap of before the network. */
static void test_saved_apps_held(void)
{
    char err[160];
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    fc_apps_hold_saved(1);
    power_cycle();
    CHECK(strncmp(state_of("app"), "Preparing", 9) == 0);
    CHECK(fc_apps_saved_delay_s() != 0);
    fc_apps_status_ok();
    CHECK_INT(fc_apps_saved_delay_s(), 0);
    CHECK_INT(fc_apps_start_saved(), 1);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    CHECK(fc_apps_saved_delay_s() != 0); /* once */
    /* No status at all (offline): after BOOT_APPS_WAIT_MS. */
    s_status_oks = 0;
    fc_apps_hold_saved(1);
    power_cycle();
    CHECK(fc_apps_saved_delay_s() != 0);
    s_init_ms -= BOOT_APPS_WAIT_MS;
    CHECK_INT(fc_apps_saved_delay_s(), 0);
    CHECK_INT(fc_apps_start_saved(), 1);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    remove_all();
}

/* A restart for want of an accepted status, with the heap below the floor,
 * counts against the app in its start as a crash does; with heap, not. */
static void test_guard_silent_restart(void)
{
    char err[200];
    s_status_oks = 1; /* the last accepted status came before it started */
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    stub_heap_free = 100 * 1024;
    fc_apps_note_silent_restart(); /* heap enough: not blamed */
    CHECK(stub_kv(KV_SILENT) == NULL);
    stub_heap_free = 8 * 1024;
    fc_apps_note_silent_restart();
    stub_heap_free = -1;
    CHECK(stub_kv(KV_SILENT) != NULL);
    restart(0);
    CHECK(stub_kv(KV_SILENT) == NULL);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1"); /* once could be anything */
    stub_heap_free = 8 * 1024;
    fc_apps_note_silent_restart();
    stub_heap_free = -1;
    restart(0);
    CHECK_STR(state_of("app"), "Error " GUARD_ERROR);
    s_status_oks = 0;
    remove_all();
    CHECK(stub_kv(KV_GUARD) == NULL);
}

static void test_guard_crash_at_start(void)
{
    /* An app the device crashes on as it starts, at every boot: after two
     * crashes it is not started, and the device goes on to the server. */
    char err[200];
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    restart(1);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1"); /* once could be anything */
    restart(1);
    CHECK_STR(state_of("app"), "Error " GUARD_ERROR);
    CHECK(find("app")->module == NULL);
    /* The same spec neither downloads nor loads it again. */
    f_fetches = 0;
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), -1);
    CHECK(strstr(err, "crashed the device") != NULL);
    CHECK_INT(f_fetches, 0);
    CHECK_STR(state_of("app"), "Error " GUARD_ERROR);
    /* Still so after a restart that was not a crash. */
    restart(0);
    CHECK_STR(state_of("app"), "Error " GUARD_ERROR);
    /* Other env vars: tried again. */
    CHECK_INT(apply_one("app", "reg/app:v1", "{\"TICK_MS\": \"200\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    CHECK(!bad("app", "reg/app:v1", "{}"));
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    remove_all();
    CHECK(stub_kv(KV_GUARD) == NULL);
}

static void test_guard_other_restarts(void)
{
    char err[200];
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    /* Restarts that were not crashes clear the count. */
    restart(1);
    restart(0);
    restart(1);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    /* An app past its first minute is not blamed for a later crash. */
    s_settle_ms = 50;
    restart(0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    for (int i = 0; i < 100 && stub_kv(KV_GUARD) != NULL; i++) {
        usleep(10 * 1000);
    }
    CHECK(stub_kv(KV_GUARD) == NULL); /* settled on its own thread */
    restart(1);
    s_settle_ms = GUARD_SETTLE_MS;
    restart(1);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    remove_all();
}

static void test_guard_blames_the_module_loading(void)
{
    /* A spec brings a second app whose module crashes the device as it
     * loads, beside one that runs: only the second one is stopped. */
    char err[200];
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    for (int i = 0; i < 2; i++) {
        guard_begin("new", "reg/new:v1", "{}", 1); /* as fc_apps_apply does before the download */
        restart(1);
        CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    }
    CHECK(bad("new", "reg/new:v1", "{}"));
    CHECK(!bad("app", "reg/app:v1", "{}"));
    const char *apps[] = {"app", "reg/app:v1", NULL, "new", "reg/new:v1", NULL, NULL};
    f_fetches = 0;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), -1);
    CHECK_INT(f_fetches, 0);
    CHECK_STR(state_of("new"), "Error " GUARD_ERROR);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    /* Out of the spec, then back: tried again. */
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK(!bad("new", "reg/new:v1", "{}"));
    remove_all();
}

static void test_guard_new_firmware(void)
{
    char err[200];
    CHECK_INT(apply_one("app", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    restart(1);
    restart(1);
    CHECK_STR(state_of("app"), "Error " GUARD_ERROR);
    /* Another firmware tries it again. */
    s_fake.firmware = "test2";
    restart(0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    remove_all();
    CHECK(stub_kv(KV_GUARD) == NULL);
    s_fake.firmware = "test";
}

/* A module loads with the heap's largest block held, and installs as
 * before (fetch_module). */
static void test_load_with_the_largest_block_held(void)
{
    char err[160];
    int retry;
    remove_all();
    stub_heap_largest = 256 * 1024;
    CHECK_INT(apply("reg/app:v1", err, sizeof(err), &retry), 0);
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    stub_heap_largest = -1;
}

/* The app's thread is made with the largest block held, its stack in
 * another block; when no other block has room for it, the hold leaves it
 * room at the block's end, so that the first pthread_create does not fail
 * (ESP-IDF logged it as an error before the second try, without the hold). */
static void test_hold_leaves_room_for_the_stack(void)
{
    const long big = THREAD_STACK + 256 * 1024; /* a host's thread has 256 KB */
    /* Another block has room: the whole largest block is held. */
    stub_heap_largest_next[0] = big;
    stub_heap_largest_next[1] = THREAD_STACK + THREAD_SLACK;
    stub_heap_largest_queued = 2;
    void *hold = hold_but_stack();
    CHECK(hold != NULL);
    CHECK((long)usable_size(hold) >= big - 1024);
    fc_heap_unhold(hold);
    /* None has: the hold leaves room for the stack and what comes with it. */
    stub_heap_largest_next[0] = big;
    stub_heap_largest_next[1] = 4096;
    stub_heap_largest_next[2] = big;
    stub_heap_largest_queued = 3;
    hold = hold_but_stack();
    CHECK(hold != NULL);
    CHECK((long)usable_size(hold) <= big - THREAD_STACK - THREAD_SLACK);
    CHECK((long)usable_size(hold) >= big - THREAD_STACK - THREAD_SLACK - 1024);
    fc_heap_unhold(hold);
    CHECK_INT(stub_heap_largest_queued, 0);
    /* No block worth holding, or a port that cannot tell: no hold. */
    stub_heap_largest_next[0] = 16 * 1024;
    stub_heap_largest_queued = 1;
    CHECK(hold_but_stack() == NULL);
    CHECK(hold_but_stack() == NULL); /* stub_heap_largest: -1 */
}

/* The heap's low over a stretch leaves out what a hold took meanwhile: the
 * heap's own low fell by it, the agent did not use it. */
static void test_heap_low_holds(void)
{
    stub_heap_watch_low = 50 * 1024;
    int k = fc_heap_low_begin();
    CHECK(k >= 0);
    stub_heap_largest_next[0] = 100 * 1024;
    stub_heap_largest_queued = 1;
    void *hold = fc_heap_hold(24 * 1024); /* folds the stretch so far: 50 KB, nothing held */
    CHECK(hold != NULL);
    long n = 100 * 1024 - 24 * 1024 - 64;
    stub_heap_watch_low = 5 * 1024; /* the handshake, with the hold */
    long raw, low = fc_heap_low_peek(k, &raw);
    CHECK_INT(low, 50 * 1024);
    CHECK_INT(raw, 5 * 1024);
    fc_heap_unhold(hold); /* 5 KB + n: above 50 KB */
    stub_heap_watch_low = 45 * 1024; /* after it */
    low = fc_heap_low_end(k, &raw);
    CHECK_INT(low, 45 * 1024);
    CHECK_INT(raw, 5 * 1024);
    CHECK(n + 5 * 1024 > 50 * 1024);
    CHECK_INT(fc_heap_low_end(k, NULL), -1); /* closed */
    /* A port that cannot watch: no stretch. */
    stub_heap_watch_low = -1;
    CHECK_INT(fc_heap_low_begin(), -1);
}

/* A decoded module gives unused SRAM back without giving up the block
 * for its linear memory. Meter the smaller reservation from that point. */
static void test_trim_heap_hold(void)
{
    stub_heap_largest_next[0] = 160 * 1024;
    stub_heap_largest_queued = 1;
    void *hold = fc_heap_hold(0);
    CHECK(hold != NULL);
    stub_heap_watch_low = 180 * 1024;
    int k = fc_heap_low_begin();
    CHECK(k >= 0);
    CHECK(fc_heap_trim_hold(NULL, 64 * 1024) == NULL);
    CHECK(fc_heap_trim_hold(hold, 0) == hold);
    CHECK(fc_heap_trim_hold(hold, 256 * 1024) == hold);
    hold = fc_heap_trim_hold(hold, 128 * 1024);
    CHECK(hold != NULL);
    stub_heap_watch_low = 12 * 1024;
    CHECK_INT(fc_heap_low_peek(k, NULL), 140 * 1024);
    hold = fc_heap_trim_hold(hold, 64 * 1024);
    CHECK_INT(fc_heap_low_peek(k, NULL), 76 * 1024);
    fc_heap_unhold(hold);
    CHECK_INT(fc_heap_low_end(k, NULL), 12 * 1024);
    stub_heap_watch_low = -1;
}

/* An app whose memory would leave the agent less heap than it needs for a
 * connection and a spec is refused, tried once more, and not started again
 * until its image or env vars change. */
static void test_heap_floor(void)
{
    char err[160];
    stub_log_clear();
    stub_heap_free = 64 * 1024;
    stub_heap_watch_low = 40 * 1024;
    CHECK_INT(apply_one("h", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("h", "Running v1", 3000), "Running v1");
    CHECK(stub_log_has("app h: instance made, 64 KB of heap left for the agent"));
    /* What the agent took after it, once the agent has been idle a while. */
    const char *note = "app h (install): 64 KB of heap free after its instance, 40 KB at the lowest while the agent "
                       "went on (it took 24 KB; 40 KB at the heap's own lowest; floor 40 KB)";
    for (int i = 0; i < 400 && !stub_log_has(note); i++) {
        usleep(10 * 1000);
    }
    CHECK(stub_log_has(note));
    stub_heap_watch_low = -1;
    remove_all();

    stub_heap_free = 8 * 1024;
    stub_heap_free_next[0] = 1024 * 1024; /* when it is downloaded (wasm_limit): room enough */
    stub_heap_free_queued = 1;
    CHECK_INT(apply_one("h", "reg/app:v1", NULL, err, sizeof(err)), 0);
    const char *want = "Error its memory would leave the agent 8 KB of heap, less than the 40 KB it needs";
    CHECK_STR(wait_state_of("h", want, 3000), want);
    /* Not started again, though it would now fit. */
    stub_heap_free = -1;
    app_t *h = find("h");
    while (!__atomic_load_n(&h->runner_done, __ATOMIC_ACQUIRE)) {
        usleep(1000); /* once the runner has exited it cannot restart itself */
    }
    CHECK_STR(state_of("h"), want);
    pthread_mutex_lock(&s_lock);
    CHECK_INT(find("h")->restarts, 0);
    pthread_mutex_unlock(&s_lock);
    /* Started again once the heap has room for it (fc_apps_poll), but not
     * before its next look. */
    CHECK_INT(h->mem_wait, 1);
    fc_apps_poll(0);
    CHECK_STR(state_of("h"), want);
    h->mem_next_ms = 0;
    stub_heap_free = 8 * 1024; /* still no room */
    fc_apps_poll(0);
    CHECK_STR(state_of("h"), want);
    CHECK(h->mem_next_ms > fc_apps_now_ms());
    h->mem_next_ms = 0;
    stub_heap_free = -1;
    fc_apps_poll(0);
    CHECK(stub_log_has("app h: the heap has room for it again, starting it"));
    CHECK_STR(wait_state_of("h", "Running v1", 3000), "Running v1");
    CHECK_INT(h->mem_wait, 0);
    remove_all();

    /* Refused again: new env vars start it again too. */
    stub_heap_free = 8 * 1024;
    stub_heap_free_next[0] = 1024 * 1024;
    stub_heap_free_queued = 1;
    CHECK_INT(apply_one("h", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("h", want, 3000), want);
    stub_heap_free = -1;
    CHECK_INT(apply_one("h", "reg/app:v1", "{\"TICK_MS\": \"200\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("h", "Running v1", 3000), "Running v1");
    remove_all();
}

/* With starts deferred (the agent), an app applied waits, Preparing, until
 * fc_apps_start_pending: the agent calls it once the spec is freed. */
static void test_deferred_start(void)
{
    char err[160];
    fc_apps_defer_starts(1);
    CHECK_INT(apply_one("d", "reg/app:v1", NULL, err, sizeof(err)), 0);
    usleep(200 * 1000);
    CHECK_STR(state_of("d"), "Preparing ");
    CHECK_INT(fc_apps_start_pending(), 1);
    CHECK_STR(wait_state_of("d", "Running v1", 3000), "Running v1");
    CHECK_INT(fc_apps_start_pending(), 0);
    fc_apps_defer_starts(0);
    remove_all();
}

static void test_catalog_commit(void)
{
    char err[160], key[64], old_key[64];
    const char *apps[] = {"first", "reg/app:v1", NULL, "second", "reg/app:v1", NULL, NULL};
    CHECK_INT(apply_apps(apps, err, sizeof(err)), 0);
    wasm_key("first", find("first")->module_slot, old_key, sizeof(old_key));
    cJSON *catalog = read_catalog(NULL);
    wasm_key("first", free_flash_slot(catalog), key, sizeof(key));
    cJSON_Delete(catalog);

    /* A power cut after staging and before committing leaves v1 selected. */
    apps[1] = "reg/app:v2";
    apps[2] = "{\"TICK_MS\":\"222\"}";
    f_catalog_failure = true;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "cannot store the app catalog");
    CHECK(stub_kv(key) && stub_kv(old_key));
    CHECK_INT(find("first")->tick_ms, 100);
    stub_kv_del_fails = 0;
    f_catalog_failure = false;
    power_cycle();
    CHECK_STR(wait_state_of("first", "Running v1", 3000), "Running v1");
    CHECK_STR(find("first")->image, "reg/app:v1");
    CHECK_INT(find("first")->tick_ms, 100);
    CHECK_STR(wait_state_of("second", "Running v1", 3000), "Running v1");
    CHECK(!stub_kv(key) && stub_kv(old_key));

    /* A failed sync can leave the new catalog visible. A superseding spec
     * first restores the catalog of the live apps before reusing any slot. */
    f_catalog_failure = f_catalog_visible = true;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), -1);
    CHECK(stub_kv(key) && stub_kv(old_key));
    CHECK_CONTAINS(stub_kv(KV_APPS), "reg/app:v2");
    f_catalog_failure = f_catalog_visible = false;
    apps[1] = "reg/app:v1";
    apps[2] = NULL;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), 0);
    CHECK(strstr(stub_kv(KV_APPS), "reg/app:v2") == NULL);

    /* With the new catalog visible at power loss, v2 must boot even
     * though the write had returned an error to the previous process. */
    apps[1] = "reg/app:v2";
    apps[2] = "{\"TICK_MS\":\"222\"}";
    f_catalog_failure = f_catalog_visible = true;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), -1);
    f_catalog_failure = f_catalog_visible = false;
    power_cycle();
    CHECK_STR(wait_state_of("first", "Running v2", 3000), "Running v2");
    CHECK_INT(find("first")->tick_ms, 222);
    CHECK(stub_kv(key) && !stub_kv(old_key));
    apps[1] = "reg/app:v1";
    apps[2] = NULL;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), 0);
    apps[1] = "reg/app:v2";
    apps[2] = "{\"TICK_MS\":\"222\"}";

    /* A cut after the commit, before reclaiming the old module, selects v2. */
    stub_kv_del_fails = 1;
    CHECK_INT(apply_apps(apps, err, sizeof(err)), 0);
    CHECK(stub_kv(key) && stub_kv(old_key));
    stub_kv_del_fails = 0;
    power_cycle();
    CHECK_STR(wait_state_of("first", "Running v2", 3000), "Running v2");
    CHECK_STR(find("first")->image, "reg/app:v2");
    CHECK_INT(find("first")->tick_ms, 222);
    CHECK_STR(wait_state_of("second", "Running v1", 3000), "Running v1");
    CHECK(stub_kv(key) && !stub_kv(old_key));

    f_catalog_failure = true;
    fc_apps_plan_t plan;
    CHECK_INT(fc_apps_prepare(NULL, &plan), FC_APPS_CATALOG_ERROR);
    CHECK(find("first") && find("second") && stub_kv(key));
    f_catalog_failure = false;
    remove_all();
    CHECK(!stub_kv(key));
}

static void test_legacy_catalog_update(void)
{
    const char legacy[] = "[{\"name\":\"app\",\"image\":\"reg/app:v1\",\"env\":\"{}\"}]";
    CHECK_INT(fc_port_kv_set("app:app", s_v1, sizeof(s_v1)), 0);
    CHECK_INT(fc_port_kv_set(KV_APPS, legacy, sizeof(legacy) - 1), 0);
    power_cycle();
    CHECK_STR(wait_state("Running v1 reg/app:v1"), "Running v1 reg/app:v1");
    char err[160];
    int retry;
    f_catalog_failure = true;
    CHECK_INT(apply("reg/app:v2", err, sizeof(err), &retry), -1);
    CHECK_INT(retry, 1);
    CHECK_CONTAINS(err, "cannot store the app catalog");
    f_catalog_failure = false;
    CHECK(stored_is(s_v1) && stub_kv("app:app"));
    CHECK_INT(apply("reg/app:v2", err, sizeof(err), &retry), 0);
    power_cycle();
    CHECK_STR(wait_state("Running v2 reg/app:v2"), "Running v2 reg/app:v2");
    CHECK(stored_is(s_v2) && !stub_kv("app:app"));
    remove_all();
}

static void test_slot_lifetime(void)
{
    char err[160];
    const char *both[] = {"aux", "reg/app:v1", NULL, "main", "reg/app:v2", NULL, NULL};
    CHECK(!s_apps[0] && !s_apps[1]);
    CHECK_INT(apply_one("aux", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK(s_apps[0] && !s_apps[1]);
    int before = f_fetches;
    f_slots_left = 0;
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "out of memory for its slot");
    CHECK_INT(f_fetches, before);
    CHECK(s_apps[1] == NULL);
    f_slots_left = -1;
    stub_kv_set_fails = 1;
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK(s_apps[1] == NULL);
    stub_kv_set_fails = 0;
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    app_t *survivor = find("main");
    CHECK(survivor == s_apps[1] && survivor != NULL);
    CHECK_INT(apply_one("main", "reg/app:v2", NULL, err, sizeof(err)), 0);
    CHECK(s_apps[0] == NULL && find("main") == survivor);
    CHECK_STR(wait_state_of("main", "Running v2", 3000), "Running v2");
    CHECK_INT(mqtt_on_message(&s_sync[0], 7, NULL), -1); /* delivery after removal */
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    CHECK_INT(mqtt_on_message(&s_sync[0], 7, NULL), -1); /* same slot, new occupant */
    CHECK(find("main") == survivor);
    remove_all();
    CHECK(!s_apps[0] && !s_apps[1]);

    /* At boot a saved primary gets the only descriptor available, even
     * if it was installed in slot 1. Recover the other one offline. */
    CHECK_INT(apply_one("aux", "reg/app:v1", NULL, err, sizeof(err)), 0);
    both[5] = "{\"FC_PRIMARY\":\"true\"}";
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    f_slots_left = 1;
    before = f_fetches;
    power_cycle();
    CHECK(primary_app() == find("main") && find("main") != NULL);
    CHECK(s_restore && s_restore->child && find("aux") == NULL);
    CHECK_STR(wait_state_of("main", "Running v2", 3000), "Running v2");
    f_slots_left = -1;
    fc_apps_poll(0);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
    CHECK_INT(f_fetches, before);
    CHECK(s_restore == NULL);
    f_slots_left = 1;
    power_cycle();
    CHECK(waiting_slot("aux") != NULL);
    CHECK_INT(apply_one("main", "reg/app:v2", both[5], err, sizeof(err)), 0);
    CHECK(waiting_slot("aux") == NULL && stub_kv("app:aux") == NULL);
    CHECK_INT(f_fetches, before);
    f_slots_left = -1;
    remove_all();

    f_no_io = 1;
    CHECK_INT(apply_one("main", "reg/app:v2", both[5], err, sizeof(err)), 0);
    survivor = find("main");
    CHECK(survivor && survivor->mem_wait && !survivor->io && !survivor->thread_running);
    f_no_io = 0;
    survivor->mem_next_ms = 0;
    CHECK(fc_apps_primary_retry_due());
    stub_heap_free = 0;
    fc_apps_poll(1);
    survivor->mem_next_ms = 0;
    CHECK(!fc_apps_primary_retry_due()); /* an idle session was already released */
    stub_heap_free = -1;
    fc_apps_poll(0);
    CHECK_STR(wait_state_of("main", "Running v2", 3000), "Running v2");
    remove_all();
}

static void test_refused_primary_reloads_from_flash_once(void)
{
    char err[160];
    enum { STILL_FRAGMENTED, BELOW_FLOOR, ENOUGH };
    for (int after = STILL_FRAGMENTED; after <= ENOUGH; after++) {
        f_no_io = 1;
        CHECK_INT(apply_one("main", "reg/app:v2", "{\"FC_PRIMARY\":\"true\"}", err, sizeof(err)), 0);
        f_no_io = 0;
        app_t *a = find("main");
        CHECK(a && a->module && a->mem_wait);
        /* A joined runner that failed to allocate its 64 KiB linear memory. */
        a->mem_cost = 0;
        a->mem_block = 65536;
        a->mem_next_ms = 0;
        stub_heap_largest = 52 * 1024;
        int fetches = f_fetches;
        fc_apps_poll(1);
        CHECK(!a->module && a->relocated && a->start_pending && a->parked);
        CHECK(a->mem_next_ms > fc_apps_now_ms());
        a->mem_next_ms = 0;
        CHECK(fc_apps_primary_retry_due()); /* idle TLS may hold the missing block */
        stub_heap_free = a->mem_block;
        if (after != STILL_FRAGMENTED) {
            stub_heap_largest = a->mem_block;
        }
        if (after == ENOUGH) {
            stub_heap_free = -1;
        }
        fc_apps_poll(1);
        if (after != ENOUGH) {
            a->mem_next_ms = 0;
            CHECK(!fc_apps_primary_retry_due()); /* no third idle turn at unchanged heap */
            CHECK(!a->module && a->parked && a->start_pending);
            stub_heap_free = -1;
            stub_heap_largest = -1;
            stub_kv_get_fails = 1;
            fc_apps_poll(0);
            CHECK(!a->module && a->relocated && a->start_pending && a->mem_next_ms > fc_apps_now_ms());
            stub_kv_get_fails = 0;
            a->mem_next_ms = 0;
            f_no_io = 1;
            fc_apps_poll(0);
            CHECK(a->relocated && a->module && a->mem_wait && a->retried_idle);
            a->mem_next_ms = 0;
            CHECK(!fc_apps_primary_retry_due());
            f_no_io = 0;
            fc_apps_poll(0);
        }
        CHECK_STR(wait_state_of("main", "Running v2", 3000), "Running v2");
        CHECK(!a->relocated && !a->retried_idle); /* a successful instance ends this refusal */
        CHECK_INT(f_fetches, fetches);
        stub_heap_largest = -1;
        remove_all();
    }
}

static void test_completed_primary_wakes_parked_secondary(void)
{
    char err[160];
    const char *both[] = {"aux", "reg/app:v1", NULL, "main", "reg/t_entry",
                          "{\"FC_PRIMARY\":\"true\",\"FC_ENTRY\":\"run\"}", NULL};
    for (int room = 0; room <= 1; room++) {
        CHECK_INT(apply_one("aux", "reg/app:v1", NULL, err, sizeof(err)), 0);
        fc_apps_defer_starts(1);
        CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
        app_t *aux = find("aux");
        CHECK(aux && aux->parked);
        app_t *main_app = primary_app(); /* starts are deferred */
        CHECK(!fc_apps_awaiting_primary_completion());
        main_app->live = true;
        CHECK(!fc_apps_awaiting_primary_completion()); /* a perpetual tick loop */
        main_app->one_shot = true;
        CHECK(fc_apps_awaiting_primary_completion());
        aux->parked = false;
        CHECK(!fc_apps_awaiting_primary_completion());
        aux->parked = true;
        main_app->live = main_app->one_shot = false;
        aux->mem_next_ms = INT64_MAX;
        CHECK_INT(fc_apps_start_pending(), 1);
        while (!__atomic_load_n(&main_app->runner_done, __ATOMIC_ACQUIRE)) {
            usleep(1000);
        }
        CHECK(main_app->one_shot && !fc_apps_awaiting_primary_completion());
        stub_heap_free = room ? -1 : 0;
        fc_apps_poll(0);
        if (room) {
            CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
        } else {
            CHECK(aux->parked && !aux->module && aux->mem_next_ms == INT64_MAX);
            CHECK(!fc_apps_poll_due());
        }
        stub_heap_free = -1;
        fc_apps_defer_starts(0);
        remove_all();
    }
}

static void test_primary_gets_memory_first(void)
{
    char err[160];
    const char *flag = "{\"FC_PRIMARY\":\"true\"}";
    const char *both[] = {"aux", "reg/app:v1", NULL, "main", "reg/app:v1", flag, NULL};
    CHECK_INT(apply_one("aux", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
    int aux_slot = find("aux")->slot;
    fc_apps_defer_starts(1);
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    CHECK(find("aux")->module == NULL && !find("aux")->thread_running && !find("aux")->io);
    CHECK(find("main")->slot != aux_slot);
    CHECK_STR(state_of("aux"), "Preparing ");
    /* The primary takes its memory first. Refuse only the secondary's
     * event queue, then read the same customInfo field a device reports. */
    CHECK_INT(start_pending_app(find("main"), 0), 1);
    f_no_io = 1;
    CHECK_INT(fc_apps_start_pending(), 0);
    cJSON *pending_apps = cJSON_CreateArray();
    cJSON *pending_summary = cJSON_CreateObject();
    cJSON *pending_info = cJSON_CreateObject();
    fc_apps_status(pending_apps, pending_summary, pending_info);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(pending_info, "app.aux")),
              "waiting for app handles and event queue memory");
    cJSON_Delete(pending_apps);
    cJSON_Delete(pending_summary);
    cJSON_Delete(pending_info);
    f_no_io = 0;
    find("aux")->mem_next_ms = 0;
    CHECK_INT(fc_apps_start_pending(), 1);
    CHECK_STR(wait_state_of("main", "Running v1", 3000), "Running v1");
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");

    CHECK_INT(fc_metrics_app_set("aux", "ticks", 5, 1, 7), 0);
    fc_apps_file_changed(FC_BOARD_PATH);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
    fc_metric_point_t point;
    CHECK_INT(fc_metrics_app_points(&point, 1), 1);
    CHECK_STR(point.name, "app.aux.ticks");
    CHECK(point.d == 7);
    CHECK(fc_apps_poll_due());

    /* Even in the first catalog slot, aux stays on flash at managed boot. */
    fc_apps_hold_saved(1);
    power_cycle();
    CHECK(find("aux") == NULL && waiting_slot("aux") != NULL);
    cJSON *apps = cJSON_CreateArray(), *summary = cJSON_CreateObject(), *custom = cJSON_CreateObject();
    fc_apps_status(apps, summary, custom);
    CHECK_INT(cJSON_GetArraySize(apps), 2);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(custom, "app.aux")), "Preparing");
    cJSON_Delete(apps);
    cJSON_Delete(summary);
    cJSON_Delete(custom);
    int saved_fetches = f_fetches;
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    CHECK(find("aux") == NULL && f_fetches == saved_fetches);
    CHECK_INT(fc_apps_start_saved(), 1);
    CHECK_STR(wait_state_of("main", "Running v1", 3000), "Running v1");
    CHECK(find("aux") == NULL);
    CHECK_INT(fc_apps_poll(0), 1);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");

    /* A failed fetch of a primary update leaves both installed versions usable. */
    both[4] = "reg/missing";
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_STR(state_of("main"), "Running v1");
    CHECK_INT(fc_apps_start_pending(), 1);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
    both[4] = "reg/app:v2";
    int before = f_fetches;
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    CHECK_INT(f_fetches, before + 1);
    CHECK(find("aux")->module == NULL);
    CHECK_INT(fc_apps_start_pending(), 2);
    CHECK_STR(wait_state_of("main", "Running v2", 3000), "Running v2");
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");

    /* Ambiguous priority cannot remove an existing app or start a download. */
    both[2] = flag;
    before = f_fetches;
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_INT(f_fetches, before);
    CHECK_STR(state_of("main"), "Running v2");
    remove_all();

    /* On first install the secondary image waits even when it is listed first. */
    both[2] = NULL;
    before = f_fetches;
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_INT(f_fetches, before + 1);
    CHECK(find("aux") == NULL);
    CHECK_INT(fc_apps_start_pending(), 1);
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    CHECK_INT(fc_apps_start_pending(), 1);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
    cJSON *remaining = cJSON_Parse("[{\"name\":\"main\",\"appType\":\"container\"}]");
    fc_apps_plan_t plan;
    int retry;
    CHECK_INT(fc_apps_prepare(remaining, &plan), -1);
    CHECK(find("aux") != NULL);
    cJSON_AddStringToObject(remaining->child, "image", "reg/app:v2");
    cJSON_AddItemToObject(remaining->child, "envVars", cJSON_Parse(flag));
    CHECK_INT(fc_apps_prepare(remaining, &plan), 0);
    CHECK(find("aux") == NULL);
    /* A direct apply between prepare and finish can add the removed app. */
    CHECK_INT(apply_apps(both, err, sizeof(err)), 0);
    CHECK(find("aux") != NULL);
    CHECK_INT(fc_apps_apply_prepared(&plan, err, sizeof(err), &retry), 0);
    cJSON_Delete(remaining);
    CHECK(find("aux") == NULL && stub_kv("app:aux") == NULL);
    CHECK_STR(state_of("main"), "Running v2");

    /* A live primary whose memory can grow keeps the secondary on flash. */
    both[4] = "reg/growing";
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_INT(fc_apps_start_pending(), 1);
    CHECK_STR(wait_state_of("main", "Running ", 3000), "Running ");
    CHECK_INT(fc_apps_poll(0), 0);
    before = f_fetches;
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_INT(f_fetches, before);
    CHECK(find("aux") == NULL);

    /* Hand the role to aux while main's replacement is unavailable. The
     * saved role still chooses aux at the next boot. */
    both[2] = flag;
    both[4] = "reg/missing";
    both[5] = NULL;
    CHECK_INT(apply_apps(both, err, sizeof(err)), -1);
    CHECK_INT(fc_apps_start_pending(), 2);
    CHECK_STR(wait_state_of("aux", "Running v1", 3000), "Running v1");
    CHECK_STR(wait_state_of("main", "Running ", 3000), "Running ");
    fc_apps_hold_saved(1);
    power_cycle();
    CHECK(primary_app() == find("aux"));
    CHECK(find("main") == NULL && waiting_slot("main") != NULL);
    CHECK_INT(fc_apps_start_saved(), 1);
    fc_apps_defer_starts(0);
    remove_all();
}

/* Code the module runs as it is instantiated, before any call of the app:
 * a start section, or C constructors (__wasm_call_ctors). It runs within the
 * instruction budget, so a loop there is stopped as one in tick() is, and
 * the app can be removed; and in the app's context, so an fc call from there
 * is the app's. */
static void test_instantiation_code(void)
{
    static const char *const loops[] = {"reg/t_startloop", "reg/t_ctorloop"};
    char err[160];
    s_call_budget = 2000000;
    for (size_t i = 0; i < sizeof(loops) / sizeof(loops[0]); i++) {
        if (apply_one("c", loops[i], NULL, err, sizeof(err)) != 0) {
            fprintf(stderr, "%s: %s\n", loops[i], err);
            CHECK(0);
        }
        const char *want = "Error WASM module instantiate failed: Exception: instruction limit exceeded";
        if (strcmp(wait_state_of("c", want, 5000), want) != 0) {
            fprintf(stderr, "%s: %s\n", loops[i], state_of("c"));
        }
        CHECK_STR(state_of("c"), want);
        int64_t t0 = fc_apps_now_ms();
        remove_all(); /* while it restarts, or loops again */
        CHECK(fc_apps_now_ms() - t0 < 3000);
        CHECK_STR(state_of("c"), "none");
    }
    /* Removed while a constructor is in its loop: stopped there. No budget
     * at all (-1, as a module run once has), so that the loop is still
     * running when it is removed, however fast or slow the host. */
    s_call_budget = -1;
    CHECK_INT(apply_one("c", "reg/t_ctorloop", NULL, err, sizeof(err)), 0);
    usleep(300 * 1000);
    CHECK_STR(state_of("c"), "Preparing ");
    int64_t t0 = fc_apps_now_ms();
    remove_all();
    CHECK(fc_apps_now_ms() - t0 < STOP_MS);
    CHECK_STR(state_of("c"), "none");
    s_call_budget = FC_APP_CALL_BUDGET;
    CHECK_INT(apply_one("c", "reg/t_ctorfc", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("c", "Running from ctor", 3000), "Running from ctor");
    remove_all();
}

/* The largest .wasm the device takes follows its heap: what it can load
 * and still leave the agent its floor. One larger is refused before it is
 * downloaded, and the spec is not tried again for it. */
static void test_wasm_limit(void)
{
    char err[160];
    int retry = 0;
    CHECK_INT((int)wasm_limit(), MAX_WASM_SIZE); /* a port that cannot tell */
    stub_heap_free = 1024 * 1024;
    CHECK_INT((int)wasm_limit(), MAX_WASM_SIZE);
    stub_heap_free = 16 * 1024; /* below the floor: nothing */
    CHECK_INT((int)wasm_limit(), 0);
    stub_heap_free = s_heap_floor + LOAD_FACTOR * (long)(sizeof(w_app_hwcheck) - 1);
    CHECK_INT((int)wasm_limit(), (int)sizeof(w_app_hwcheck) - 1);
    CHECK(sizeof(s_v1) < sizeof(w_app_hwcheck));
    int fetches = f_fetches;
    cJSON *apps = cJSON_CreateArray();
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", "w");
    cJSON_AddStringToObject(a, "appType", "container");
    cJSON_AddStringToObject(a, "image", "reg/app_hwcheck");
    cJSON_AddItemToArray(apps, a);
    CHECK_INT(fc_apps_apply(apps, err, sizeof(err), &retry), -1);
    cJSON_Delete(apps);
    char want[96];
    snprintf(want, sizeof(want), "layer is %d bytes (max %d with this heap)", (int)sizeof(w_app_hwcheck),
             (int)sizeof(w_app_hwcheck) - 1);
    if (strstr(err, want) == NULL) {
        fprintf(stderr, "%s\n", err);
    }
    CHECK(strstr(err, want) != NULL);
    CHECK_INT(retry, 0);
    CHECK_INT(f_fetches, fetches + 1);
    /* A smaller one is taken. */
    CHECK_INT(apply_one("w", "reg/app:v1", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("w", "Running v1", 3000), "Running v1");
    remove_all();
    stub_heap_free = -1;
}

static void test_remove(void)
{
    char err[160];
    int retry;
    CHECK_INT(apply(NULL, err, sizeof(err), &retry), 0);
    CHECK_STR(app_state(), "none");
    CHECK(stub_kv("app:app") == NULL);
    power_cycle();
    CHECK_STR(app_state(), "none");
}

int main(void)
{
    stub_reset();
    make_v2();
    s_entry_wait_ms = 0; /* test_entry_waits tries the wait */
    fc_apps_set_hw(&s_fake);
    if (fc_apps_init() != 0) {
        return 1;
    }
    RUN(test_install_and_update);
    RUN(test_cannot_store);
    RUN(test_not_loadable);
    RUN(test_reboot);
    RUN(test_saved_module_broken);
    RUN(test_saved_apps_held);
    RUN(test_guard_silent_restart);
    RUN(test_guard_crash_at_start);
    RUN(test_guard_other_restarts);
    RUN(test_guard_blames_the_module_loading);
    RUN(test_guard_new_firmware);
    RUN(test_load_with_the_largest_block_held);
    RUN(test_remove);
    RUN(test_endless_loop);
    RUN(test_fc_imports);
    RUN(test_fc_refused);
    RUN(test_entry);
    RUN(test_entry_stopped);
    RUN(test_entry_waits);
    RUN(test_replace_idle);
    RUN(test_tail_calls);
    RUN(test_native_tables_sorted);
    RUN(test_libc);
    RUN(test_libc_oob);
    RUN(test_libc_loop);
    RUN(test_libc_mixed);
    RUN(test_libc_math);
    RUN(test_malloc);
    RUN(test_hold_leaves_room_for_the_stack);
    RUN(test_heap_low_holds);
    RUN(test_trim_heap_hold);
    RUN(test_heap_floor);
    RUN(test_deferred_start);
    RUN(test_catalog_commit);
    RUN(test_legacy_catalog_update);
    RUN(test_slot_lifetime);
    RUN(test_primary_gets_memory_first);
    RUN(test_refused_primary_reloads_from_flash_once);
    RUN(test_completed_primary_wakes_parked_secondary);
    RUN(test_instantiation_code);
    RUN(test_wasm_limit);
    RUN(test_fc_info);
    RUN(test_fc_timer);
    RUN(test_fc_config);
    RUN(test_fc_kv);
    RUN(test_fc_gpio);
    RUN(test_fc_metric);
    RUN(test_fc_bus);
    RUN(test_fc_serial);
    RUN(test_fc_grants);
    RUN(test_fc_limits);
    RUN(test_fc_events);
    RUN(test_mqtt_waits_for_heap);
    RUN(test_fc_mqtt);
    RUN(test_board_map);
    RUN(test_board_dt);
    RUN(test_example_apps);
    RUN(test_tinyml);
    RUN(test_tinymlp);
    RUN(test_arduino);
    RUN(test_arduino_blink);
    power_cycle(); /* stops the threads; leaves nothing for the leak checker */
    wasm_runtime_destroy();
    return TEST_RESULT();
}
