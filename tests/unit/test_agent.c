/*
 * Agent: rendered specs (config files, errors, versions), status content,
 * certificate renewal answers, re-enrollment after expiry, X.509 dates.
 * Network calls are not made: downloads and apps are stubs.
 */
#include <unistd.h>
#define sleep(s) ((void)(s), 0u) /* retries must not slow the tests */

#include "../../core/src/fc_agent.c"

#include <stdlib.h>

#include "mbedtls/x509_crt.h"
#include "port_stub.h"
#include "test.h"

/* ---- stubs ------------------------------------------------------------------ */

static agent_t s_a;

static int s_fw_fetches, s_fw_refused, s_fw_ok;

int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len)
{
    s_fw_fetches++;
    if (s_fw_ok) {
        snprintf(digest, digest_len, "sha256:new");
        return 0;
    }
    if (s_fw_refused) {
        snprintf(err, err_len, "firmware image rejected by the bootloader checks");
        return FC_OCI_REFUSED;
    }
    snprintf(err, err_len, "registry down");
    return -1;
}

void fc_oci_set_signing_key(const char *pub_pem) {}

void fc_oci_set_turn(void (*begin)(void *ctx), void (*end)(void *ctx), void *ctx) {}

static const char *s_delta_outcome = "", *s_delta_reason = "";

const char *fc_oci_delta_outcome(const char **reason)
{
    *reason = s_delta_reason;
    return s_delta_outcome;
}

int fc_apps_init(void)
{
    return 0;
}

static int s_apps_applied, s_apps_removed_all;
static char s_apps_running_on[64]; /* the firmware the last apps were applied with */

/* Two firmwares: "fw:v1" has no env.snprintf, "fw:v2" has it. An app whose
 * image says "needs-v2" imports it, and v1 refuses it as fc_apps does. */
int fc_apps_apply(const cJSON *applications, char *err, size_t err_len, int *retry)
{
    *retry = 0;
    s_apps_applied++;
    snprintf(s_apps_running_on, sizeof(s_apps_running_on), "%s", s_a.os_image);
    if (cJSON_IsArray(applications) && cJSON_GetArraySize(applications) == 0) {
        s_apps_removed_all++;
    }
    int rc = 0;
    const cJSON *app;
    cJSON_ArrayForEach(app, applications) {
        const char *image = cJSON_GetStringValue(cJSON_GetObjectItem(app, "image"));
        if (image != NULL && strstr(image, "needs-v2") != NULL && strstr(s_a.os_image, "fw:v2") == NULL) {
            snprintf(err, err_len, "app spin: %s: not a loadable WASM module: imports env.snprintf, which this "
                     "firmware does not have", image);
            rc = -1;
        }
    }
    return rc;
}

static int s_apps_started, s_apps_started_depth = -1;

/* What fc_apps starts once the spec is freed: with the connection held. */
int fc_apps_start_pending(void)
{
    s_apps_started++;
    pthread_mutex_lock(&s_a.lock);
    s_apps_started_depth = s_a.turn_depth;
    pthread_mutex_unlock(&s_a.lock);
    return 0;
}

void fc_apps_defer_starts(int on)
{
    (void)on;
}

void fc_apps_poll(void)
{
}

void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info)
{
    cJSON_AddStringToObject(summary, "status", "NoApplications");
}

static long s_apps_stack = -1;

long fc_apps_stack_unused(void)
{
    return s_apps_stack;
}

int fc_apps_heap_agent(char *text, size_t len)
{
    return 0;
}

void fc_apps_set_hw(const fc_hw_t *hw) {}
void fc_apps_set_device(const char *name) {}

/* The console (fc_console.c): what the agent asks of it. */
static int s_console_syncs, s_console_stops;
void fc_console_set_device(const char *name) {}
int fc_console_sync(const cJSON *consoles, const fc_http_target_t *t)
{
    s_console_syncs++;
    return cJSON_GetArraySize(consoles) > 0 && t->session != NULL && t->cert_pem != NULL;
}
void fc_console_stop(void)
{
    s_console_stops++;
}
static int s_files_changed;

void fc_apps_file_changed(const char *path)
{
    s_files_changed++;
}

int fc_apps_check_file(const char *path, const char *data, char *err, size_t err_len)
{
    return 0;
}

void fc_apps_mqtt_hooks(int (*agent_idle)(int claim), void (*agent_connected)(void)) {}

static int on_config_file(const char *path, const char *data, size_t len, void *ctx)
{
    return len > 0 && data[0] == '!' ? -1 : 0; /* "!..." means invalid */
}

static fc_agent_config_t s_cfg = {.on_config_file = on_config_file, .agent_version = "flightctl-mcu/test"};

static void new_agent(void)
{
    stub_reset();
    free(s_a.mgmt_cert_pem);
    free(s_a.metrics);
    fc_identity_free(&s_a.id);
    memset(&s_a, 0, sizeof(s_a));
    s_a.stack_spec = s_a.stack_status = -1;
    s_a.export_low = -1;
    s_apps_stack = -1;
    s_a.cfg = &s_cfg;
    pthread_mutex_init(&s_a.lock, NULL);
    pthread_cond_init(&s_a.status_wake, NULL);
    pthread_cond_init(&s_a.status_sent, NULL);
    fc_identity_load(&s_a.id);
    s_fw_fetches = 0;
    s_fw_ok = 0;
    s_apps_applied = s_apps_removed_all = 0;
    s_apps_running_on[0] = '\0';
}

/* A reboot: memory gone, storage kept, then what fc_agent_run does first.
 * `starts`: the new image runs (else the bootloader went back to the old). */
static void reboot_device(int starts)
{
    free(s_a.mgmt_cert_pem);
    free(s_a.metrics);
    fc_identity_free(&s_a.id);
    memset(&s_a, 0, sizeof(s_a));
    s_a.stack_spec = s_a.stack_status = -1;
    s_a.export_low = -1;
    s_a.cfg = &s_cfg;
    pthread_mutex_init(&s_a.lock, NULL);
    pthread_cond_init(&s_a.status_wake, NULL);
    pthread_cond_init(&s_a.status_sent, NULL);
    fc_identity_load(&s_a.id);
    kv_load_str(KV_RENDERED, s_a.rendered_version, sizeof(s_a.rendered_version));
    kv_load_str(KV_OS_IMAGE, s_a.os_image, sizeof(s_a.os_image));
    kv_load_str(KV_OS_DIGEST, s_a.os_digest, sizeof(s_a.os_digest));
    kv_load_str(KV_OS_FAILED, s_a.failed_os_image, sizeof(s_a.failed_os_image));
    stub_ota_pending_verify = starts;
    resume_firmware_update(&s_a);
    stub_ota_pending_verify = 0;
}

