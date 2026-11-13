/*
 * WASM apps (fc_apps.c) with WAMR and the in-memory port: install, update,
 * a module that cannot be stored or loaded, the apps coming back after a
 * reboot, and removal. Downloads are replaced by fc_oci_fetch_layer below.
 */
#include "../../core/src/fc_apps.c"

#include <stdlib.h>

#include "port_stub.h"
#include "test.h"

/* An app that sets its state to "v1" on its first tick, built with
 * wasm-apps/build.sh's flags from:
 *     static int32_t sent;
 *     FC_EXPORT("tick") void tick(int32_t now_ms) { if (!sent) { sent = 1; FC_STATE("v1"); } }
 * "v2" is the same module with that string changed (make_v2). */
static const unsigned char s_v1[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0e, 0x03, 0x60,
  0x02, 0x7f, 0x7f, 0x00, 0x60, 0x00, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x00,
  0x02, 0x11, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x09, 0x73, 0x65, 0x74, 0x5f,
  0x73, 0x74, 0x61, 0x74, 0x65, 0x00, 0x00, 0x03, 0x03, 0x02, 0x01, 0x02,
  0x04, 0x05, 0x01, 0x70, 0x01, 0x01, 0x01, 0x05, 0x04, 0x01, 0x01, 0x01,
  0x01, 0x06, 0x0d, 0x02, 0x7f, 0x00, 0x41, 0x85, 0x08, 0x0b, 0x7f, 0x00,
  0x41, 0x90, 0x08, 0x0b, 0x07, 0x3d, 0x05, 0x06, 0x6d, 0x65, 0x6d, 0x6f,
  0x72, 0x79, 0x02, 0x00, 0x0e, 0x66, 0x63, 0x5f, 0x61, 0x62, 0x69, 0x5f,
  0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x00, 0x01, 0x04, 0x74, 0x69,
  0x63, 0x6b, 0x00, 0x02, 0x0a, 0x5f, 0x5f, 0x64, 0x61, 0x74, 0x61, 0x5f,
  0x65, 0x6e, 0x64, 0x03, 0x00, 0x0b, 0x5f, 0x5f, 0x68, 0x65, 0x61, 0x70,
  0x5f, 0x62, 0x61, 0x73, 0x65, 0x03, 0x01, 0x0a, 0x30, 0x02, 0x04, 0x00,
  0x41, 0x01, 0x0b, 0x29, 0x00, 0x02, 0x40, 0x41, 0x00, 0x2d, 0x00, 0x84,
  0x88, 0x80, 0x80, 0x00, 0x0d, 0x00, 0x41, 0x00, 0x41, 0x01, 0x3a, 0x00,
  0x84, 0x88, 0x80, 0x80, 0x00, 0x41, 0x80, 0x88, 0x80, 0x80, 0x00, 0x41,
  0x02, 0x10, 0x80, 0x80, 0x80, 0x80, 0x00, 0x0b, 0x0b, 0x0b, 0x0a, 0x01,
  0x00, 0x41, 0x80, 0x08, 0x0b, 0x03, 0x76, 0x31, 0x00
};
static unsigned char s_v2[sizeof(s_v1)];

static void make_v2(void)
{
    memcpy(s_v2, s_v1, sizeof(s_v1));
    for (size_t i = 0; i + 1 < sizeof(s_v2); i++) {
        if (s_v2[i] == 'v' && s_v2[i + 1] == '1') {
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
    const unsigned char *src = strcmp(image, "reg/app:v1") == 0   ? s_v1
                               : strcmp(image, "reg/app:v2") == 0 ? s_v2
                               : strcmp(image, "reg/app:bad") == 0 ? junk
                                                                   : NULL;
    size_t len = src == junk ? sizeof(junk) : sizeof(s_v1);
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
    }
    memset(s_apps, 0, sizeof(s_apps));
    wasm_runtime_destroy();
    s_inited = 0;
    CHECK_INT(fc_apps_init(NULL, 0), 0);
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
    if (fc_apps_init(NULL, 0) != 0) {
        return 1;
    }
    RUN(test_install_and_update);
    RUN(test_cannot_store);
    RUN(test_not_loadable);
    RUN(test_reboot);
    RUN(test_saved_module_broken);
    RUN(test_remove);
    power_cycle(); /* stops the threads; leaves nothing for the leak checker */
    wasm_runtime_destroy();
    return TEST_RESULT();
}
