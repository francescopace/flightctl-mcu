/*
 * WASM apps (fc_apps.c, fc_appapi.c, fc_board.c) with WAMR and the in-memory
 * port: install, update, a module that cannot be stored or loaded, the apps
 * coming back after a reboot, removal, an app that never returns; and the
 * interface against a fake board (below): imports, timers, config, storage,
 * GPIO and LED, grants, the board map, and the example apps of wasm-apps/.
 * Downloads are replaced by fc_oci_fetch_layer below; the modules are in
 * wasm_vectors.h (tests/unit/gen-wasm-vectors.sh).
 */
#include "../../core/src/fc_apps.c"

#include <stdlib.h>

#include "port_stub.h"
#include "test.h"
#include "wasm_vectors.h"

/* An app that sets its state to "v1" (tests/unit/wasm/t_state.c); "v2" is the
 * same module with that string changed (make_v2). */
#define s_v1 w_t_state
static unsigned char s_v2[sizeof(s_v1)];

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

/* The registry: three images. */
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
#define V(n) {"reg/" #n, w_##n, sizeof(w_##n)}
        V(t_imports), V(t_timer), V(t_config), V(t_kv), V(t_gpio), V(t_perm), V(t_sleep), V(t_oob),
        V(t_badsig), V(t_unknown), V(t_mixed), V(t_events), V(t_neglen), V(t_info), V(app_alarm_v1), V(app_alarm_v2), V(app_trend), V(t_env),
#undef V
    };
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
static int f_led_open, f_led_writes;
static uint8_t f_led[3];
static int32_t f_temp = 41000;
static pthread_mutex_t f_lock = PTHREAD_MUTEX_INITIALIZER;

static char *f_defaults(void)
{
    return strdup("{\"gpio\": {\"button0\": {\"pin\": 9, \"active_low\": true}, \"relay1\": {\"pin\": 5, \"safe\": 0}},"
                  " \"led\": {\"led0\": {\"pin\": 8}}, \"i2c\": {\"i2c0\": {\"sda\": 1, \"scl\": 2}}}");
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
    *v = __atomic_load_n(&f_temp, __ATOMIC_ACQUIRE);
    return 0;
}

static const fc_hw_t s_fake = {
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
    int same = fc_port_kv_get("app:app", &v, &len) == 0 && len == sizeof(s_v1) && memcmp(v, m, len) == 0;
    free(v);
    return same;
}

/* Stops everything as a power cut would, then boots: fc_apps_init again. */
static void power_cycle(void)
{
    for (int i = 0; i < MAX_APPS; i++) {
        app_release(&s_apps[i]);
        reset_slot(i);
    }
    wasm_runtime_destroy();
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
    fc_port_kv_set("app:app", (const char *)w_t_env, sizeof(w_t_env));
    power_cycle();
    CHECK(strncmp(app_state(), "Error", 5) == 0);
    fc_port_kv_set("app:app", "junk", 4);
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
    CHECK(strstr(err, "built for the earlier app interface (imports from \"env\")") != NULL);
    CHECK_STR(state_of("r"), "none");
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
    CHECK_STR(wait_state_of("p", "Running -4 -2 -8 -3 -4", 3000), "Running -4 -2 -8 -3 -4");
    /* Stopped: the relay to its safe level, the button released, the LED off. */
    remove_all();
    CHECK_INT(f_read(5), 0);
    CHECK(f_released[9]);
    CHECK(f_cb[9] == NULL);
    CHECK(!f_led_open);
}

static void test_fc_grants(void)
{
    char err[160];
    /* Nothing granted: EPERM, but a name the map lacks is ENOENT. */
    CHECK_INT(apply_one("p", "reg/t_perm", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("p", "Running -3 -2 -3 -3 -3", 3000), "Running -3 -2 -3 -3 -3");
    /* New env vars restart it with the new grants: relay1 opens, once; the
     * I2C bus is granted but not in this firmware yet. */
    CHECK_INT(apply_one("p", "reg/t_perm", "{\"FC_ALLOW\": \"relay1 i2c0\"}", err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("p", "Running 0 -2 -8 -3 -4", 3000), "Running 0 -2 -8 -3 -4");
    CHECK(strstr(stub_kv("apps"), "relay1 i2c0") != NULL);
    /* After a reboot, before the spec comes, it has the same grants. */
    power_cycle();
    CHECK_STR(wait_state_of("p", "Running 0 -2 -8 -3 -4", 3000), "Running 0 -2 -8 -3 -4");
    remove_all();
}

static void test_fc_limits(void)
{
    char err[160];
    CHECK_INT(apply_one("s", "reg/t_sleep", NULL, err, sizeof(err)), 0);
    CHECK_STR(wait_state_of("s", "Error Exception: a call took more than 1 s", 3000),
              "Error Exception: a call took more than 1 s");
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
    remove_all();
    fc_apps_post(a, EV_CONFIG, -1, 0); /* stopped: dropped */
    CHECK_INT(a->ev_count, 0);
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
    fc_apps_set_hw(&s_fake);
    if (fc_apps_init() != 0) {
        return 1;
    }
    RUN(test_install_and_update);
    RUN(test_cannot_store);
    RUN(test_not_loadable);
    RUN(test_reboot);
    RUN(test_saved_module_broken);
    RUN(test_remove);
    RUN(test_endless_loop);
    RUN(test_fc_imports);
    RUN(test_fc_refused);
    RUN(test_fc_info);
    RUN(test_fc_timer);
    RUN(test_fc_config);
    RUN(test_fc_kv);
    RUN(test_fc_gpio);
    RUN(test_fc_grants);
    RUN(test_fc_limits);
    RUN(test_fc_events);
    RUN(test_board_map);
    RUN(test_example_apps);
    power_cycle(); /* stops the threads; leaves nothing for the leak checker */
    wasm_runtime_destroy();
    return TEST_RESULT();
}