/* The new firmware reached Flight Control (put_status, without the network). */
static void confirm_firmware(void)
{
    CHECK_INT(s_a.verifying, 1);
    kv_store_str(KV_OS_IMAGE, s_a.os_image);
    fc_port_kv_del(KV_OS_PENDING);
    fc_port_kv_del(KV_PREV_RENDERED);
    s_a.reported_rendered[0] = '\0';
    s_a.verifying = 0;
    wait_confirmed(&s_a); /* returns at once now */
}

static void apply(const char *version, const char *spec)
{
    char json[4096];
    snprintf(json, sizeof(json), "{\"metadata\":{\"annotations\":{\"device-controller/renderedVersion\":\"%s\"}},"
             "\"spec\":%s}", version, spec);
    apply_rendered(&s_a, json);
    start_apps(&s_a);
}

/* Every file comes again with each rendered version; an app restarted or
 * told of a new config each time would lose its state for nothing. */
static void apps_hear_only_of_files_that_changed(void)
{
    new_agent();
    s_files_changed = 0;
    const char *spec = "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/fc/board.json\",\"content\":\"{}\"}]}]}";
    apply("7", spec);
    CHECK_INT(s_files_changed, 1);
    apply("8", spec);
    CHECK_INT(s_files_changed, 1);
    apply("9", "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/fc/board.json\",\"content\":\"{ }\"}]}]}");
    CHECK_INT(s_files_changed, 2);
}

/* ---- dates ------------------------------------------------------------------ */

static void x509_dates_convert_to_epoch(void)
{
    mbedtls_x509_time t = {.year = 1970, .mon = 1, .day = 1};
    CHECK_INT(x509_epoch(&t), 0);
    t = (mbedtls_x509_time){.year = 2000, .mon = 3, .day = 1};
    CHECK_INT(x509_epoch(&t), 951868800);
    t = (mbedtls_x509_time){.year = 2024, .mon = 2, .day = 29, .hour = 23, .min = 59, .sec = 59};
    CHECK_INT(x509_epoch(&t), 1709251199);
    t = (mbedtls_x509_time){.year = 2026, .mon = 11, .day = 8, .hour = 12};
    CHECK_INT(x509_epoch(&t), 1794139200);
}

/* ---- specs ------------------------------------------------------------------ */

static void inline_config_is_written_and_the_version_kept(void)
{
    new_agent();
    apply("3", "{\"config\":[{\"name\":\"a\",\"inline\":["
               "{\"path\":\"/etc/a.json\",\"content\":\"{\\\"t\\\":40}\"},"
               "{\"path\":\"/etc/b.txt\",\"content\":\"aGVsbG8=\",\"contentEncoding\":\"base64\"}]}]}");
    CHECK_STR(stub_file("/etc/a.json"), "{\"t\":40}");
    CHECK_STR(stub_file("/etc/b.txt"), "hello");
    CHECK_STR(s_a.rendered_version, "3");
    CHECK_STR(stub_kv("rendered.ver"), "3");
    CHECK_STR(s_a.update_error, "");
}

/* A file the spec no longer gives is removed, as by the Linux agent, and the
 * apps hear of it; a metrics.json removed stops the exports. */
static void files_dropped_from_the_spec_are_removed(void)
{
    new_agent();
    /* Written before the device kept the list: not known, not removed. */
    fc_port_write_file("/etc/old", "x", 1);
    apply("3", "{\"config\":[{\"name\":\"a\",\"inline\":["
               "{\"path\":\"/etc/a.json\",\"content\":\"{}\"},"
               "{\"path\":\"/etc/fc/metrics.json\",\"content\":\"{}\"}]},"
               "{\"name\":\"b\",\"inline\":[{\"path\":\"/etc/b.txt\",\"content\":\"b\"}]}]}");
    CHECK_STR(stub_file("/etc/b.txt"), "b");
    CHECK_STR(stub_file("/etc/old"), "x");
    s_files_changed = 0;
    s_a.metrics_poke = 0;
    apply("4", "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/a.json\",\"content\":\"{}\"}]}]}");
    CHECK(stub_file("/etc/b.txt") == NULL);
    CHECK(stub_file("/etc/fc/metrics.json") == NULL);
    CHECK_STR(stub_file("/etc/a.json"), "{}");
    CHECK_STR(stub_file("/etc/old"), "x");
    CHECK_INT(s_files_changed, 2);
    CHECK_INT(s_a.metrics_poke, 1);
    CHECK_STR(s_a.rendered_version, "4");
    /* A provider that is not inline: its files are not known, nothing goes. */
    apply("5", "{\"config\":[{\"name\":\"g\",\"gitRef\":{}}]}");
    CHECK_STR(stub_file("/etc/a.json"), "{}");
    apply("6", "{}");
    CHECK(stub_file("/etc/a.json") == NULL);
    CHECK_STR(stub_kv("cfg.files"), "");
}

/* Flight Control 1.4.0 serves base64 content percent-encoded: "aGk=" as
 * "aGk%3D". */
static void percent_encoded_base64_is_decoded(void)
{
    new_agent();
    apply("3", "{\"config\":[{\"name\":\"a\",\"inline\":["
               "{\"path\":\"/etc/a\",\"content\":\"aGk%3D\",\"contentEncoding\":\"base64\"},"
               "{\"path\":\"/etc/b\",\"content\":\"aGk%2\",\"contentEncoding\":\"base64\"}]}]}");
    CHECK_STR(stub_file("/etc/a"), "hi");
    CHECK(stub_file("/etc/b") == NULL);
    CHECK_CONTAINS(s_a.update_error, "/etc/b: invalid base64");
}

static void a_rejected_config_keeps_the_old_version(void)
{
    new_agent();
    apply("3", "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/a.json\",\"content\":\"{}\"}]}]}");
    apply("4", "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/a.json\",\"content\":\"!bad\"}]}]}");
    CHECK_STR(s_a.rendered_version, "3");   /* reported: still the old one */
    CHECK_STR(stub_file("/etc/a.json"), "{}"); /* still in the spec: kept */
    CHECK_STR(s_a.attempted_version, "4");  /* not retried until a newer one */
    CHECK_CONTAINS(s_a.update_error, "/etc/a.json");
    cJSON *st = build_status(&s_a);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "updated"), "status")), "OutOfDate");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "config"), "renderedVersion")), "3");
    cJSON_Delete(st);
}

static const char *status_at(const cJSON *st, const char *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, obj), key));
}

/* spec.resources: applied, kept for the next boot, and its alerts in the
 * status and the summary. The stub's heap is half used. */
