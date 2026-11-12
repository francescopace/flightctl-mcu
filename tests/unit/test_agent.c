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

static int s_fw_fetches, s_fw_refused;

int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len)
{
    s_fw_fetches++;
    if (s_fw_refused) {
        snprintf(err, err_len, "firmware image rejected by the bootloader checks");
        return FC_OCI_REFUSED;
    }
    snprintf(err, err_len, "registry down");
    return -1;
}

void fc_oci_set_signing_key(const char *pub_pem) {}

static const char *s_delta_outcome = "", *s_delta_reason = "";

const char *fc_oci_delta_outcome(const char **reason)
{
    *reason = s_delta_reason;
    return s_delta_outcome;
}

int fc_apps_init(const void *natives, int n_natives)
{
    return 0;
}

int fc_apps_apply(const cJSON *applications, char *err, size_t err_len, int *retry)
{
    *retry = 0;
    return 0;
}

void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info)
{
    cJSON_AddStringToObject(summary, "status", "NoApplications");
}

static int on_config_file(const char *path, const char *data, size_t len, void *ctx)
{
    return len > 0 && data[0] == '!' ? -1 : 0; /* "!..." means invalid */
}

static agent_t s_a;
static fc_agent_config_t s_cfg = {.on_config_file = on_config_file, .agent_version = "flightctl-mcu/test"};

static void new_agent(void)
{
    stub_reset();
    free(s_a.mgmt_cert_pem);
    free(s_a.metrics);
    fc_identity_free(&s_a.id);
    memset(&s_a, 0, sizeof(s_a));
    s_a.cfg = &s_cfg;
    pthread_mutex_init(&s_a.lock, NULL);
    pthread_cond_init(&s_a.status_wake, NULL);
    fc_identity_load(&s_a.id);
    s_fw_fetches = 0;
}

static void apply(const char *version, const char *spec)
{
    char json[4096];
    snprintf(json, sizeof(json), "{\"metadata\":{\"annotations\":{\"device-controller/renderedVersion\":\"%s\"}},"
             "\"spec\":%s}", version, spec);
    apply_rendered(&s_a, json);
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

static void a_rejected_config_keeps_the_old_version(void)
{
    new_agent();
    apply("3", "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/a.json\",\"content\":\"{}\"}]}]}");
    apply("4", "{\"config\":[{\"name\":\"a\",\"inline\":[{\"path\":\"/etc/a.json\",\"content\":\"!bad\"}]}]}");
    CHECK_STR(s_a.rendered_version, "3");   /* reported: still the old one */
    CHECK_STR(s_a.attempted_version, "4");  /* not retried until a newer one */
    CHECK_CONTAINS(s_a.update_error, "/etc/a.json");
    cJSON *st = build_status(&s_a);
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "updated"), "status")), "OutOfDate");
    CHECK_STR(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "config"), "renderedVersion")), "3");
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

/* ---- certificates ----------------------------------------------------------- */

/* A certificate for `subject_key_pem` issued by a throwaway CA, as PEM. */
static char *issue_cert(const char *subject_key_pem, const char *from, const char *to)
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
    mbedtls_x509write_crt_set_subject_name(&w, "CN=device");
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

/* ---- status content ---------------------------------------------------------- */

static const char *custom_info(cJSON *st, const char *key)
{
    return cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(st, "systemInfo"), "customInfo"), key));
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

int main(void)
{
    fc_http_global_init();
    RUN(x509_dates_convert_to_epoch);
    RUN(inline_config_is_written_and_the_version_kept);
    RUN(a_rejected_config_keeps_the_old_version);
    RUN(only_inline_config_is_supported);
    RUN(a_deeply_nested_spec_is_refused_without_crashing);
    RUN(the_current_firmware_is_not_downloaded_again);
    RUN(a_refused_firmware_is_not_downloaded_again);
    RUN(a_renewed_certificate_for_our_key_is_installed);
    RUN(a_certificate_for_another_key_is_refused);
    RUN(a_denied_request_is_a_failure);
    RUN(an_expired_certificate_starts_over_as_a_new_device);
    RUN(status_requests_are_urgent_or_grouped);
    RUN(counters_are_read_again_only_when_due);
    RUN(a_status_with_nothing_new_is_the_same);
    RUN(the_last_firmware_download_is_in_os_last_delta);
    free(s_a.mgmt_cert_pem);
    fc_identity_free(&s_a.id);
    stub_reset();
    return TEST_RESULT();
}
