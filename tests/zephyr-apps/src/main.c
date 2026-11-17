/*
 * The apps' C library on Zephyr (tests/zephyr-apps): the runtime of the apps
 * (core/src/fc_apps.c) with the real Zephyr port and C library, without the
 * agent or Flight Control. It runs the C library's tests of tests/unit
 * (t_libc, a pointer outside the app's memory, a loop in the library), the
 * example apps that use it (trend, hwcheck) and t_printf (%f at its deepest),
 * and reports the stack each app's thread has used. Built and run with
 * tests/zephyr-apps.sh. Prints "PASS" or "FAIL n" last.
 */
#include "../../../core/src/fc_apps.c"

#include <zephyr/kernel.h>

#include "fc_log.h"
#include "fc_port_zephyr.h"
#include "../../unit/wasm_vectors.h"
#include "vectors.h"

void fc_agent_notify_status(void) {}
int fc_agent_busy(void) { return 0; }

/* The registry: the modules by name. */
int fc_oci_fetch_layer(const char *image, const char *arch, const char *layer_type, size_t max_size,
                       unsigned char **out, size_t *out_len, char *digest, size_t digest_len, char *err,
                       size_t err_len)
{
    static const struct {
        const char *image;
        const unsigned char *code;
        size_t len;
    } reg[] = {
#define V(n) {"reg/" #n, w_##n, sizeof(w_##n)}
        V(t_state), V(t_libc), V(t_libcoob_1), V(t_libcoob_2), V(t_libcoob_3), V(t_libcoob_4), V(t_libcoob_5),
        V(t_libcloop), V(app_trend), V(app_hwcheck), V(t_printf),
#undef V
    };
    for (size_t i = 0; i < sizeof(reg) / sizeof(reg[0]); i++) {
        if (strcmp(image, reg[i].image) == 0) {
            *out = malloc(reg[i].len);
            if (*out == NULL) {
                snprintf(err, err_len, "no memory");
                return -1;
            }
            memcpy(*out, reg[i].code, reg[i].len);
            *out_len = reg[i].len;
            snprintf(digest, digest_len, "sha256:%s", image);
            return 0;
        }
    }
    snprintf(err, err_len, "%s: not found", image);
    return -1;
}

/* The board's own hardware, with a die_temp that rises 0.3 C a read, for
 * trend (the Zephyr port has no chip sensor yet). */
static int32_t s_temp = 40000;
static int fake_sensor(const char *name, int32_t *v)
{
    if (strcmp(name, "die_temp") != 0) {
        return -1;
    }
    *v = __atomic_add_fetch(&s_temp, 300, __ATOMIC_RELAXED);
    return 0;
}

static int s_fails;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printk("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                        \
            s_fails++;                                                                                                 \
        }                                                                                                              \
    } while (0)

static int apply_one(const char *name, const char *image, const char *env)
{
    cJSON *list = cJSON_CreateArray();
    if (name != NULL) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "name", name);
        cJSON_AddStringToObject(a, "appType", "container");
        cJSON_AddStringToObject(a, "image", image);
        if (env != NULL) {
            cJSON_AddItemToObject(a, "envVars", cJSON_Parse(env));
        }
        cJSON_AddItemToArray(list, a);
    }
    char err[160] = "";
    int retry;
    int rc = fc_apps_apply(list, err, sizeof(err), &retry);
    cJSON_Delete(list);
    if (rc != 0) {
        printk("apply %s: %s\n", image ? image : "(none)", err);
    }
    return rc;
}

/* "status state-or-error" of the app, or "none". */
static const char *state_of(const char *name)
{
    static char out[200];
    pthread_mutex_lock(&s_lock);
    app_t *a = find(name);
    snprintf(out, sizeof(out), "%s %s", a ? a->status : "none", a ? (a->error[0] ? a->error : a->state) : "");
    pthread_mutex_unlock(&s_lock);
    return out;
}

static const char *wait_state(const char *name, const char *prefix, int ms)
{
    for (int i = 0; i < ms / 20 && strncmp(state_of(name), prefix, strlen(prefix)) != 0; i++) {
        k_msleep(20);
    }
    return state_of(name);
}

/* The least stack the app's thread has had unused, as the device reports it
 * (fc_port_stack_unused, read once a second on the thread). */
static long stack_unused(const char *name)
{
    pthread_mutex_lock(&s_lock);
    app_t *a = find(name);
    long n = a ? __atomic_load_n(&a->stack_unused, __ATOMIC_RELAXED) : -2;
    pthread_mutex_unlock(&s_lock);
    return n;
}

/* Whether the device's log since `from` has this whole line. */
static int log_has(uint32_t from, const char *line)
{
    fc_log_entry_t e;
    uint32_t seq = from;
    while (fc_log_next(&seq, &e) == 0) {
        if (strstr(e.text, line) != NULL) {
            return 1;
        }
    }
    return 0;
}

static void show_log(uint32_t from, const char *app)
{
    fc_log_entry_t e;
    uint32_t seq = from;
    char tag[24];
    snprintf(tag, sizeof(tag), "app %s:", app);
    while (fc_log_next(&seq, &e) == 0) {
        if (strstr(e.text, tag) != NULL) {
            printk("  log: %s\n", e.text);
        }
    }
}

/* Runs one app until its state starts with `want`, then a little over a
 * second more so that its thread has read its stack after the C library's
 * work; reports what it used of THREAD_STACK. */