static void resource_monitors_are_kept_and_reported(void)
{
    new_agent();
    load_resources();
    apply("5", "{\"resources\":[{\"monitorType\":\"Memory\",\"samplingInterval\":\"1s\",\"alertRules\":["
               "{\"severity\":\"Critical\",\"duration\":\"0s\",\"percentage\":40,\"description\":\"\"}]}]}");
    CHECK_STR(s_a.update_error, "");
    CHECK_CONTAINS(stub_kv("resources"), "\"Memory\"");
    fc_sysinfo_t si;
    fc_port_sysinfo(&si);
    CHECK_INT(fc_res_sample(&si, fc_res_now() + 1), 1);
    cJSON *st = build_status(&s_a);
    CHECK_STR(status_at(st, "resources", "memory"), "Critical");
    CHECK_STR(status_at(st, "resources", "cpu"), "Healthy");
    CHECK_STR(status_at(st, "summary", "status"), "Error");
    CHECK_STR(status_at(st, "summary", "info"), "Critical resource alert: Memory");
    cJSON_Delete(st);

    /* after a reboot: the defaults, then the stored monitors */
    load_resources();
    fc_port_sysinfo(&si);
    fc_res_sample(&si, fc_res_now() + 2);
    st = build_status(&s_a);
    CHECK_STR(status_at(st, "resources", "memory"), "Critical");
    cJSON_Delete(st);

    /* a spec without resources: the defaults, and nothing kept */
    apply("6", "{}");
    CHECK(stub_kv("resources") == NULL);
    fc_res_sample(&si, fc_res_now() + 3);
    st = build_status(&s_a);
    CHECK_STR(status_at(st, "resources", "memory"), "Healthy");
    CHECK_STR(status_at(st, "summary", "status"), "Online");
    cJSON_Delete(st);
}

static void only_inline_config_is_supported(void)
{
    new_agent();
    apply("2", "{\"config\":[{\"name\":\"repo\",\"gitRef\":{}}]}");
    CHECK_CONTAINS(s_a.update_error, "only inline providers");
}

static void a_deeply_nested_spec_is_refused_without_crashing(void)
{
    new_agent();
    char json[8192] = "{\"spec\":";
    for (int i = 0; i < 200; i++) {
        strcat(json, "[");
    }
    for (int i = 0; i < 200; i++) {
        strcat(json, "]");
    }
    strcat(json, "}");
    apply_rendered(&s_a, json); /* over cJSON's nesting limit: not parsed */
    CHECK_STR(s_a.rendered_version, "");
}

static void the_current_firmware_is_not_downloaded_again(void)
{
    new_agent();
    snprintf(s_a.os_image, sizeof(s_a.os_image), "reg/fw:v1");
    apply("5", "{\"os\":{\"image\":\"reg/fw:v1\"}}");
    CHECK_INT(s_fw_fetches, 0);
    apply("6", "{\"os\":{\"image\":\"reg/fw:v2\"}}");
    CHECK_INT(s_fw_fetches, 1);
    CHECK_CONTAINS(s_a.update_error, "registry down");
    CHECK_STR(s_a.attempted_version, "5"); /* 6 failed to download: retried, not skipped */
}

/* A download that failed is tried again with the same spec; an image the
 * device refused is not: it would be written to the slot again for nothing. */
static void a_refused_firmware_is_not_downloaded_again(void)
{
    new_agent();
    snprintf(s_a.os_image, sizeof(s_a.os_image), "reg/fw:v1");
    apply("6", "{\"os\":{\"image\":\"reg/fw:v2\"}}");
    apply("6", "{\"os\":{\"image\":\"reg/fw:v2\"}}");
    CHECK_INT(s_fw_fetches, 2); /* the registry was down: tried again */
    s_fw_refused = 1;
    apply("6", "{\"os\":{\"image\":\"reg/fw:v2\"}}");
    CHECK_INT(s_fw_fetches, 3);
    CHECK_CONTAINS(s_a.update_error, "rejected by the bootloader");
    CHECK_STR(s_a.attempted_version, "6");
    CHECK_STR(s_a.rendered_version, ""); /* not applied: the device stays out of date, with the error */
    s_fw_refused = 0;
}

/* A spec with a new firmware and an app that needs it, in one step: the
 * running firmware (v1) would refuse the app, so the firmware comes first
 * and the app after the reboot, checked by v2. The 17/11 case: fw v3.14-rc1
 * and spin importing env.snprintf, refused by v3.13 with the firmware. */
static const char *FW_AND_APP = "{\"os\":{\"image\":\"reg/fw:v2\"},\"applications\":[{\"name\":\"spin\","
                                "\"appType\":\"container\",\"image\":\"reg/spin:needs-v2\"}]}";

static void a_new_firmware_and_an_app_that_needs_it_in_one_spec(void)
{
    new_agent();
    snprintf(s_a.os_image, sizeof(s_a.os_image), "reg/fw:v1");
    kv_store_str(KV_OS_IMAGE, "reg/fw:v1");
    apply("4", "{\"os\":{\"image\":\"reg/fw:v1\"}}");
    CHECK_STR(stub_kv("rendered.ver"), "4");

    s_fw_ok = 1;
    apply("5", FW_AND_APP);
    CHECK_INT(s_fw_fetches, 1);
    CHECK_INT(stub_reboots, 1);
    CHECK_INT(s_apps_applied, 1); /* the "4" spec's only: v1 never saw the app */
    CHECK_STR(s_a.update_error, "");
    CHECK_STR(stub_kv("rendered.ver"), "4"); /* not applied yet: asked for again after the reboot */
    CHECK_STR(s_a.reported_rendered, "4");

    reboot_device(1);
    CHECK_STR(s_a.os_image, "reg/fw:v2");
    CHECK_STR(s_a.rendered_version, "4"); /* the poll gets "5" again */
    confirm_firmware();
    apply("5", FW_AND_APP);
    CHECK_INT(s_fw_fetches, 1); /* v2 runs: not downloaded again */
    CHECK_INT(stub_reboots, 1);
    CHECK_INT(s_apps_applied, 2);
    CHECK_STR(s_apps_running_on, "reg/fw:v2");
    CHECK_STR(s_a.update_error, "");
    CHECK_STR(s_a.rendered_version, "5");
    CHECK_STR(stub_kv("rendered.ver"), "5");
    cJSON *st = build_status(&s_a);
    CHECK_STR(status_at(st, "updated", "status"), "UpToDate");
    CHECK_STR(status_at(st, "config", "renderedVersion"), "5");
    cJSON_Delete(st);
}

/* The new firmware does not start: the old one is back, says why, and keeps
 * its apps; the spec's app, which needs v2, is refused by v1 as before, but
 * the error shown is the firmware's. */
static void a_firmware_rolled_back_leaves_the_apps_to_the_old_one(void)
{
    new_agent();
    snprintf(s_a.os_image, sizeof(s_a.os_image), "reg/fw:v1");
    kv_store_str(KV_OS_IMAGE, "reg/fw:v1");
    kv_store_str(KV_RENDERED, "4");
    snprintf(s_a.rendered_version, sizeof(s_a.rendered_version), "4");
    s_fw_ok = 1;
    apply("5", FW_AND_APP);
    CHECK_INT(s_apps_applied, 0);
    reboot_device(0);
    CHECK_STR(s_a.os_image, "reg/fw:v1");
    CHECK_STR(s_a.failed_os_image, "reg/fw:v2");
    CHECK_STR(s_a.rendered_version, "4");
    apply("5", FW_AND_APP);
    CHECK_INT(s_fw_fetches, 1); /* not tried again */
    CHECK_INT(s_apps_applied, 1);
    CHECK_STR(s_apps_running_on, "reg/fw:v1");
    CHECK_CONTAINS(s_a.update_error, "failed to start and was rolled back");
    CHECK_STR(s_a.rendered_version, "4");
    CHECK_STR(s_a.attempted_version, "5");
}

/* A firmware download to be tried again leaves the apps as they are: v1
 * would refuse the new app, and download it again at every try. One the
 * device refused is not coming: the apps are updated by the running one. */
static void the_apps_wait_for_a_firmware_still_to_come(void)
{
    new_agent();
    snprintf(s_a.os_image, sizeof(s_a.os_image), "reg/fw:v1");
    apply("5", FW_AND_APP);
    CHECK_INT(s_fw_fetches, 1);
    CHECK_INT(s_apps_applied, 0);
    CHECK_CONTAINS(s_a.update_error, "registry down");
    CHECK_STR(s_a.attempted_version, "");
    s_fw_refused = 1;
    apply("6", "{\"os\":{\"image\":\"reg/fw:v2\"},\"applications\":[{\"name\":\"a\","
               "\"appType\":\"container\",\"image\":\"reg/a:2\"}]}");
    s_fw_refused = 0;
    CHECK_INT(s_apps_applied, 1);
    CHECK_STR(s_apps_running_on, "reg/fw:v1");
    CHECK_CONTAINS(s_a.update_error, "rejected by the bootloader");
    CHECK_STR(s_a.attempted_version, "6");
}

/* ---- certificates ----------------------------------------------------------- */

/* A certificate for `subject_key_pem` issued by a throwaway CA, as PEM. */
static char *issue_cert_cn(const char *subject_key_pem, const char *subject, const char *from, const char *to);
static char *issue_cert(const char *subject_key_pem, const char *from, const char *to)
{
    return issue_cert_cn(subject_key_pem, "CN=device", from, to);
}

static char *issue_cert_cn(const char *subject_key_pem, const char *subject, const char *from, const char *to)
{
    mbedtls_pk_context ca, subj;
    stub_ec_key(&ca);
    mbedtls_pk_init(&subj);
    mbedtls_pk_parse_key(&subj, (const unsigned char *)subject_key_pem, strlen(subject_key_pem) + 1, NULL, 0);
    mbedtls_x509write_cert w;
    mbedtls_x509write_crt_init(&w);
    mbedtls_x509write_crt_set_md_alg(&w, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&w, &subj);
    mbedtls_x509write_crt_set_issuer_key(&w, &ca);
    mbedtls_x509write_crt_set_subject_name(&w, subject);
    mbedtls_x509write_crt_set_issuer_name(&w, "CN=test-ca");
    unsigned char serial[] = {1};
    mbedtls_x509write_crt_set_serial_raw(&w, serial, sizeof(serial));
    mbedtls_x509write_crt_set_validity(&w, from, to);
    char *pem = calloc(1, 2048);
    mbedtls_x509write_crt_pem(&w, (unsigned char *)pem, 2048);
    mbedtls_x509write_crt_free(&w);
    mbedtls_pk_free(&ca);
    mbedtls_pk_free(&subj);
    return pem;
}

static int csr_answer(const char *cond, const char *cert_pem)
{
    char json[4096], b64[3000] = "";
    size_t n = 0;
    if (cert_pem) {
        mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &n, (const unsigned char *)cert_pem, strlen(cert_pem));
    }
    snprintf(json, sizeof(json), "{\"status\":{\"conditions\":[{\"type\":\"%s\",\"status\":\"True\"}]%s%s%s}}", cond,
             cert_pem ? ",\"certificate\":\"" : "", b64, cert_pem ? "\"" : "");
    snprintf(s_a.csr_name, sizeof(s_a.csr_name), "csr-1");
    return handle_csr(&s_a, json);
}

static void a_renewed_certificate_for_our_key_is_installed(void)
{
    new_agent();
    s_a.mgmt_cert_pem = strdup("old");
    char *pem = issue_cert(s_a.id.key_pem, "20260101000000", "20270101000000");
    CHECK_INT(csr_answer("Approved", pem), 1);
    CHECK_STR(s_a.mgmt_cert_pem, pem);
    CHECK_STR(stub_kv("mgmt.crt"), pem);
    /* Renewal again once a quarter of the lifetime has passed. */
    time_t nb = 1767225600, na = 1798761600; /* 2026-01-01, 2027-01-01 */
    CHECK_INT(s_a.cert_not_after, na);
    CHECK_INT(s_a.cert_renew_at, nb + (na - nb) / 4);
    free(pem);
}

static void a_certificate_for_another_key_is_refused(void)
{
    new_agent();
    s_a.mgmt_cert_pem = strdup("old");
    fc_identity_t other = {0};
    fc_identity_forget();
    fc_identity_load(&other); /* another key */
    char *pem = issue_cert(other.key_pem, "20260101000000", "20270101000000");
    CHECK_INT(csr_answer("Approved", pem), -1);
    CHECK_STR(s_a.mgmt_cert_pem, "old");
    free(pem);
    fc_identity_free(&other);
}

/* The Telemetry Gateway's certificate: its own key in storage, never the
 * management certificate's, and only with the CN the gateway reads. */
static void the_gateways_certificate_never_replaces_the_management_one(void)
{
    new_agent();
    s_a.mgmt_cert_pem = strdup("mgmt");
    char cn[96], json[4096], b64[3000];
    snprintf(cn, sizeof(cn), "CN=otel-%s", s_a.id.name);
    char *otel = issue_cert_cn(s_a.id.key_pem, cn, "20260101000000", "20270101000000");
    size_t n = 0;
    mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &n, (const unsigned char *)otel, strlen(otel));
    b64[n] = '\0';
    snprintf(json, sizeof(json), "{\"status\":{\"conditions\":[{\"type\":\"Approved\",\"status\":\"True\"}],"
             "\"certificate\":\"%s\"}}", b64);
    CHECK_INT(handle_otel_csr(&s_a, json), 1);
    CHECK_STR(s_a.otel_cert_pem, otel);
    CHECK_STR(stub_kv("otel.crt"), otel);
    CHECK_STR(s_a.mgmt_cert_pem, "mgmt");
    CHECK(stub_kv("mgmt.crt") == NULL);
    /* The device's key but another CN (a management certificate): refused. */
    char *mgmt = issue_cert(s_a.id.key_pem, "20260101000000", "20270101000000");
    mbedtls_base64_encode((unsigned char *)b64, sizeof(b64), &n, (const unsigned char *)mgmt, strlen(mgmt));
    b64[n] = '\0';
    snprintf(json, sizeof(json), "{\"status\":{\"certificate\":\"%s\"}}", b64);
    CHECK_INT(handle_otel_csr(&s_a, json), -1);
    CHECK_STR(s_a.otel_cert_pem, otel);
    CHECK_INT(handle_otel_csr(&s_a, "{\"status\":{\"conditions\":[{\"type\":\"Denied\",\"status\":\"True\"}]}}"), -1);
    CHECK_INT(handle_otel_csr(&s_a, "{\"status\":{}}"), 0); /* pending */
    /* Stored and read back at the next start; one of an older key is not. */
    free(s_a.otel_cert_pem);
    s_a.otel_cert_pem = NULL;
    s_a.otel_next_try = time(NULL) + 3600; /* nothing asked for meanwhile */
    CHECK_INT(otel_cert(&s_a, 1780000000), 0);
    CHECK_STR(s_a.otel_cert_pem, otel);
    free(mgmt);
    free(otel);
    free(s_a.otel_cert_pem);
    s_a.otel_cert_pem = NULL;
}