static void run(const char *name, const char *image, const char *env, const char *want)
{
    uint32_t from = fc_log_end();
    CHECK(apply_one(name, image, env) == 0);
    const char *st = wait_state(name, want, 5000);
    printk("%s: %s\n", image, st);
    CHECK(strncmp(st, want, strlen(want)) == 0);
    k_msleep(2200);
    long unused = stack_unused(name);
    printk("STACK %s: %ld of %d bytes used (%ld unused)\n", image, unused >= 0 ? (long)THREAD_STACK - unused : -1L,
           THREAD_STACK, unused);
    show_log(from, name);
}

int main(void)
{
    static fc_hw_t hw;
    CHECK(fc_port_zephyr_init() == 0);
    fc_hw_zephyr_fill(&hw);
    hw.sensor_read = fake_sensor;
    fc_apps_set_hw(&hw);
    CHECK(fc_apps_init() == 0);
    apply_one(NULL, NULL, NULL); /* none left from a previous run in the flash */

    /* No C library: the thread's baseline. */
    run("s", "reg/t_state", NULL, "Running");
    CHECK(apply_one(NULL, NULL, NULL) == 0);

    /* tests/unit's test_libc. */
    uint32_t from = fc_log_end();
    run("l", "reg/t_libc", NULL, "Running copied 7 -42 255 ff 3.14 xyz 0 A bad=0");
    CHECK(log_has(from, "app l: hello"));
    CHECK(log_has(from, "app l: n=5 more"));
    CHECK(log_has(from, "app l: ok"));
    CHECK(log_has(from, "app l: line"));
    CHECK(log_has(from, "app l: partial"));
    CHECK(apply_one(NULL, NULL, NULL) == 0);

    /* %f at its deepest. snprintf takes five arguments, one of them on the
     * stack, where WAMR's invokeNative for Thumb left the stack 4 bytes off
     * the 8 C wants (third_party/patches/wamr-thumb-stack-align.patch): a
     * double was then read from the wrong place. */
    from = fc_log_end();
    run("f", "reg/t_printf", NULL, "Running 3.14 -0.0 42");
    CHECK(log_has(from, "app f: f 3.141593 -0.000         3.1415926536 3.1     |"));
    CHECK(log_has(from, "app f: p 3|3.1|3.14|3.142|3.1416|3.14159|3.141593|3.1415927"));
    CHECK(log_has(from, "app f: q 1.00|2.50|0.12|100.00|3.14"));
    CHECK(log_has(from, "app f: d -1 -2147483647 4000000000 beef 000000ff str z"));
    CHECK(apply_one(NULL, NULL, NULL) == 0);

    /* The example apps. The heap the agent keeps (CONFIG_FC_APP_HEAP_CHECK):
     * what the instance left, and the lowest after it, are logged. */
    from = fc_log_end();
    run("trend", "reg/app_trend", NULL, "Running rising");
    CHECK(log_has(from, "app trend: instance made"));
    for (int i = 0; i < 100 && !log_has(from, "app trend (install): "); i++) {
        k_msleep(50);
    }
    CHECK(log_has(from, "app trend (install): "));
    char heap[96];
    CHECK(fc_apps_heap_agent(heap, sizeof(heap)) == 1);
    printk("heapAgentBytes %s\n", heap);
    CHECK(apply_one(NULL, NULL, NULL) == 0);
    /* With less heap than the floor, the app is refused. */
    long floor = s_heap_floor;
    s_heap_floor = fc_port_heap_free() + 1024;
    CHECK(apply_one("trend", "reg/app_trend", NULL) != 0 || strncmp(wait_state("trend", "Error", 3000), "Error", 5) == 0);
    printk("floor above the heap: %s\n", state_of("trend"));
    s_heap_floor = floor;
    apply_one(NULL, NULL, NULL);
    run("hw", "reg/app_hwcheck", "{\"FC_ALLOW\": \"i2c0 spi0 uart0 adc0 pwm0\", \"TICK_MS\": \"100\"}", "Running i2c");
    CHECK(apply_one(NULL, NULL, NULL) == 0);

    /* A pointer outside the app's memory stops it, as a trap does; so does
     * abort (t_libcoob_5). */
    static const char *const oob[] = {"reg/t_libcoob_1", "reg/t_libcoob_2", "reg/t_libcoob_3", "reg/t_libcoob_4",
                                      "reg/t_libcoob_5"};
    for (size_t i = 0; i < sizeof(oob) / sizeof(oob[0]); i++) {
        CHECK(apply_one("o", oob[i], NULL) == 0);
        const char *st = wait_state("o", "Error", 3000);
        printk("%s: %s\n", oob[i], st);
        CHECK(strncmp(st, "Error", 5) == 0);
        CHECK(apply_one(NULL, NULL, NULL) == 0);
    }

    /* A loop in the C library is stopped after a second. On native_sim the
     * simulated clock stands still while the host runs memset, so there the
     * instruction budget stops it instead. */
    int64_t t0 = fc_apps_now_ms();
    CHECK(apply_one("p", "reg/t_libcloop", NULL) == 0);
    const char *st = wait_state("p", "Error", 8000);
    printk("t_libcloop: %s after %lld ms\n", st, (long long)(fc_apps_now_ms() - t0));
    CHECK(strstr(st, IS_ENABLED(CONFIG_ARCH_POSIX) ? "instruction limit" : "more than 1 s") != NULL);
    CHECK(apply_one(NULL, NULL, NULL) == 0);

    if (s_fails == 0) {
        printk("PASS\n");
    } else {
        printk("FAIL %d\n", s_fails);
    }
#ifdef CONFIG_ARCH_POSIX
    extern void nsi_exit(int code);
    nsi_exit(s_fails != 0);
#endif
    return 0;
}