static void the_metrics_config_gives_the_gateway(void)
{
    new_agent();
    metrics_cfg_t c;
    CHECK_INT(metrics_config(&c), -1); /* none: nothing sent */
    const char *j = "{\"endpoint\": \"https://telemetry.example.com:4317\"}";
    fc_port_write_file(METRICS_CONFIG, j, strlen(j));
    CHECK_INT(metrics_config(&c), 0);
    CHECK_STR(c.host, "telemetry.example.com");
    CHECK_INT(c.port, 4317);
    CHECK_INT(c.interval_s, METRICS_DEFAULT_S);
    j = "{\"endpoint\": \"telemetry.example.com\", \"intervalSeconds\": 5}";
    fc_port_write_file(METRICS_CONFIG, j, strlen(j));
    CHECK_INT(metrics_config(&c), 0);
    CHECK_INT(c.port, 443);
    CHECK_INT(c.interval_s, METRICS_MIN_S);
    j = "{\"endpoint\": \":99999\"}";
    fc_port_write_file(METRICS_CONFIG, j, strlen(j));
    CHECK_INT(metrics_config(&c), -1);
    /* From the fleet: a new one is tried at once, whatever the backoff. */
    s_a.metrics_backoff_s = 3600;
    apply("4", "{\"config\":[{\"name\":\"m\",\"inline\":["
               "{\"path\":\"/etc/fc/metrics.json\",\"content\":\"{\\\"endpoint\\\":\\\"t.example:443\\\"}\"}]}]}");
    CHECK_INT(s_a.metrics_poke, 1);
    s_a.metrics_poke = 0;
    apply("5", "{\"config\":[{\"name\":\"m\",\"inline\":["
               "{\"path\":\"/etc/fc/metrics.json\",\"content\":\"{\\\"endpoint\\\":\\\"t.example:443\\\"}\"}]}]}");
    CHECK_INT(s_a.metrics_poke, 0); /* the same again: nothing */
}

static void the_device_counters_become_points(void)
{
    new_agent();
    fc_metric_point_t p[DEVICE_POINTS];
    int n = device_points(&s_a, p, 7);
    int io = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        io += strcmp(p[i].name, "device.network.io") == 0 && p[i].sum && p[i].start_ns == 7 && p[i].attr_key != NULL;
        skipped += strcmp(p[i].name, "device.metrics.skipped") == 0;
        CHECK(strncmp(p[i].name, "device.", 7) == 0);
    }
    CHECK_INT(io, 2);
    CHECK_INT(skipped, 1);
    CHECK(n <= (int)DEVICE_POINTS);
}

static void a_denied_request_is_a_failure(void)
{
    new_agent();
    CHECK_INT(csr_answer("Denied", NULL), -1);
    CHECK_INT(csr_answer("Approved", NULL), 0); /* approved, certificate not issued yet */
}

static void an_expired_certificate_starts_over_as_a_new_device(void)
{
    new_agent();
    fc_port_kv_set("mgmt.crt", "cert", 4);
    fc_port_kv_set("rendered.ver", "7", 1);
    fc_port_kv_set("app:alarm", "wasm", 4);
    s_a.cert_not_after = time(NULL) + 3600;
    reenroll_if_expired(&s_a); /* still valid: nothing happens */
    CHECK_INT(stub_reboots, 0);
    CHECK(stub_kv("mgmt.crt") != NULL);

    s_a.cert_not_after = time(NULL) - CERT_EXPIRED_GRACE_S - 1;
    reenroll_if_expired(&s_a);
    CHECK_INT(stub_reboots, 1);
    CHECK(stub_kv("mgmt.crt") == NULL);
    CHECK(stub_kv("device.key") == NULL);
    CHECK(stub_kv("rendered.ver") == NULL);
    CHECK_STR(stub_kv("app:alarm"), "wasm"); /* apps stay */
}

static void status_requests_are_urgent_or_grouped(void)
{
    new_agent();
    fc_agent_notify_status(); /* no agent running: ignored */
    s_running = &s_a;
    fc_agent_notify_status();
    CHECK_INT(s_a.status_dirty, 1);
    CHECK_INT(s_a.status_urgent, 0);
    request_status(&s_a);
    CHECK_INT(s_a.status_urgent, 1);
    s_running = NULL;
}

/* The status thread's part, played by hand: the status on its way is sent. */
static void *status_goes_out(void *arg)
{
    agent_t *a = arg;
    usleep(50 * 1000);
    pthread_mutex_lock(&a->lock);
    CHECK_INT(a->downloading, 0); /* the download waits for it */
    a->status_urgent = 0;
    a->putting = 0;
    pthread_cond_broadcast(&a->status_sent);
    pthread_mutex_unlock(&a->lock);
    return NULL;
}

/* One TLS connection at a time: a download takes the agent's turn once the
 * status asked for ("Downloading") is out, and a status reports after it. */
static void *export_ends(void *arg)
{
    agent_t *a = arg;
    usleep(50 * 1000);
    pthread_mutex_lock(&a->lock);
    CHECK_INT(a->downloading, 0); /* the download waits for it */
    pthread_mutex_unlock(&a->lock);
    metrics_done(a);
    return NULL;
}

/* A metrics export and a pull never share the time: the pull waits for the
 * export, and nothing of an export (not even its certificate) goes out
 * while a pull has the turn. */
static void metrics_and_pulls_take_turns(void)
{
    new_agent();
    s_a.exporting = 1;
    pthread_t t;
    pthread_create(&t, NULL, export_ends, &s_a);
    turn_begin(&s_a);
    CHECK_INT(s_a.downloading, 1);
    CHECK_INT(s_a.exporting, 0);
    pthread_join(t, NULL);
    const char *j = "{\"endpoint\": \"telemetry.example.com:443\"}";
    fc_port_write_file(METRICS_CONFIG, j, strlen(j));
    metrics_tick(&s_a); /* due, with no certificate yet */
    CHECK_INT(s_a.metrics_skipped, 1);
    CHECK_STR(s_a.otel_csr_name, ""); /* not asked for */
    CHECK_INT(s_a.exporting, 0);
    CHECK(s_a.metrics_next > time(NULL));
    turn_end(&s_a);
    CHECK_INT(fc_agent_busy(), 1); /* not started: busy anyway */
}

/* A spec waits for an export under way, and no export starts while it is
 * applied: both take the heap. */
static void *export_ends_later(void *arg)
{
    agent_t *a = arg;
    /* Until the spec has begun (no new export from now on), however slow
     * the machine: it waits for the export, so it cannot end before. */
    int applying = 0;
    while (!applying) {
        usleep(1000);
        pthread_mutex_lock(&a->lock);
        applying = a->applying;
        pthread_mutex_unlock(&a->lock);
    }
    metrics_done(a);
    return NULL;
}

static void a_spec_waits_for_an_export(void)
{
    new_agent();
    s_a.exporting = 1;
    pthread_t t;
    pthread_create(&t, NULL, export_ends_later, &s_a);
    time_t t0 = time(NULL);
    apply_begin(&s_a);
    CHECK_INT(s_a.exporting, 0); /* it waited for metrics_done */
    CHECK(time(NULL) - t0 < 5);
    pthread_join(t, NULL);
    const char *j = "{\"endpoint\": \"telemetry.example.com:443\"}";
    fc_port_write_file(METRICS_CONFIG, j, strlen(j));
    metrics_tick(&s_a); /* due, but a spec is being applied */
    CHECK_INT(s_a.exporting, 0);
    CHECK_STR(s_a.otel_csr_name, "");
    apply_end(&s_a);
    CHECK_INT(s_a.applying, 0);
}

/* The broker's handshake holds the agent from the moment it is allowed: a
 * spec that comes meanwhile waits for its end, and none starts during a
 * spec, nor an export during the handshake. */
static void *handshake_ends_later(void *arg)
{
    agent_t *a = arg;
    /* Until the spec has begun, however slow the machine. */
    int applying = 0, connecting = 0;
    while (!applying) {
        usleep(1000);
        pthread_mutex_lock(&a->lock);
        applying = a->applying;
        connecting = a->mqtt_connecting;
        pthread_mutex_unlock(&a->lock);
    }
    CHECK_INT(connecting, 1); /* the spec is waiting for it */
    CHECK_INT(idle_for_mqtt(1), 0); /* no second handshake while applying */
    mqtt_connected();
    return NULL;
}

static void a_spec_waits_for_the_brokers_handshake(void)
{
    new_agent();
    s_running = &s_a;
    CHECK_INT(idle_for_mqtt(0), 0); /* no status accepted yet */
    s_a.status_once = 1;
    CHECK_INT(idle_for_mqtt(0), 1);
    CHECK_INT(s_a.mqtt_connecting, 0); /* only asked */
    CHECK_INT(fc_agent_busy(), 0);
    CHECK_INT(idle_for_mqtt(1), 1);
    CHECK_INT(s_a.mqtt_connecting, 1);
    CHECK_INT(fc_agent_busy(), 1);
    const char *j = "{\"endpoint\": \"telemetry.example.com:443\"}";
    fc_port_write_file(METRICS_CONFIG, j, strlen(j));
    metrics_tick(&s_a); /* due, but the handshake runs */
    CHECK_INT(s_a.exporting, 0);
    CHECK_STR(s_a.otel_csr_name, "");
    pthread_t t;
    pthread_create(&t, NULL, handshake_ends_later, &s_a);
    time_t t0 = time(NULL);
    apply_begin(&s_a);
    CHECK_INT(s_a.mqtt_connecting, 0); /* it waited for mqtt_connected, */
    CHECK(time(NULL) - t0 < TURN_WAIT_S); /* not for its time to run out */
    pthread_join(t, NULL);
    apply_end(&s_a);
    CHECK_INT(idle_for_mqtt(1), 1); /* after the spec */
    mqtt_connected();
    fc_port_remove_file(METRICS_CONFIG);
    s_running = NULL;
}

static void a_download_takes_the_connections_turn(void)
{
    new_agent();
    s_console_stops = 0;
    turn_begin(&s_a);
    CHECK_INT(s_a.downloading, 1);
    CHECK_INT(s_console_stops, 1); /* a console's stream ends with the connection */
    turn_end(&s_a);
    CHECK_INT(s_a.downloading, 0);
    CHECK_INT(s_a.status_urgent, 1);

    s_a.putting = 1;
    pthread_t t;
    pthread_create(&t, NULL, status_goes_out, &s_a);
    turn_begin(&s_a);
    CHECK_INT(s_a.downloading, 1);
    CHECK_INT(s_a.putting, 0);
    CHECK_INT(s_a.status_urgent, 0);
    pthread_join(t, NULL);
    turn_end(&s_a);
    CHECK_INT(s_a.downloading, 0);

    /* Pulls held together (the apps of one spec): closed at the first,
     * opened again at the end of the hold. */
    s_a.status_urgent = 0;
    turn_hold(&s_a);
    CHECK_INT(s_a.downloading, 0); /* a hold alone closes nothing */
    turn_begin(&s_a);
    turn_end(&s_a);
    turn_begin(&s_a);
    turn_end(&s_a);
    CHECK_INT(s_a.downloading, 1);
    CHECK_INT(s_a.status_urgent, 0);
    turn_end(&s_a);
    CHECK_INT(s_a.downloading, 0);
    CHECK_INT(s_a.status_urgent, 1);
    turn_hold(&s_a);
    turn_end(&s_a);
    CHECK_INT(s_a.turn_depth, 0);

    /* A spec's apps start after the spec is applied and freed, the
     * connection still held closed; then it opens again. */
    s_apps_started = 0;
    s_apps_started_depth = -1;
    char json[512];
    snprintf(json, sizeof(json), "{\"metadata\":{\"annotations\":{\"device-controller/renderedVersion\":\"7\"}},"
             "\"spec\":{\"applications\":[]}}");
    CHECK_INT(apply_rendered(&s_a, json), 0);
    CHECK_INT(s_apps_started, 0);
    CHECK_INT(s_a.turn_depth, 1);
    start_apps(&s_a);
    CHECK_INT(s_apps_started, 1);
    CHECK_INT(s_apps_started_depth, 1);
    CHECK_INT(s_a.turn_depth, 0);
    start_apps(&s_a); /* once per spec */
    CHECK_INT(s_apps_started, 1);
}

/* The server answers at once while a console session is listed, with the
 * version already applied: only the console is started, nothing applied. */
static void a_console_alone_is_not_applied_again(void)
{
    new_agent();
    s_a.sess = fc_http_session_new();
    s_a.mgmt_cert_pem = strdup("cert");
    const char *with = "{\"metadata\": {\"annotations\": {\"" RENDERED_ANNOTATION "\": \"5\"}},"
                       " \"spec\": {\"consoles\": [{\"sessionID\": \"s1\", \"sessionMetadata\": \"{}\"}]}}";
    s_console_syncs = 0;
    CHECK_INT(consoles(&s_a, with, "5"), 1);
    CHECK_INT(s_console_syncs, 1);
    CHECK_INT(consoles(&s_a, with, "4"), 0); /* a new version too: applied */
    CHECK_INT(consoles(&s_a, "{\"metadata\": {}, \"spec\": {}}", "5"), 0);
    CHECK_INT(s_console_syncs, 2);
    free(s_a.mgmt_cert_pem);
    s_a.mgmt_cert_pem = NULL;
    fc_http_session_free(s_a.sess);
    s_a.sess = NULL;
}

/* ---- status content ---------------------------------------------------------- */

static const char *custom_info(cJSON *st, const char *key)
{
    return cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "systemInfo"), "customInfo"), key));
}

/* stackMinFreeBytes: each thread's lowest reading, the threads the port
 * cannot read left out, and nothing at all when it can read none. */
static void stack_headroom_is_reported_per_thread(void)
{
    new_agent();
    cJSON *st = build_status(&s_a);
    CHECK(custom_info(st, "stackMinFreeBytes") == NULL);
    cJSON_Delete(st);

    new_agent();
    s_a.stack_spec = 7000;
    s_apps_stack = 2500;
    stub_stack_unused = 6000;
    st = build_status(&s_a);
    CHECK_STR(custom_info(st, "stackMinFreeBytes"), "spec=7000 status=6000 apps=2500");
    cJSON_Delete(st);
    stub_stack_unused = 6500; /* a later, higher reading does not raise it */
    s_a.metrics_at = 0;
    st = build_status(&s_a);
    CHECK_STR(custom_info(st, "stackMinFreeBytes"), "spec=7000 status=6000 apps=2500");
    cJSON_Delete(st);
    CHECK_INT(stack_low(-1, 300), 300);
    CHECK_INT(stack_low(300, -1), 300);
    CHECK_INT(stack_low(300, 200), 200);
}

/* Each change in customInfo is an event in Flight Control 1.4: the counters
 * are read once an interval, and repeated as they were in between. */
static void counters_are_read_again_only_when_due(void)
{
    new_agent();
    cJSON *st = build_status(&s_a);
    CHECK_STR(custom_info(st, "reads"), "1");
    CHECK(custom_info(st, "netTxBytes") != NULL);
    CHECK(custom_info(st, "memFreeBytes") != NULL);
    char *first = cJSON_PrintUnformatted(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "systemInfo"), "customInfo"));
    cJSON_Delete(st);
    for (int i = 0; i < 3; i++) {
        st = build_status(&s_a);
        char *again = cJSON_PrintUnformatted(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "systemInfo"), "customInfo"));
        CHECK_STR(again, first); /* the same customInfo, byte for byte */
        free(again);
        cJSON_Delete(st);
    }
    free(first);
    CHECK_INT(stub_metrics_reads, 1);

    /* The first read was at start: the second comes after five minutes. */
    s_a.metrics_at -= METRICS_SECOND_READ_S - 1;
    cJSON_Delete(build_status(&s_a));
    CHECK_INT(stub_metrics_reads, 1);
    s_a.metrics_at -= 1;
    st = build_status(&s_a);
    CHECK_STR(custom_info(st, "reads"), "2");
    cJSON_Delete(st);

    s_a.metrics_at -= METRICS_INTERVAL_S - 1; /* then hourly */
    cJSON_Delete(build_status(&s_a));
    CHECK_INT(stub_metrics_reads, 2);
    s_a.metrics_at -= 1;
    cJSON_Delete(build_status(&s_a));
    CHECK_INT(stub_metrics_reads, 3);

    s_a.metrics_at += 10 * 365 * 86400; /* the clock was set back: read them now */
    st = build_status(&s_a);
    CHECK_STR(custom_info(st, "reads"), "4");
    cJSON_Delete(st);

    fc_agent_config_t cfg = s_cfg;
    cfg.metrics_interval_s = 30;
    s_a.cfg = &cfg;
    s_a.metrics_at -= 29;
    cJSON_Delete(build_status(&s_a));
    CHECK_INT(stub_metrics_reads, 4);
    s_a.metrics_at -= 1;
    cJSON_Delete(build_status(&s_a));
    CHECK_INT(stub_metrics_reads, 5);
    s_a.cfg = &s_cfg;
}

static const char *transition_time(cJSON *st)
{
    return cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(st, "conditions"), 0), "lastTransitionTime"));
}

/* Flight Control 1.4 logs an event for any change in the status: two
 * statuses with nothing new are the same, byte for byte. */
static void a_status_with_nothing_new_is_the_same(void)
{
    new_agent();
    cJSON_Delete(build_status(&s_a));
    snprintf(s_a.cond_since, sizeof(s_a.cond_since), "2026-01-01T00:00:00Z"); /* as if long ago */
    cJSON *st = build_status(&s_a);
    char *first = cJSON_PrintUnformatted(st);
    CHECK_STR(transition_time(st), "2026-01-01T00:00:00Z");
    cJSON_Delete(st);
    st = build_status(&s_a);
    char *again = cJSON_PrintUnformatted(st);
    CHECK_STR(again, first);
    free(again);
    cJSON_Delete(st);
    free(first);

    /* The condition changes: so does its time. */
    snprintf(s_a.update_error, sizeof(s_a.update_error), "registry down");
    st = build_status(&s_a);
    CHECK(strcmp(transition_time(st), "2026-01-01T00:00:00Z") != 0);
    cJSON_Delete(st);
}

static const char *last_delta(cJSON *st, const char *key)
{
    return cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "os"), "lastDelta"), key));
}

static void the_last_firmware_download_is_in_os_last_delta(void)
{
    new_agent();
    cJSON *st = build_status(&s_a);
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "os"), "lastDelta") == NULL); /* none yet */
    cJSON_Delete(st);

    s_delta_outcome = "Applied";
    st = build_status(&s_a);
    CHECK_STR(last_delta(st, "outcome"), "Applied");
    CHECK(last_delta(st, "fallbackReason") == NULL);
    CHECK(custom_info(st, "firmwareDownload") == NULL);
    cJSON_Delete(st);

    s_delta_outcome = "Fallback";
    s_delta_reason = "the delta does not apply";
    st = build_status(&s_a);
    CHECK_STR(last_delta(st, "outcome"), "Fallback");
    CHECK_STR(last_delta(st, "fallbackReason"), "the delta does not apply");
    cJSON_Delete(st);
    s_delta_outcome = s_delta_reason = "";
}

/* ---- decommissioning ------------------------------------------------------------ */

static const cJSON *decom_condition(cJSON *st)
{
    const cJSON *c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(st, "conditions")) {
        if (strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(c, "type")), "DeviceDecommissioning") == 0) {
            return c;
        }
    }
    return NULL;
}

static const char *lifecycle(cJSON *st)
{
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "lifecycle"), "status"));
}

/* The spec that asks for it has the device out of its fleet: no apps, no
 * config. None of it is applied. */
static void a_decommissioning_spec_is_not_applied(void)
{
    new_agent();
    apply("3", "{\"config\":[]}");
    apply("4", "{\"decommissioning\":{\"target\":\"Unenroll\"},\"applications\":[],"
               "\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/a.json\",\"content\":\"{}\"}]}]}");
    CHECK(stub_file("/etc/a.json") == NULL);
    CHECK_INT(s_apps_applied, 1); /* the first spec only */
    CHECK_STR(s_a.rendered_version, "3");
    CHECK_INT(s_a.decom, DECOM_STARTED);
    CHECK_STR(s_a.decom_target, "Unenroll");
    cJSON *st = build_status(&s_a);
    CHECK_STR(lifecycle(st), "Decommissioning");
    cJSON_Delete(st);
}

static void decommissioning_reports_started_then_completed_then_wipes(void)
{
    new_agent();
    fc_port_kv_set("mgmt.crt", "cert", 4);
    fc_port_kv_set("rendered.ver", "7", 1);
    fc_port_kv_set("app:alarm", "wasm", 4);
    cJSON *st = build_status(&s_a);
    CHECK(decom_condition(st) == NULL);
    CHECK_STR(lifecycle(st), "Enrolled");
    cJSON_Delete(st);

    apply("8", "{\"decommissioning\":{\"target\":\"Unenroll\"}}");
    decommission_step(&s_a); /* the last status accepted did not carry it yet */
    CHECK(stub_kv("decommission") == NULL);

    st = build_status(&s_a);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(decom_condition(st), "reason")), "Started");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(decom_condition(st), "status")), "True");
    cJSON_Delete(st);
    /* Started accepted: then Completed. */
    decommission_step(&s_a);
    CHECK_INT(s_a.decom, DECOM_COMPLETED);
    CHECK_STR(stub_kv("mgmt.crt"), "cert"); /* still needed for the next status */
    CHECK_INT(stub_reboots, 0);
    /* A restart now (power, the network gone) is a device as before: the
     * server still has Decommissioning, and the spec asks again. */
    CHECK(stub_kv("decommission") == NULL);
    CHECK_INT(decommissioned_at_boot(), 0);

    st = build_status(&s_a);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(decom_condition(st), "reason")), "Completed");
    cJSON_Delete(st);
    /* Completed accepted: wiped. */
    decommission_step(&s_a);
    CHECK(stub_kv("mgmt.crt") == NULL);
    CHECK(stub_kv("device.key") == NULL);
    CHECK(stub_kv("rendered.ver") == NULL);
    CHECK_INT(stub_reboots, 1);
    CHECK_STR(stub_kv("app:alarm"), "wasm"); /* Unenroll: the apps stay */
    CHECK_STR(stub_kv("decommission"), "Unenroll");
    CHECK_INT(s_apps_removed_all, 0);
}

static void a_factory_reset_also_removes_the_apps(void)
{
    new_agent();
    apply("2", "{\"decommissioning\":{\"target\":\"FactoryReset\"}}");
    CHECK_STR(s_a.decom_target, "FactoryReset");
    cJSON_Delete(build_status(&s_a));
    decommission_step(&s_a);
    CHECK_INT(s_apps_removed_all, 1);
    cJSON_Delete(build_status(&s_a));
    decommission_step(&s_a);
    CHECK_STR(stub_kv("decommission"), "FactoryReset");

    new_agent(); /* an unknown target is an Unenroll */
    apply("2", "{\"decommissioning\":{\"target\":\"Recycle\"}}");
    CHECK_STR(s_a.decom_target, "Unenroll");
    apply("3", "{\"decommissioning\":\"Unenroll\"}"); /* not an object: ignored */
}

static void a_decommissioned_device_does_not_enroll_at_boot(void)
{
    new_agent();
    fc_port_kv_set("mgmt.crt", "cert", 4);
    CHECK_INT(decommissioned_at_boot(), 0);
    CHECK_STR(stub_kv("mgmt.crt"), "cert");

    fc_port_kv_set("decommission", "Unenroll", 8);
    CHECK_INT(decommissioned_at_boot(), 1);
    CHECK(stub_kv("mgmt.crt") == NULL); /* a wipe cut short by a reboot is finished */
    CHECK(stub_kv("device.key") == NULL);
}

int main(void)
{
    fc_http_global_init();
    RUN(x509_dates_convert_to_epoch);
    RUN(inline_config_is_written_and_the_version_kept);
    RUN(a_rejected_config_keeps_the_old_version);
    RUN(files_dropped_from_the_spec_are_removed);
    RUN(apps_hear_only_of_files_that_changed);
    RUN(percent_encoded_base64_is_decoded);
    RUN(only_inline_config_is_supported);
    RUN(resource_monitors_are_kept_and_reported);
    RUN(a_deeply_nested_spec_is_refused_without_crashing);
    RUN(the_current_firmware_is_not_downloaded_again);
    RUN(a_refused_firmware_is_not_downloaded_again);
    RUN(a_new_firmware_and_an_app_that_needs_it_in_one_spec);
    RUN(a_firmware_rolled_back_leaves_the_apps_to_the_old_one);
    RUN(the_apps_wait_for_a_firmware_still_to_come);
    RUN(a_renewed_certificate_for_our_key_is_installed);
    RUN(a_certificate_for_another_key_is_refused);
    RUN(a_denied_request_is_a_failure);
    RUN(the_gateways_certificate_never_replaces_the_management_one);
    RUN(the_metrics_config_gives_the_gateway);
    RUN(the_device_counters_become_points);
    RUN(an_expired_certificate_starts_over_as_a_new_device);
    RUN(status_requests_are_urgent_or_grouped);
    RUN(a_download_takes_the_connections_turn);
    RUN(a_spec_waits_for_an_export);
    RUN(a_spec_waits_for_the_brokers_handshake);
    RUN(metrics_and_pulls_take_turns);
    RUN(a_console_alone_is_not_applied_again);
    RUN(counters_are_read_again_only_when_due);
    RUN(stack_headroom_is_reported_per_thread);
    RUN(a_status_with_nothing_new_is_the_same);
    RUN(the_last_firmware_download_is_in_os_last_delta);
    RUN(a_decommissioning_spec_is_not_applied);
    RUN(decommissioning_reports_started_then_completed_then_wipes);
    RUN(a_factory_reset_also_removes_the_apps);
    RUN(a_decommissioned_device_does_not_enroll_at_boot);
    free(s_a.mgmt_cert_pem);
    fc_identity_free(&s_a.id);
    stub_reset();
    return TEST_RESULT();
}
