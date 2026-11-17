/*
 * Agent state machine: enrollment, then two POSIX threads:
 *  - spec thread:   long-polls GET /devices/{name}/rendered and applies it
 *  - status thread: PUT /devices/{name}/status every interval or on change
 */
#include "fc_agent.h"

#include "fc_apps.h"
#include "fc_console.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "fc_crypto.h"
#include "fc_http.h"
#include "fc_identity.h"
#include "fc_metrics.h"
#include "fc_oci.h"
#include "fc_port.h"
#include "fc_resources.h"
#include "fc_tls_pool.h"
#include "mbedtls/base64.h"
#include "mbedtls/x509_crt.h"

#define KV_MGMT_CERT "mgmt.crt"
#define KV_RENDERED  "rendered.ver"
#define KV_OS_IMAGE  "os.image"
#define KV_OS_DIGEST "os.digest"
/* Firmware update bookkeeping across the reboot into the new image. */
#define KV_OS_PENDING        "os.pending"     /* image being verified */
#define KV_OS_PENDING_DIGEST "os.pend.digest"
#define KV_PREV_RENDERED     "rendered.prev"  /* rendered version before the update */
#define KV_OS_FAILED         "os.failed"      /* image that was rolled back */
/* Decommissioned: the target asked for. Kept when everything else is wiped,
 * so the device does not enroll again on its own at the next boot. */
#define KV_DECOMMISSIONED    "decommission"
/* The Telemetry Gateway's certificate: for the device key, signed by
 * flightctl.io/device-svc-client. Never the management certificate's key in
 * storage. */
#define KV_OTEL_CERT         "otel.crt"
/* The paths of the config files the last spec gave, one per line: those the
 * next spec no longer has are removed, as the Linux agent does. */
#define KV_CONFIG_FILES      "cfg.files"
#define KV_RESOURCES         "resources"      /* spec.resources last applied */

#define API "/api/v1"
#define RENDERED_ANNOTATION "device-controller/renderedVersion"
#define SPEC_POLL_TIMEOUT_S 300 /* server holds the long-poll up to ~4 min */
/* While a console session is listed, the server answers the long-poll at
 * once with the same version: the next poll waits this long. */
#define CONSOLE_POLL_S 5
/* Only sessions seen before listed for this long (Flight Control leaves one
 * listed when its user left before the device came): polled less often.
 * A new one is still seen within CONSOLE_STALE_POLL_S, well under the 50 s
 * the UI's Logs tab waits. */
#define CONSOLE_STALE_AFTER_S 60
#define CONSOLE_STALE_POLL_S 15
#define RETRY_DELAY_S 10
/* A new firmware must reach Flight Control (one accepted status update)
 * within this time, or it is rolled back to the previous image
 * (fc_agent_config_t.ota_verify_timeout_s overrides it). */
#define OTA_VERIFY_TIMEOUT_S 150
/* Liveness: no accepted status update for this long means the agent is
 * stuck (network stack, TLS, a hung connection): restart the device. */
#define STATUS_STALL_REBOOT_S 600
/* The agent's own changes (spec applied, firmware confirmed) are reported
 * within a few seconds. Changes reported by apps and the firmware (app state,
 * alarms) are grouped: at most one status every NOTIFY_MIN_GAP_S, so a value
 * flapping around a threshold cannot flood the network (each status is about
 * 5 KB on the wire, the server's answer included). */
#define STATUS_MIN_GAP_S 3
#define NOTIFY_MIN_GAP_S 15
/* How long a download waits for a status already on its way (or asked for,
 * such as "Downloading") before it takes the connection's turn anyway. */
#define TURN_WAIT_S 30
/* Management certificate renewal, as the Linux agent does it: once a quarter
 * of the lifetime has passed (75% left), ask for a new certificate for the
 * same key. The server approves renewal requests on its own when they come
 * with the current management certificate. Failures back off up to an hour. */
#define CERT_RENEW_AT_PERCENT 25
#define CERT_RETRY_MIN_S 60
#define CERT_RETRY_MAX_S 3600
#define RENEWAL_SIGNER "flightctl.io/device-management-renewal"
/* A device whose management certificate expired (offline for longer than its
 * lifetime) is refused by the server, renewal included. It enrolls again with
 * the enrollment certificate, under a new key (a new device name): the old
 * enrollment request is approved already and cannot issue another
 * certificate. Apps, config and firmware stay. */
#define CERT_EXPIRED_GRACE_S 60
/* Counters in customInfo (fc_port_metrics, heap and traffic) are read again
 * after this long; the statuses in between repeat them. Flight Control 1.4
 * logs an event for every change in the status: with counters read at every
 * status that was one event per device at every status. */
#define METRICS_INTERVAL_S 3600
/* The first read is at the first status, before the agent has done much:
 * no traffic yet, the heap still whole. The second comes this long after. */
#define METRICS_SECOND_READ_S 300
#define CLOCK_SET_AFTER 1735689600 /* 2025-01-01: before that, the clock is not set */
/* Metrics for the Telemetry Gateway (fc_metrics.h), when the fleet gives its
 * address in METRICS_CONFIG: {"endpoint": "host:port", "intervalSeconds": N}.
 * Each export is a TLS connection of its own, beside the agent's, opened only
 * when the heap has the block a connection needs; otherwise it is skipped
 * (counted) and tried again soon. */
#define METRICS_CONFIG       "/etc/fc/metrics.json"
#define OTEL_SIGNER          "flightctl.io/device-svc-client"
#define METRICS_DEFAULT_S    60
#define METRICS_MIN_S        30   /* each export is a TLS handshake */
#define METRICS_LOOK_S       60   /* without the config, looked for again this often */
#define METRICS_TIMEOUT_S    15   /* the whole export, well under TURN_WAIT_S */
#define METRICS_BACKOFF_MAX_S 3600
#ifndef FC_METRICS_HEAP_MIN
#define FC_METRICS_HEAP_MIN (24 * 1024) /* the largest free block, for a second TLS connection */
#endif

/* Decommissioning, as the Linux agent does it: the DeviceDecommissioning
 * condition Started, then Completed, and then the device key and the
 * management certificate are wiped. Each step waits for the server to have
 * the one before (a status accepted). */
enum { DECOM_NONE, DECOM_STARTED, DECOM_COMPLETED };

typedef struct {
    const fc_agent_config_t *cfg;
    fc_identity_t id;
    char *mgmt_cert_pem;   /* guarded by lock: requests use a copy (mgmt_request) */
    time_t cert_renew_at;  /* when renewal is due */
    time_t cert_not_after;
    time_t cert_next_try;
    int cert_backoff_s;
    char csr_name[96];     /* renewal request waiting for its certificate */
    /* The kept HTTP/2 connection: the spec's long-poll and the status
     * thread's requests go over it at once, each on its stream. */
    fc_http_session_t *sess;
    char boot_id[40];

    pthread_mutex_t lock;
    pthread_cond_t status_wake;
    pthread_cond_t status_sent; /* a status update ended (turn_begin waits on it) */
    int downloading; /* under lock: the connection is closed for a pull, the status waits */
    int turn_depth;  /* under lock: pulls and holds in progress (turn_begin, turn_hold) */
    int apps_ready; /* under lock: the saved apps are loaded, the status may list them */
    int putting;    /* under lock: a status update is being built or sent */
    int applying;   /* under lock: a new spec is being applied */
    int status_urgent;      /* the agent's own change: report soon */
    int status_dirty;       /* an app or the firmware changed something */
    char rendered_version[32]; /* last version applied without errors */
    char attempted_version[32]; /* last version received (knownRenderedVersion) */
    char os_image[256];
    char os_digest[80];
    char desired_os_image[256];
    char update_error[160]; /* last apply error, empty if none */
    char update_reason[24]; /* Updating condition reason while updating */
    char failed_os_image[256];
    int verifying;          /* running a new firmware not confirmed yet */
    /* While a firmware update is in flight (download done, rebooting, new
     * image not confirmed yet) status reports the previous rendered version,
     * so Flight Control does not count the device as updated (rollout batch
     * success) before the new image has proven it can reach the server. */
    char reported_rendered[32];
    time_t verify_deadline;
    /* The Updating condition as last reported, and since when: its
     * lastTransitionTime changes only with it (status thread). */
    char cond_last[220];
    char cond_since[32];
    char *metrics;          /* counters for customInfo, as a JSON object (status thread) */
    time_t metrics_at;
    int metrics_reads;
    /* Decommissioning: requested by the spec thread, carried on by the
     * status thread. decom_reported is the step in the last status built. */
    int decom;
    int decom_reported;
    int decom_since_step;
    char decom_target[16];
    char decom_since[32];
    /* Stack never used by each thread, the lowest seen (fc_port_stack_unused;
     * -1 unknown): spec (the caller's thread, from boot) under lock, status
     * in add_metrics. */
    long stack_spec, stack_status;
    /* Metrics (status thread): the gateway's certificate and its request,
     * and when the next export is due. */
    char *otel_cert_pem;
    time_t otel_renew_at, otel_not_after, otel_next_try;
    int otel_backoff_s;
    char otel_csr_name[96];
    time_t metrics_next;
    int metrics_backoff_s;
    unsigned long metrics_skipped;
    long export_low; /* the heap's low during the last export (fc_port_heap_watch), -1 unknown */
    int exporting; /* under lock: an export runs (fc_agent_busy) */
    int metrics_poke; /* under lock: METRICS_CONFIG changed, export now */
    int console_quiet; /* spec thread: the sessions listed were all seen before */
} agent_t;

static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void rfc3339_now(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void make_boot_id(char *out)
{
    unsigned char b[16];
    fc_random(b, sizeof(b));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    sprintf(out, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void kv_load_str(const char *key, char *dst, size_t len)
{
    char *v = NULL;
    size_t n = 0;
    dst[0] = '\0';
    if (fc_port_kv_get(key, &v, &n) == 0) {
        snprintf(dst, len, "%s", v);
        free(v);
    }
}

static void kv_store_str(const char *key, const char *v)
{
    fc_port_kv_set(key, v, strlen(v));
}

static fc_http_target_t enrollment_target(const agent_t *a, int timeout_s)
{
    fc_http_target_t t = {
        .host = a->cfg->server_host,
        .port = a->cfg->server_port,
        .ca_pem = a->cfg->ca_pem,
        .cert_pem = a->cfg->enrollment_cert_pem,
        .key_pem = a->cfg->enrollment_key_pem,
        .timeout_s = timeout_s,
    };
    return t;
}

/* A request with the management certificate, on the agent's kept
 * connection. The certificate is copied: a renewal on the status thread may
 * replace and free it while a long-poll is still running. */
static int mgmt_request(agent_t *a, fc_http_session_t *sess, int timeout_s, const char *method, const char *path,
                        const char *content_type, const char *body, size_t body_len, fc_http_response_t *r)
{
    fc_http_target_t t = enrollment_target(a, timeout_s);
    pthread_mutex_lock(&a->lock);
    char *cert = strdup(a->mgmt_cert_pem);
    pthread_mutex_unlock(&a->lock);
    if (cert == NULL) {
        memset(r, 0, sizeof(*r));
        r->status = -1;
        return -1;
    }
    t.cert_pem = cert;
    t.key_pem = a->id.key_pem;
    t.hw_key = a->id.hw_key;
    t.session = sess;
    int rc = fc_http_request(&t, method, path, content_type, body, body_len, r);
    free(cert);
    return rc;
}

/* The spec's console sessions (fc_console.c), on the agent's connection.
 * Returns 1 if the spec is the one applied already: only a console made the
 * server answer, and nothing is to be applied. */
static int consoles(agent_t *a, const char *json, const char *known)
{
    cJSON *dev = cJSON_Parse(json);
    const cJSON *list = cJSON_GetObjectItem(cJSON_GetObjectItem(dev, "spec"), "consoles");
    const char *version = cJSON_GetStringValue(
        cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(dev, "metadata"), "annotations"), RENDERED_ANNOTATION));
    int same = 0;
    a->console_quiet = 0;
    if (cJSON_GetArraySize(list) > 0) {
        fc_http_target_t t = enrollment_target(a, 30);
        pthread_mutex_lock(&a->lock);
        char *cert = a->mgmt_cert_pem ? strdup(a->mgmt_cert_pem) : NULL;
        pthread_mutex_unlock(&a->lock);
        if (cert != NULL) {
            t.cert_pem = cert;
            t.key_pem = a->id.key_pem;
            t.hw_key = a->id.hw_key;
            t.session = a->sess;
            int listed = fc_console_sync(list, &t);
            a->console_quiet = listed == 1;
            same = listed && version != NULL && known[0] && strcmp(version, known) == 0;
            free(cert);
        }
    }
    cJSON_Delete(dev);
    return same;
}

/* ---- status ------------------------------------------------------------ */

/* The lower of a thread's last reading and this one: a stack's unused part
 * only shrinks, but an app's thread is a new one after each restart. */
static long stack_low(long seen, long now)
{
    return now < 0 ? seen : seen < 0 || now < seen ? now : seen;
}

/* stackMinFreeBytes: how much of each thread's stack was never used, which is
 * what can be taken from it (spec: the main task's; status:
 * FC_AGENT_STACK_SIZE). Called
 * from the status thread with the lock held. */
static void add_stacks(agent_t *a, cJSON *m)
{
    a->stack_status = stack_low(a->stack_status, fc_port_stack_unused());
    const char *names[] = {"spec", "status", "apps"};
    long v[] = {a->stack_spec, a->stack_status, fc_apps_stack_unused()};
    char text[96];
    size_t n = 0;
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        if (v[i] >= 0 && n < sizeof(text)) {
            n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s=%ld", n ? " " : "", names[i], v[i]);
        }
    }
    if (n > 0 && n < sizeof(text)) {
        cJSON_AddStringToObject(m, "stackMinFreeBytes", text);
    }
}

static cJSON *read_counters(agent_t *a, const fc_sysinfo_t *si);

/* The counters, read again when they are due and kept as text (a few hundred
 * bytes, not a cJSON tree): they go into customInfo as they were until then. */
static void add_metrics(agent_t *a, cJSON *custom, const fc_sysinfo_t *si)
{
    int interval = a->cfg->metrics_interval_s > 0 ? a->cfg->metrics_interval_s : METRICS_INTERVAL_S;
    if (a->metrics_reads == 1 && interval > METRICS_SECOND_READ_S) {
        interval = METRICS_SECOND_READ_S;
    }
    time_t now = time(NULL);
    /* A clock set after the first status moves `now` by years: read them again. */
    if (a->metrics == NULL || now - a->metrics_at >= interval || now < a->metrics_at) {
        cJSON *m = read_counters(a, si);
        char *text = m != NULL ? cJSON_PrintUnformatted(m) : NULL;
        cJSON_Delete(m);
        if (text != NULL) {
            free(a->metrics);
            a->metrics = text;
            a->metrics_at = now;
            a->metrics_reads++;
        }
    }
    cJSON *m = a->metrics != NULL ? cJSON_Parse(a->metrics) : NULL;
    if (m == NULL) {
        return;
    }
    while (m->child != NULL) {
        cJSON *item = cJSON_DetachItemViaPointer(m, m->child);
        cJSON_AddItemToObject(custom, item->string, item);
    }
    cJSON_Delete(m);
}

/* The counters, as strings in a JSON object: the heap, traffic, handshakes,
 * CPU, the port's (fc_port_metrics) and the threads' stacks. */
static cJSON *read_counters(agent_t *a, const fc_sysinfo_t *si)
{
    cJSON *m = cJSON_CreateObject();
    if (m == NULL) {
        return NULL;
    }
    char num[32];
    snprintf(num, sizeof(num), "%llu", (unsigned long long)si->mem_free);
    cJSON_AddStringToObject(m, "memFreeBytes", num);
    unsigned long long tx, rx;
    fc_http_traffic(&tx, &rx);
    snprintf(num, sizeof(num), "%llu", tx);
    cJSON_AddStringToObject(m, "netTxBytes", num);
    snprintf(num, sizeof(num), "%llu", rx);
    cJSON_AddStringToObject(m, "netRxBytes", num);
    snprintf(num, sizeof(num), "%lu", fc_http_handshakes());
    cJSON_AddStringToObject(m, "tlsHandshakes", num);
    int cpu = fc_res_cpu_percent();
    if (cpu >= 0) {
        snprintf(num, sizeof(num), "%d", cpu);
        cJSON_AddStringToObject(m, "cpuPercent", num);
    }
    fc_port_metrics(m);
    add_stacks(a, m);
    return m;
}

static cJSON *build_status(agent_t *a)
{
    fc_sysinfo_t si = {0};
    fc_port_sysinfo(&si);
    char now[32];
    rfc3339_now(now, sizeof(now));

    pthread_mutex_lock(&a->lock);
    int failed = a->update_error[0] != '\0';
    int updating = !failed && a->desired_os_image[0] && strcmp(a->desired_os_image, a->os_image) != 0;

    cJSON *st = cJSON_CreateObject();

    cJSON *conds = cJSON_AddArrayToObject(st, "conditions");
    cJSON *c = cJSON_CreateObject();
    const char *cstatus = updating ? "True" : "False";
    const char *creason = failed ? "Error" : (updating ? a->update_reason : "Updated");
    const char *cmessage = failed ? a->update_error : "";
    /* Flight Control 1.4 logs an event for any change in the status: the
     * time moves only when the condition does, as with Kubernetes'. */
    char ckey[sizeof(a->cond_last)];
    snprintf(ckey, sizeof(ckey), "%s|%s|%s", cstatus, creason, cmessage);
    if (strcmp(ckey, a->cond_last) != 0 || a->cond_since[0] == '\0') {
        snprintf(a->cond_last, sizeof(a->cond_last), "%s", ckey);
        snprintf(a->cond_since, sizeof(a->cond_since), "%s", now);
    }
    cJSON_AddStringToObject(c, "type", "Updating");
    cJSON_AddStringToObject(c, "status", cstatus);
    cJSON_AddStringToObject(c, "reason", creason);
    cJSON_AddStringToObject(c, "message", cmessage);
    cJSON_AddStringToObject(c, "lastTransitionTime", a->cond_since);
    cJSON_AddItemToArray(conds, c);

    a->decom_reported = a->decom;
    if (a->decom != DECOM_NONE) {
        if (a->decom_since_step != a->decom) {
            a->decom_since_step = a->decom;
            snprintf(a->decom_since, sizeof(a->decom_since), "%s", now);
        }
        int done = a->decom == DECOM_COMPLETED;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "type", "DeviceDecommissioning");
        cJSON_AddStringToObject(d, "status", "True");
        cJSON_AddStringToObject(d, "reason", done ? "Completed" : "Started");
        cJSON_AddStringToObject(d, "message", done ? "Device completed decommissioning and will wipe its management "
                                                     "certificate"
                                                   : "Device started decommissioning");
        cJSON_AddStringToObject(d, "lastTransitionTime", a->decom_since);
        cJSON_AddItemToArray(conds, d);
    }

    cJSON *sys = cJSON_AddObjectToObject(st, "systemInfo");
    cJSON_AddStringToObject(sys, "architecture", si.architecture);
    cJSON_AddStringToObject(sys, "operatingSystem", si.operating_system);
    cJSON_AddStringToObject(sys, "bootID", a->boot_id);
    cJSON_AddStringToObject(sys, "agentVersion", a->cfg->agent_version);
    cJSON *custom = cJSON_AddObjectToObject(sys, "customInfo");
    /* Every status carries all of this (Flight Control replaces the whole
     * status), so only what is worth monitoring: the chip and the agent are
     * already in systemInfo, the labels and agentVersion. Each change in the
     * status is an event in Flight Control 1.4: what changes all the time
     * is in add_metrics. */
    cJSON_AddStringToObject(custom, "osVersion", si.os_version);
    add_metrics(a, custom, &si);
    if (a->cert_not_after > 0) {
        char exp[32];
        struct tm tm;
        gmtime_r(&a->cert_not_after, &tm);
        strftime(exp, sizeof(exp), "%Y-%m-%dT%H:%M:%SZ", &tm);
        cJSON_AddStringToObject(custom, "mgmtCertNotAfter", exp);
    }
    fc_port_custom_info(custom);
    if (a->cfg->custom_info != NULL) {
        a->cfg->custom_info(custom, a->cfg->app_ctx);
    }

    fc_apps_status(cJSON_AddArrayToObject(st, "applications"), cJSON_AddObjectToObject(st, "applicationsSummary"),
                   custom);

    char res_info[80];
    int res_level = fc_res_status(cJSON_AddObjectToObject(st, "resources"), res_info, sizeof(res_info));

    cJSON *integ = cJSON_AddObjectToObject(st, "integrity");
    cJSON_AddStringToObject(integ, "status", "Unsupported");

    cJSON *cfg = cJSON_AddObjectToObject(st, "config");
    cJSON_AddStringToObject(cfg, "renderedVersion",
                            a->reported_rendered[0] ? a->reported_rendered : a->rendered_version);

    cJSON *os = cJSON_AddObjectToObject(st, "os");
    cJSON_AddStringToObject(os, "image", a->os_image);
    cJSON_AddStringToObject(os, "imageDigest", a->os_digest);
    const char *why;
    const char *outcome = fc_oci_delta_outcome(&why);
    if (outcome[0] != '\0') {
        /* how the last firmware came: as a delta, or whole and why */
        cJSON *delta = cJSON_AddObjectToObject(os, "lastDelta");
        cJSON_AddStringToObject(delta, "outcome", outcome);
        if (why[0] != '\0') {
            cJSON_AddStringToObject(delta, "fallbackReason", why);
        }
    }

    cJSON *upd = cJSON_AddObjectToObject(st, "updated");
    cJSON_AddStringToObject(upd, "status", failed ? "OutOfDate" : (updating ? "Updating" : "UpToDate"));
    if (failed) {
        cJSON_AddStringToObject(upd, "info", a->update_error);
    }

    /* A critical resource alert first, then a failed update, then a warning. */
    cJSON *sum = cJSON_AddObjectToObject(st, "summary");
    if (res_level == 2 || (res_level == 1 && !failed)) {
        cJSON_AddStringToObject(sum, "status", res_level == 2 ? "Error" : "Degraded");
        cJSON_AddStringToObject(sum, "info", res_info);
    } else {
        cJSON_AddStringToObject(sum, "status", failed ? "Degraded" : "Online");
        cJSON_AddStringToObject(sum, "info", failed ? a->update_error : "Device is running");
    }

    cJSON *life = cJSON_AddObjectToObject(st, "lifecycle");
    /* The server sets Decommissioning when asked, and keeps the lifecycle
     * the agent reports: Enrolled here would undo it until Started. */
    cJSON_AddStringToObject(life, "status", a->decom != DECOM_NONE ? "Decommissioning" : "Enrolled");
    pthread_mutex_unlock(&a->lock);
    return st;
}

static void request_status(agent_t *a);

/* ---- decommissioning ----------------------------------------------------- */

/* What makes this device known to Flight Control. Apps, config files and the
 * firmware stay: Unenroll leaves the device running, unmanaged. */
static void decommission_wipe(void)
{
    fc_port_kv_del(KV_MGMT_CERT);
    fc_port_kv_del(KV_OTEL_CERT);
    fc_port_kv_del(KV_RENDERED);
    fc_port_kv_del(KV_PREV_RENDERED);
    fc_port_kv_del(KV_OS_PENDING);
    fc_port_kv_del(KV_OS_PENDING_DIGEST);
    fc_identity_forget();
}

/* Spec thread: the rendered spec asks for it. Nothing else in that spec is
 * applied (the server has taken the device out of its fleet, and the spec
 * would remove its apps). */
static void request_decommission(agent_t *a, const cJSON *decom)
{
    const char *target = cJSON_GetStringValue(cJSON_GetObjectItem(decom, "target"));
    pthread_mutex_lock(&a->lock);
    int first = a->decom == DECOM_NONE;
    if (first) {
        /* Unenroll is the one the CLI and the UI send; FactoryReset also
         * removes the apps. Anything else is taken as Unenroll. */
        snprintf(a->decom_target, sizeof(a->decom_target), "%s",
                 target != NULL && strcmp(target, "FactoryReset") == 0 ? "FactoryReset" : "Unenroll");
        a->decom = DECOM_STARTED;
    }
    pthread_mutex_unlock(&a->lock);
    if (first) {
        fc_port_log(FC_LOG_WARN, "decommissioning requested (%s): the device key and the management certificate "
                                 "will be wiped", a->decom_target);
        request_status(a);
    }
}

/* Status thread, after a status was accepted: the step it carried is known to
 * the server, go on to the next. A failure leaves the step as it is, to be
 * done again after the next status. */
static void decommission_step(agent_t *a)
{
    if (a->decom_reported == DECOM_STARTED) {
        if (strcmp(a->decom_target, "FactoryReset") == 0) {
            cJSON *none = cJSON_CreateArray();
            char err[160];
            int retry;
            fc_apps_apply(none, err, sizeof(err), &retry);
            cJSON_Delete(none);
        }
        pthread_mutex_lock(&a->lock);
        a->decom = DECOM_COMPLETED;
        pthread_mutex_unlock(&a->lock);
        request_status(a);
    } else if (a->decom_reported == DECOM_COMPLETED) {
        /* Only now: a restart before the server has Completed must not leave
         * it at Decommissioning for good. The spec asks again then. */
        if (fc_port_kv_set(KV_DECOMMISSIONED, a->decom_target, strlen(a->decom_target)) != 0) {
            fc_port_log(FC_LOG_WARN, "decommissioning: cannot store it, trying again");
            return;
        }
        fc_port_log(FC_LOG_WARN, "decommissioned: wiping the device key and the management certificate");
        decommission_wipe();
        fc_port_reboot();
    }
}

/* At boot: a device that was decommissioned does not enroll again. Its
 * storage has to be erased first (it holds the marker). */
static int decommissioned_at_boot(void)
{
    char target[16] = "";
    kv_load_str(KV_DECOMMISSIONED, target, sizeof(target));
    if (target[0] == '\0') {
        return 0;
    }
    decommission_wipe(); /* again: a reboot may have cut it short */
    fc_port_log(FC_LOG_WARN, "decommissioned (%s): not enrolling. To enroll it again, erase its storage", target);
    return 1;
}

static int put_status(agent_t *a)
{
    cJSON *dev = cJSON_CreateObject();
    cJSON_AddStringToObject(dev, "apiVersion", "v1beta1");
    cJSON_AddStringToObject(dev, "kind", "Device");
    cJSON *meta = cJSON_AddObjectToObject(dev, "metadata");
    cJSON_AddStringToObject(meta, "name", a->id.name);
    cJSON_AddItemToObject(dev, "status", build_status(a));
    char *body = cJSON_PrintUnformatted(dev);
    cJSON_Delete(dev);
    if (body == NULL) {
        /* out of memory: a missing node is skipped by cJSON, the print fails */
        fc_port_log(FC_LOG_WARN, "status update: out of memory");
        return -1;
    }

    char path[128];
    snprintf(path, sizeof(path), API "/devices/%s/status", a->id.name);
    fc_http_response_t r;
    int rc = mgmt_request(a, a->sess, 60, "PUT", path, "application/json", body, strlen(body), &r);
    free(body);
    if (rc != 0) {
        return -1;
    }
    if (r.status != 200) {
        fc_port_log(FC_LOG_WARN, "status update: HTTP %d %s", r.status, r.body);
        fc_http_free(&r);
        return -1;
    }
    fc_http_free(&r);
    fc_port_log(FC_LOG_DEBUG, "status updated");
    if (a->verifying) {
        /* The new firmware reached Flight Control: keep it. */
        fc_port_ota_confirm();
        kv_store_str(KV_OS_IMAGE, a->os_image);
        kv_store_str(KV_OS_DIGEST, a->os_digest);
        fc_port_kv_del(KV_OS_PENDING);
        fc_port_kv_del(KV_OS_PENDING_DIGEST);
        fc_port_kv_del(KV_PREV_RENDERED);
        pthread_mutex_lock(&a->lock);
        a->reported_rendered[0] = '\0';
        pthread_mutex_unlock(&a->lock);
        a->verifying = 0;
        fc_port_log(FC_LOG_INFO, "firmware %s confirmed", a->os_image);
        request_status(a); /* now report the new rendered version */
    }
    decommission_step(a);
    return 0;
}

static void wake_status(agent_t *a, int urgent)
{
    pthread_mutex_lock(&a->lock);
    if (urgent) {
        a->status_urgent = 1;
    } else {
        a->status_dirty = 1;
    }
    pthread_cond_signal(&a->status_wake);
    pthread_mutex_unlock(&a->lock);
}

static void request_status(agent_t *a)
{
    wake_status(a, 1);
}

/* One TLS connection at a time (fc_tls_pool.h): a pull from a registry
 * (fc_oci_set_turn, on the spec thread, where no long-poll is running then)
 * takes the turn of the agent's connection. A status on its way, or already
 * asked for ("Downloading"), goes out first, and a metrics export ends; then the connection is closed,
 * and the status thread waits until the pull is over. Pulls nest in a hold
 * (turn_hold): the connection opens again when the last of them ends. */
static void turn_begin(void *ctx)
{
    agent_t *a = ctx;
    pthread_mutex_lock(&a->lock);
    a->turn_depth++;
    if (a->downloading) {
        pthread_mutex_unlock(&a->lock);
        return; /* closed already, for this hold */
    }
    struct timespec until = {.tv_sec = time(NULL) + TURN_WAIT_S};
    while ((a->putting || a->status_urgent || a->exporting) && time(NULL) < until.tv_sec) {
        pthread_cond_timedwait(&a->status_sent, &a->lock, &until);
    }
    a->downloading = 1;
    pthread_mutex_unlock(&a->lock);
    fc_console_stop(); /* its stream would keep the connection open */
    fc_http_session_close(a->sess);
}

/* The pull's connection is closed: once no pull or hold is left, the
 * agent's opens again with the next request, a status reporting what was
 * done. */
static void turn_end(void *ctx)
{
    agent_t *a = ctx;
    pthread_mutex_lock(&a->lock);
    if (a->turn_depth > 0 && --a->turn_depth == 0 && a->downloading) {
        a->downloading = 0;
        a->status_urgent = 1;
        pthread_cond_signal(&a->status_wake);
    }
    pthread_mutex_unlock(&a->lock);
}

/* While the apps are applied: a pull among them keeps the connection closed
 * until the new apps have made their instances (fc_apps waits for them),
 * whose memory then comes before what the reopened connection holds. Ended
 * with turn_end; closes nothing when there is no pull. */
static void turn_hold(agent_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->turn_depth++;
    pthread_mutex_unlock(&a->lock);
}

static agent_t *s_running;

int fc_agent_busy(void)
{
    agent_t *a = s_running;
    if (a == NULL) {
        /* Not started yet: at boot it is about to connect, and its first
         * TLS handshake needs a 16.7 KB block, which a classic ESP32 did not
         * have with CoreMark's 64 KB taken. */
        return 1;
    }
    pthread_mutex_lock(&a->lock);
    int busy = a->status_urgent || a->status_dirty || a->putting || a->applying || a->exporting;
    pthread_mutex_unlock(&a->lock);
    return busy;
}

void fc_agent_notify_status(void)
{
    if (s_running != NULL) {
        wake_status(s_running, 0);
    }
}

/* ---- management certificate renewal ------------------------------------ */

/* UTC broken-down time to epoch, without timegm() (not in every libc). */
static time_t x509_epoch(const mbedtls_x509_time *t)
{
    int y = t->year - (t->mon <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153L * (t->mon + (t->mon > 2 ? -3 : 9)) + 2) / 5 + t->day - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    return (time_t)days * 86400 + t->hour * 3600 + t->min * 60 + t->sec;
}

/* Checks that a PEM certificate is for this device's key, and returns when
 * its renewal is due and when it expires. */
static int cert_info(const agent_t *a, const char *pem, time_t *renew_at, time_t *not_after)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    char name[64];
    int rc = -1;
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0 &&
        fc_identity_name_of(&crt.pk, name, sizeof(name)) == 0 && strcmp(name, a->id.name) == 0) {
        time_t nb = x509_epoch(&crt.valid_from), na = x509_epoch(&crt.valid_to);
        *not_after = na;
        *renew_at = nb + (na - nb) * CERT_RENEW_AT_PERCENT / 100;
        rc = 0;
    }
    mbedtls_x509_crt_free(&crt);
    return rc;
}

static void log_cert(const char *what, time_t renew_at, time_t not_after)
{
    char na[32], ra[32];
    struct tm tm;
    gmtime_r(&not_after, &tm);
    strftime(na, sizeof(na), "%Y-%m-%dT%H:%M:%SZ", &tm);
    gmtime_r(&renew_at, &tm);
    strftime(ra, sizeof(ra), "%Y-%m-%dT%H:%M:%SZ", &tm);
    fc_port_log(FC_LOG_INFO, "%s: valid until %s, renewal from %s", what, na, ra);
}

/* Reads a CertificateSigningRequest answer. Returns 1 if a new certificate
 * was installed, 0 if still pending, -1 if the request failed. */
static int handle_csr(agent_t *a, const char *json)
{
    cJSON *csr = cJSON_Parse(json);
    const cJSON *status = cJSON_GetObjectItem(csr, "status");
    const cJSON *cond;
    int result = 0;
    cJSON_ArrayForEach(cond, cJSON_GetObjectItem(status, "conditions")) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "type"));
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "status"));
        if (type && st && strcmp(st, "True") == 0 && (strcmp(type, "Denied") == 0 || strcmp(type, "Failed") == 0)) {
            fc_port_log(FC_LOG_WARN, "certificate renewal %s %s: %s", a->csr_name, type,
                        cJSON_GetStringValue(cJSON_GetObjectItem(cond, "message")));
            result = -1;
        }
    }
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(status, "certificate"));
    if (result == 0 && b64 != NULL && b64[0]) {
        size_t len = strlen(b64), olen = 0;
        unsigned char *pem = malloc(len + 1);
        time_t renew_at, not_after;
        if (pem == NULL ||
            mbedtls_base64_decode(pem, len, &olen, (const unsigned char *)b64, len) != 0) {
            result = -1;
        } else {
            pem[olen] = '\0';
            if (cert_info(a, (char *)pem, &renew_at, &not_after) != 0) {
                fc_port_log(FC_LOG_WARN, "certificate renewal %s: certificate is not for this device", a->csr_name);
                result = -1;
            } else {
                /* Stored first: after a reboot the device starts with it. */
                if (fc_port_kv_set(KV_MGMT_CERT, (char *)pem, olen) != 0) {
                    fc_port_log(FC_LOG_WARN, "could not store the renewed certificate");
                }
                pthread_mutex_lock(&a->lock);
                free(a->mgmt_cert_pem);
                a->mgmt_cert_pem = (char *)pem;
                pem = NULL;
                a->cert_renew_at = renew_at;
                a->cert_not_after = not_after;
                pthread_mutex_unlock(&a->lock);
                log_cert("management certificate renewed", renew_at, not_after);
                result = 1;
            }
        }
        free(pem);
    }
    cJSON_Delete(csr);
    return result;
}

/* Creates the renewal request, or polls the one already created. */
static int request_renewal(agent_t *a)
{
    fc_http_response_t r; /* requests on the status thread's connection */
    int rc;
    if (a->csr_name[0] == '\0') {
        unsigned char rnd[4];
        fc_random(rnd, sizeof(rnd));
        snprintf(a->csr_name, sizeof(a->csr_name), "%s-%02x%02x%02x%02x", a->id.name, rnd[0], rnd[1], rnd[2],
                 rnd[3]);
        size_t plen = strlen(a->id.csr_pem), blen = 0;
        mbedtls_base64_encode(NULL, 0, &blen, (const unsigned char *)a->id.csr_pem, plen);
        char *b64 = malloc(blen);
        if (b64 == NULL ||
            mbedtls_base64_encode((unsigned char *)b64, blen, &blen, (const unsigned char *)a->id.csr_pem, plen) != 0) {
            free(b64);
            a->csr_name[0] = '\0';
            return -1;
        }
        cJSON *csr = cJSON_CreateObject();
        cJSON_AddStringToObject(csr, "apiVersion", "v1beta1");
        cJSON_AddStringToObject(csr, "kind", "CertificateSigningRequest");
        cJSON_AddStringToObject(cJSON_AddObjectToObject(csr, "metadata"), "name", a->csr_name);
        cJSON *spec = cJSON_AddObjectToObject(csr, "spec");
        cJSON_AddStringToObject(spec, "request", b64);
        cJSON_AddStringToObject(spec, "signerName", RENEWAL_SIGNER);
        cJSON *usages = cJSON_AddArrayToObject(spec, "usages");
        cJSON_AddItemToArray(usages, cJSON_CreateString("clientAuth"));
        cJSON_AddItemToArray(usages, cJSON_CreateString("CA:false"));
        char *body = cJSON_PrintUnformatted(csr);
        cJSON_Delete(csr);
        free(b64);
        if (body == NULL) {
            fc_port_log(FC_LOG_WARN, "certificate renewal: out of memory");
            a->csr_name[0] = '\0';
            return -1;
        }
        fc_port_log(FC_LOG_INFO, "renewing the management certificate (%s)", a->csr_name);
        rc = mgmt_request(a, a->sess, 60, "POST", API "/certificatesigningrequests", "application/json", body,
                          strlen(body), &r);
        free(body);
        if (rc != 0) {
            a->csr_name[0] = '\0';
            return -1;
        }
        if (r.status != 200 && r.status != 201) {
            fc_port_log(FC_LOG_WARN, "certificate renewal: HTTP %d %s", r.status, r.body);
            fc_http_free(&r);
            a->csr_name[0] = '\0';
            return -1;
        }
    } else {
        char path[160];
        snprintf(path, sizeof(path), API "/certificatesigningrequests/%s", a->csr_name);
        if (mgmt_request(a, a->sess, 60, "GET", path, NULL, NULL, 0, &r) != 0) {
            return 0; /* network trouble: poll again */
        }
        if (r.status != 200) {
            fc_port_log(FC_LOG_WARN, "certificate renewal %s: HTTP %d", a->csr_name, r.status);
            fc_http_free(&r);
            a->csr_name[0] = '\0';
            return -1;
        }
    }
    rc = handle_csr(a, r.body);
    fc_http_free(&r);
    if (rc != 0) {
        a->csr_name[0] = '\0';
    }
    return rc;
}

/* After a failed status update: with an expired certificate nothing will
 * ever succeed again, so start over as a new device. */
static void reenroll_if_expired(agent_t *a)
{
    time_t now = time(NULL);
    if (a->cert_not_after == 0 || now < CLOCK_SET_AFTER || now < a->cert_not_after + CERT_EXPIRED_GRACE_S) {
        return;
    }
    char exp[32];
    struct tm tm;
    gmtime_r(&a->cert_not_after, &tm);
    strftime(exp, sizeof(exp), "%Y-%m-%dT%H:%M:%SZ", &tm);
    fc_port_log(FC_LOG_ERROR, "management certificate expired on %s: enrolling again as a new device", exp);
    fc_port_kv_del(KV_MGMT_CERT);
    fc_port_kv_del(KV_OTEL_CERT);
    fc_port_kv_del(KV_RENDERED);
    fc_port_kv_del(KV_PREV_RENDERED);
    fc_identity_forget();
    fc_port_reboot();
}

/* Called from the status thread on every round. */
static void renew_cert_if_due(agent_t *a)
{
    time_t now = time(NULL);
    if (a->cert_renew_at == 0 || now < a->cert_renew_at || now < a->cert_next_try) {
        return;
    }
    int rc = request_renewal(a);
    if (rc == 0) {
        a->cert_next_try = now + RETRY_DELAY_S;
    } else if (rc < 0) {
        a->cert_backoff_s = a->cert_backoff_s ? a->cert_backoff_s * 2 : CERT_RETRY_MIN_S;
        if (a->cert_backoff_s > CERT_RETRY_MAX_S) {
            a->cert_backoff_s = CERT_RETRY_MAX_S;
        }
        a->cert_next_try = now + a->cert_backoff_s;
        fc_port_log(FC_LOG_WARN, "certificate renewal failed, next attempt in %d s", a->cert_backoff_s);
    } else {
        a->cert_backoff_s = 0;
        a->cert_next_try = 0;
    }
}

/* ---- metrics for the Telemetry Gateway (fc_metrics.h) ---------------------- */

typedef struct {
    char host[128];
    int port;
    int interval_s;
} metrics_cfg_t;

/* METRICS_CONFIG, as the fleet wrote it: 0, or -1 when there is none (or
 * it is not usable: said once per change). */
static int metrics_config(metrics_cfg_t *c)
{
    static char s_bad[96];
    char *text = NULL;
    size_t len = 0;
    if (fc_port_read_file(METRICS_CONFIG, &text, &len) != 0) {
        s_bad[0] = '\0';
        return -1;
    }
    cJSON *j = cJSON_Parse(text);
    free(text);
    const char *ep = cJSON_GetStringValue(cJSON_GetObjectItem(j, "endpoint"));
    const cJSON *iv = cJSON_GetObjectItem(j, "intervalSeconds");
    int rc = -1;
    if (ep != NULL) {
        const char *h = strstr(ep, "://");
        h = h ? h + 3 : ep;
        const char *colon = strrchr(h, ':');
        size_t hl = colon ? (size_t)(colon - h) : strlen(h);
        c->port = colon ? atoi(colon + 1) : 443;
        if (hl > 0 && hl < sizeof(c->host) && c->port > 0 && c->port < 65536) {
            snprintf(c->host, sizeof(c->host), "%.*s", (int)hl, h);
            c->interval_s = cJSON_IsNumber(iv) ? (int)iv->valuedouble : METRICS_DEFAULT_S;
            c->interval_s = c->interval_s < METRICS_MIN_S ? METRICS_MIN_S : c->interval_s;
            rc = 0;
        }
    }
    if (rc != 0 && strcmp(s_bad, ep ? ep : "-") != 0) {
        snprintf(s_bad, sizeof(s_bad), "%s", ep ? ep : "-");
        fc_port_log(FC_LOG_WARN, "metrics: %s has no usable endpoint (\"host:port\"): nothing is sent", METRICS_CONFIG);
    }
    cJSON_Delete(j);
    return rc;
}

/* The gateway's certificate: for this device's key, with the CN the gateway
 * takes the device's ID from. 0 when it is, and sets when it is renewed. */
static int otel_cert_ok(agent_t *a, const char *pem, time_t *renew_at, time_t *not_after)
{
    if (cert_info(a, pem, renew_at, not_after) != 0) {
        return -1;
    }
    char cn[80];
    snprintf(cn, sizeof(cn), "CN=otel-%s", a->id.name);
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    char subject[128] = "";
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0) {
        mbedtls_x509_dn_gets(subject, sizeof(subject), &crt.subject);
    }
    mbedtls_x509_crt_free(&crt);
    return strcmp(subject, cn) == 0 ? 0 : -1;
}

/* The gateway's CertificateSigningRequest as the server answers it: 1 once
 * its certificate is stored (under KV_OTEL_CERT only), 0 while pending, -1
 * when it failed or the certificate is not this device's for the gateway. */
static int handle_otel_csr(agent_t *a, const char *json)
{
    cJSON *csr = cJSON_Parse(json);
    const cJSON *status = cJSON_GetObjectItem(csr, "status"), *cond;
    int result = 0;
    cJSON_ArrayForEach(cond, cJSON_GetObjectItem(status, "conditions")) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "type"));
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "status"));
        if (type && st && strcmp(st, "True") == 0 && (strcmp(type, "Denied") == 0 || strcmp(type, "Failed") == 0)) {
            fc_port_log(FC_LOG_WARN, "metrics: certificate request %s %s: %s", a->otel_csr_name, type,
                        cJSON_GetStringValue(cJSON_GetObjectItem(cond, "message")));
            result = -1;
        }
    }
    const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(status, "certificate"));
    if (result == 0 && b64 != NULL && b64[0]) {
        size_t len = strlen(b64), olen = 0;
        unsigned char *pem = malloc(len + 1);
        time_t renew_at, not_after;
        if (pem == NULL || mbedtls_base64_decode(pem, len, &olen, (const unsigned char *)b64, len) != 0) {
            result = -1;
        } else {
            pem[olen] = '\0';
            if (otel_cert_ok(a, (char *)pem, &renew_at, &not_after) != 0) {
                fc_port_log(FC_LOG_WARN, "metrics: the certificate is not this device's for the gateway");
                result = -1;
            } else {
                fc_port_kv_set(KV_OTEL_CERT, (char *)pem, olen);
                free(a->otel_cert_pem);
                a->otel_cert_pem = (char *)pem;
                pem = NULL;
                a->otel_renew_at = renew_at;
                a->otel_not_after = not_after;
                log_cert("Telemetry Gateway certificate", renew_at, not_after);
                result = 1;
            }
        }
        free(pem);
    }
    cJSON_Delete(csr);
    return result;
}

/* Asks for the gateway's certificate, or polls the request: 1 once it is
 * stored, 0 while pending, -1 when it failed. Its own request, its own key in
 * storage: the management certificate is never touched. */
static int request_otel_cert(agent_t *a)
{
    fc_http_response_t r;
    if (a->otel_csr_name[0] == '\0') {
        char cn[80], *csr_pem = NULL;
        snprintf(cn, sizeof(cn), "otel-%s", a->id.name);
        if (fc_identity_csr(&a->id, cn, &csr_pem) != 0) {
            fc_port_log(FC_LOG_WARN, "metrics: no certificate request with the device key");
            return -1;
        }
        unsigned char rnd[4];
        fc_random(rnd, sizeof(rnd));
        snprintf(a->otel_csr_name, sizeof(a->otel_csr_name), "%s-%02x%02x%02x%02x", cn, rnd[0], rnd[1], rnd[2], rnd[3]);
        size_t plen = strlen(csr_pem), blen = 0;
        mbedtls_base64_encode(NULL, 0, &blen, (const unsigned char *)csr_pem, plen);
        char *b64 = malloc(blen);
        cJSON *csr = cJSON_CreateObject();
        char *body = NULL;
        if (b64 != NULL && csr != NULL &&
            mbedtls_base64_encode((unsigned char *)b64, blen, &blen, (const unsigned char *)csr_pem, plen) == 0) {
            cJSON_AddStringToObject(csr, "apiVersion", "v1beta1");
            cJSON_AddStringToObject(csr, "kind", "CertificateSigningRequest");
            cJSON_AddStringToObject(cJSON_AddObjectToObject(csr, "metadata"), "name", a->otel_csr_name);
            cJSON *spec = cJSON_AddObjectToObject(csr, "spec");
            cJSON_AddStringToObject(spec, "request", b64);
            cJSON_AddStringToObject(spec, "signerName", OTEL_SIGNER);
            cJSON *usages = cJSON_AddArrayToObject(spec, "usages");
            cJSON_AddItemToArray(usages, cJSON_CreateString("clientAuth"));
            cJSON_AddItemToArray(usages, cJSON_CreateString("CA:false"));
            body = cJSON_PrintUnformatted(csr);
        }
        cJSON_Delete(csr);
        free(b64);
        free(csr_pem);
        if (body == NULL) {
            a->otel_csr_name[0] = '\0';
            return -1;
        }
        fc_port_log(FC_LOG_INFO, "metrics: asking for the Telemetry Gateway's certificate (%s)", a->otel_csr_name);
        int rc = mgmt_request(a, a->sess, 60, "POST", API "/certificatesigningrequests", "application/json", body,
                              strlen(body), &r);
        free(body);
        if (rc != 0 || (r.status != 200 && r.status != 201)) {
            if (rc == 0) {
                fc_port_log(FC_LOG_WARN, "metrics: certificate request: HTTP %d %s", r.status, r.body);
                fc_http_free(&r);
            }
            a->otel_csr_name[0] = '\0';
            return -1;
        }
    } else {
        char path[200];
        snprintf(path, sizeof(path), API "/certificatesigningrequests/%s", a->otel_csr_name);
        if (mgmt_request(a, a->sess, 60, "GET", path, NULL, NULL, 0, &r) != 0) {
            return 0;
        }
        if (r.status != 200) {
            fc_port_log(FC_LOG_WARN, "metrics: certificate request %s: HTTP %d", a->otel_csr_name, r.status);
            fc_http_free(&r);
            a->otel_csr_name[0] = '\0';
            return -1;
        }
    }
    int result = handle_otel_csr(a, r.body);
    fc_http_free(&r);
    if (result != 0) {
        a->otel_csr_name[0] = '\0';
    }
    return result;
}

/* The certificate, from storage or asked for: 0 when there is one to use. */
static int otel_cert(agent_t *a, time_t now)
{
    if (a->otel_cert_pem == NULL) {
        char *pem = NULL;
        size_t len = 0;
        if (fc_port_kv_get(KV_OTEL_CERT, &pem, &len) == 0 && pem != NULL) {
            if (otel_cert_ok(a, pem, &a->otel_renew_at, &a->otel_not_after) == 0) {
                a->otel_cert_pem = pem;
            } else {
                free(pem); /* an older key's: asked for again */
            }
        }
    }
    int due = a->otel_cert_pem == NULL || now >= a->otel_renew_at;
    if (due && now >= a->otel_next_try) {
        int rc = request_otel_cert(a);
        if (rc == 0) {
            a->otel_next_try = now + RETRY_DELAY_S;
        } else if (rc < 0) {
            a->otel_backoff_s = a->otel_backoff_s ? a->otel_backoff_s * 2 : CERT_RETRY_MIN_S;
            a->otel_backoff_s = a->otel_backoff_s > CERT_RETRY_MAX_S ? CERT_RETRY_MAX_S : a->otel_backoff_s;
            a->otel_next_try = now + a->otel_backoff_s;
        } else {
            a->otel_backoff_s = 0;
            a->otel_next_try = 0;
        }
    }
    return a->otel_cert_pem != NULL && now < a->otel_not_after ? 0 : -1;
}

/* What the counters (read_counters) are as OpenTelemetry metrics. */
static const struct {
    const char *key, *name, *unit;
    int sum;
    const char *attr_key, *attr_val;
} s_device_metrics[] = {
    {"memFreeBytes", "device.memory.free", "By", 0, NULL, NULL},
    {"memMinFreeBytes", "device.memory.min_free", "By", 0, NULL, NULL},
    {"memInternalFreeBytes", "device.memory.internal.free", "By", 0, NULL, NULL},
    {"memInternalLargestBlock", "device.memory.internal.largest_block", "By", 0, NULL, NULL},
    {"memKernelFreeBytes", "device.memory.kernel.free", "By", 0, NULL, NULL},
    {"memKernelMinFreeBytes", "device.memory.kernel.min_free", "By", 0, NULL, NULL},
    {"netTxBytes", "device.network.io", "By", 1, "direction", "transmit"},
    {"netRxBytes", "device.network.io", "By", 1, "direction", "receive"},
    {"tlsHandshakes", "device.tls.handshakes", "{handshake}", 1, NULL, NULL},
    {"cpuPercent", "device.cpu.utilization", "%", 0, NULL, NULL},
    {"uptimeSeconds", "device.uptime", "s", 0, NULL, NULL},
    {"wifiRssi", "device.wifi.rssi", "dBm", 0, NULL, NULL},
    {"temperatureC", "device.temperature", "Cel", 0, NULL, NULL},
};
#define DEVICE_POINTS (sizeof(s_device_metrics) / sizeof(s_device_metrics[0]) + 2)

/* The device's points, from the counters: how many. */
static int device_points(agent_t *a, fc_metric_point_t *pts, uint64_t boot_ns)
{
    fc_sysinfo_t si = {0};
    fc_port_sysinfo(&si);
    cJSON *m = read_counters(a, &si);
    int n = 0;
    for (size_t i = 0; m != NULL && i < sizeof(s_device_metrics) / sizeof(s_device_metrics[0]); i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(m, s_device_metrics[i].key));
        char *end = NULL;
        if (v == NULL || v[0] == '\0') {
            continue;
        }
        double d = strtod(v, &end);
        if (end == v || *end != '\0') {
            continue;
        }
        fc_metric_point_t *p = &pts[n++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", s_device_metrics[i].name);
        p->unit = s_device_metrics[i].unit;
        p->sum = s_device_metrics[i].sum;
        p->is_int = strchr(v, '.') == NULL && strchr(v, 'e') == NULL;
        p->i = strtoll(v, NULL, 10);
        p->d = d;
        p->attr_key = s_device_metrics[i].attr_key;
        p->attr_val = s_device_metrics[i].attr_val;
        p->start_ns = boot_ns;
    }
    cJSON_Delete(m);
    /* The internal heap's low during the last export, where the port tells. */
    if (a->export_low >= 0) {
        fc_metric_point_t *q = &pts[n++];
        memset(q, 0, sizeof(*q));
        snprintf(q->name, sizeof(q->name), "device.metrics.export_min_free");
        q->unit = "By";
        q->is_int = 1;
        q->i = a->export_low;
    }
    /* The exports skipped for want of memory, or while the agent was busy. */
    fc_metric_point_t *p = &pts[n++];
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "device.metrics.skipped");
    p->unit = "{export}";
    p->sum = 1;
    p->is_int = 1;
    p->i = (long long)a->metrics_skipped;
    p->start_ns = boot_ns;
    return n;
}

/* The tag of the firmware's image ("v3.13"), or "". */
static const char *fw_version(const agent_t *a)
{
    const char *slash = strrchr(a->os_image, '/');
    const char *colon = strrchr(slash ? slash : a->os_image, ':');
    return colon ? colon + 1 : "";
}

/* The export is over: a pull waiting for it (turn_begin) goes on. */
static void metrics_done(agent_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->exporting = 0;
    pthread_cond_broadcast(&a->status_sent);
    pthread_mutex_unlock(&a->lock);
}

/* Status thread: an export when one is due. Sets metrics_next. */
static void metrics_tick(agent_t *a)
{
    time_t now = time(NULL);
    metrics_cfg_t mc;
    if (metrics_config(&mc) != 0) {
        a->metrics_next = now + METRICS_LOOK_S;
        return;
    }
    if (now < CLOCK_SET_AFTER) {
        a->metrics_next = now + RETRY_DELAY_S;
        return;
    }
    /* Nothing on the network beside a pull (which waits for an export, as
     * for a status: turn_begin), a firmware being verified, a status due or
     * a spec being applied. */
    pthread_mutex_lock(&a->lock);
    int busy = a->downloading || a->turn_depth > 0 || a->verifying || a->status_urgent || a->applying ||
               a->decom != DECOM_NONE;
    a->exporting = !busy;
    pthread_mutex_unlock(&a->lock);
    if (!busy && otel_cert(a, now) != 0) {
        metrics_done(a);
        a->metrics_next = a->otel_next_try > now ? a->otel_next_try : now + RETRY_DELAY_S;
        return;
    }
    /* And only with the block a TLS connection takes. */
    long largest = fc_port_heap_largest();
    if (!busy && largest >= 0 && largest < FC_METRICS_HEAP_MIN) {
        if (a->metrics_skipped % 10 == 0) {
            fc_port_log(FC_LOG_WARN, "metrics: largest free block %ld bytes, under %d: export skipped", largest,
                        FC_METRICS_HEAP_MIN);
        }
        busy = 1;
    }
    if (busy) {
        metrics_done(a);
        a->metrics_skipped++;
        a->metrics_next = now + RETRY_DELAY_S;
        return;
    }
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    uint64_t now_ns = (uint64_t)now * 1000000000ull, boot_ns = (uint64_t)(now - mono.tv_sec) * 1000000000ull;
    int max = (int)DEVICE_POINTS + FC_METRICS_APPS_MAX;
    fc_metric_point_t *pts = malloc((size_t)max * sizeof(*pts));
    uint8_t *msg = NULL;
    long len = -1;
    if (pts != NULL) {
        int n = device_points(a, pts, boot_ns);
        n += fc_metrics_app_points(pts + n, max - n);
        size_t cap = 512 + (size_t)n * 160;
        msg = malloc(cap);
        len = msg != NULL ? fc_metrics_encode(msg, cap, fw_version(a), now_ns, pts, n) : -1;
    }
    free(pts);
    int rc = -1;
    if (len > 0) {
        int watching = fc_port_heap_watch(1) == 0;
        fc_http_target_t t = {
            .host = mc.host,
            .port = mc.port,
            .ca_pem = a->cfg->ca_pem,
            .cert_pem = a->otel_cert_pem,
            .key_pem = a->id.key_pem,
            .hw_key = a->id.hw_key,
            .timeout_s = METRICS_TIMEOUT_S,
            .connect_timeout_s = METRICS_TIMEOUT_S,
        };
        rc = fc_metrics_send(&t, msg, (size_t)len);
        long low = watching ? fc_port_heap_watch(0) : -1;
        if (low >= 0 && (a->export_low < 0 || low < a->export_low - 1024)) {
            fc_port_log(FC_LOG_INFO, "metrics: %s; internal heap at its lowest %ld bytes during the export",
                        rc == 0 ? "sent" : "not sent", low);
        }
        a->export_low = low;
    }
    free(msg);
    metrics_done(a);
    if (rc == 0) {
        a->metrics_backoff_s = 0;
        a->metrics_next = time(NULL) + mc.interval_s;
    } else {
        a->metrics_backoff_s = a->metrics_backoff_s ? a->metrics_backoff_s * 2 : mc.interval_s;
        a->metrics_backoff_s = a->metrics_backoff_s > METRICS_BACKOFF_MAX_S ? METRICS_BACKOFF_MAX_S : a->metrics_backoff_s;
        a->metrics_next = time(NULL) + a->metrics_backoff_s;
    }
}

static void *status_thread(void *arg)
{
    agent_t *a = arg;
    int interval = a->cfg->status_interval_s > 0 ? a->cfg->status_interval_s : 60;
    time_t last_ok = time(NULL), last_put = 0;
    /* The first status lists the saved apps: they start once this thread
     * runs (fc_agent_run). */
    pthread_mutex_lock(&a->lock);
    while (!a->apps_ready) {
        pthread_cond_wait(&a->status_wake, &a->lock);
    }
    pthread_mutex_unlock(&a->lock);
    for (;;) {
        /* Wait until a status is due: the interval, an urgent change, an
         * app change (grouped), or a certificate renewal. */
        pthread_mutex_lock(&a->lock);
        while (last_put != 0) {
            time_t due = last_put + (a->verifying ? RETRY_DELAY_S : interval);
            if (a->status_urgent && last_put + STATUS_MIN_GAP_S < due) {
                due = last_put + STATUS_MIN_GAP_S;
            } else if (a->status_dirty && last_put + NOTIFY_MIN_GAP_S < due) {
                due = last_put + NOTIFY_MIN_GAP_S;
            }
            time_t renew = a->cert_next_try > a->cert_renew_at ? a->cert_next_try : a->cert_renew_at;
            if (renew > 0 && renew < due) {
                due = renew;
            }
            /* Resource samples come on their own interval, between statuses;
             * an alert that starts or clears is reported with the next. */
            if (a->metrics_poke) {
                a->metrics_poke = 0;
                a->metrics_next = 0;
                a->metrics_backoff_s = 0;
            }
            if (time(NULL) >= a->metrics_next) {
                pthread_mutex_unlock(&a->lock);
                metrics_tick(a);
                pthread_mutex_lock(&a->lock);
                continue;
            }
            long long sample = fc_res_next_sample(), mono = fc_res_now();
            if (sample >= 0 && sample <= mono) {
                pthread_mutex_unlock(&a->lock);
                fc_sysinfo_t si = {0};
                fc_port_sysinfo(&si);
                int changed = fc_res_sample(&si, mono);
                pthread_mutex_lock(&a->lock);
                a->status_dirty |= changed;
                continue;
            }
            time_t now = time(NULL);
            if (now >= due) {
                break;
            }
            struct timespec until = {.tv_sec = sample >= 0 && now + (sample - mono) < due ? now + (sample - mono) : due};
            if (a->metrics_next < until.tv_sec) {
                until.tv_sec = a->metrics_next;
            }
            pthread_cond_timedwait(&a->status_wake, &a->lock, &until);
        }
        /* A download has the connection's turn: the status goes out once
         * it is over (turn_end). The time spent waiting does not count as
         * a stall. */
        time_t held = time(NULL);
        while (a->downloading) {
            pthread_cond_wait(&a->status_wake, &a->lock);
        }
        last_ok += time(NULL) - held;
        a->status_urgent = 0;
        a->status_dirty = 0;
        a->putting = 1;
        pthread_mutex_unlock(&a->lock);

        if (time(NULL) - last_ok > STATUS_STALL_REBOOT_S) {
            fc_port_log(FC_LOG_ERROR, "no status update accepted for %d s: restarting", STATUS_STALL_REBOOT_S);
            fc_port_reboot();
        }
        if (a->verifying && time(NULL) > a->verify_deadline) {
            fc_port_log(FC_LOG_ERROR, "firmware %s could not reach Flight Control: rolling back", a->os_image);
            fc_port_ota_rollback();
        }
        pthread_mutex_lock(&a->lock);
        int decommissioning = a->decom != DECOM_NONE;
        pthread_mutex_unlock(&a->lock);
        /* A decommissioning device is refused a certificate, and must not
         * come back as a new one. */
        if (!decommissioning) {
            renew_cert_if_due(a);
        }
        int ok = put_status(a) == 0;
        pthread_mutex_lock(&a->lock);
        a->putting = 0;
        pthread_cond_broadcast(&a->status_sent);
        pthread_mutex_unlock(&a->lock);
        last_put = time(NULL);
        if (ok) {
            last_ok = last_put;
        } else {
            fc_port_log(FC_LOG_WARN, "status update failed, retrying");
            if (!decommissioning) {
                reenroll_if_expired(a);
            }
        }
    }
    return NULL;
}

/* ---- spec -------------------------------------------------------------- */

/* Flight Control 1.4.0 keeps an inline file given in base64 as a data URL
 * with its text percent-encoded, and serves that text as the base64: the
 * padding comes as "%3D". '%' is not in the base64 alphabet, so the escapes
 * are undone first (in place). */
static void percent_decode(char *s)
{
    char *out = s;
    for (const char *p = s; *p; p++) {
        int hi, lo;
        if (p[0] == '%' && (hi = hex_digit(p[1])) >= 0 && (lo = hex_digit(p[2])) >= 0) {
            *out++ = (char)(hi * 16 + lo);
            p += 2;
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
}

static int apply_inline_file(agent_t *a, const cJSON *file, char *err, size_t err_len)
{
    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(file, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(file, "content"));
    const char *enc = cJSON_GetStringValue(cJSON_GetObjectItem(file, "contentEncoding"));
    if (path == NULL || content == NULL) {
        snprintf(err, err_len, "inline file without path or content");
        return -1;
    }
    const char *data = content;
    size_t len = strlen(content);
    unsigned char *buf = NULL;
    if (enc != NULL && strcmp(enc, "base64") == 0) {
        buf = malloc(len + 1);
        char *text = strdup(content);
        size_t out_len = 0;
        if (text != NULL) {
            percent_decode(text);
        }
        if (buf == NULL || text == NULL ||
            mbedtls_base64_decode(buf, len, &out_len, (const unsigned char *)text, strlen(text)) != 0) {
            free(buf);
            free(text);
            snprintf(err, err_len, "%s: invalid base64", path);
            return -1;
        }
        free(text);
        buf[out_len] = '\0';
        data = (const char *)buf;
        len = out_len;
    }
    /* The application validates first, so a rejected file is never stored and
     * the last good one is still there after a reboot. Every file comes again
     * with each rendered version: apps hear only of those that changed. */
    char *stored = NULL;
    size_t stored_len = 0;
    int same = fc_port_read_file(path, &stored, &stored_len) == 0 && stored_len == len && memcmp(stored, data, len) == 0;
    free(stored);
    int rc = 0;
    if (fc_apps_check_file(path, data, err, err_len) != 0) {
        rc = -1;
    } else if (a->cfg->on_config_file != NULL && a->cfg->on_config_file(path, data, len, a->cfg->app_ctx) != 0) {
        snprintf(err, err_len, "%s: rejected by application", path);
        rc = -1;
    } else if ((rc = fc_port_write_file(path, data, len)) != 0) {
        snprintf(err, err_len, "%s: write failed", path);
    } else if (!same) {
        fc_apps_file_changed(path);
        if (strcmp(path, METRICS_CONFIG) == 0) {
            /* A new gateway, or a corrected one: tried now, not after the
             * backoff the old one earned. */
            pthread_mutex_lock(&a->lock);
            a->metrics_poke = 1;
            pthread_cond_signal(&a->status_wake);
            pthread_mutex_unlock(&a->lock);
        }
    }
    free(buf);
    return rc;
}

/* 1 if `path` is a line of `list` (paths, one per line). */
static int path_listed(const char *list, const char *path)
{
    size_t n = strlen(path);
    for (const char *l = list; l != NULL && *l;) {
        const char *end = strchr(l, '\n');
        size_t len = end ? (size_t)(end - l) : strlen(l);
        if (len == n && memcmp(l, path, n) == 0) {
            return 1;
        }
        l = end ? end + 1 : NULL;
    }
    return 0;
}

/* Removes the config files the previous spec gave and this one (`now`, its
 * paths one per line) does not, then remembers `now`. A device that never
 * kept the list (before v3.14) removes nothing the first time. */
static void remove_dropped_files(agent_t *a, const char *now)
{
    char *before = NULL;
    size_t len = 0;
    if (fc_port_kv_get(KV_CONFIG_FILES, &before, &len) == 0) {
        for (char *l = before; l != NULL && *l;) {
            char *end = strchr(l, '\n');
            if (end != NULL) {
                *end = '\0';
            }
            if (*l && !path_listed(now, l)) {
                if (fc_port_remove_file(l) == 0) {
                    fc_port_log(FC_LOG_INFO, "config: removed %s", l);
                    fc_apps_file_changed(l);
                    if (strcmp(l, METRICS_CONFIG) == 0) {
                        pthread_mutex_lock(&a->lock);
                        a->metrics_poke = 1;
                        pthread_cond_signal(&a->status_wake);
                        pthread_mutex_unlock(&a->lock);
                    }
                } else {
                    fc_port_log(FC_LOG_WARN, "config: could not remove %s", l);
                }
            }
            l = end ? end + 1 : NULL;
        }
    }
    if (before == NULL || strcmp(before, now) != 0) {
        fc_port_kv_set(KV_CONFIG_FILES, now, strlen(now));
    }
    free(before);
}

/* spec.resources, kept in storage so that the monitors are the fleet's
 * again after a reboot (the long-poll then answers that nothing changed).
 * Written only when it changes. */
static void apply_resources(const cJSON *resources, char *err, size_t err_len)
{
    fc_res_apply(resources, fc_res_now(), err, err_len);
    char *text = cJSON_IsArray(resources) ? cJSON_PrintUnformatted(resources) : NULL;
    char *stored = NULL;
    size_t len = 0;
    int have = fc_port_kv_get(KV_RESOURCES, &stored, &len) == 0;
    if (text == NULL && have) {
        fc_port_kv_del(KV_RESOURCES);
    } else if (text != NULL && (!have || strcmp(text, stored) != 0)) {
        fc_port_kv_set(KV_RESOURCES, text, strlen(text));
    }
    free(stored);
    free(text);
}

static void load_resources(void)
{
    fc_res_reset(fc_res_now());
    char *stored = NULL;
    size_t len = 0;
    if (fc_port_kv_get(KV_RESOURCES, &stored, &len) == 0) {
        cJSON *r = cJSON_Parse(stored);
        char err[160];
        fc_res_apply(r, fc_res_now(), err, sizeof(err));
        cJSON_Delete(r);
        free(stored);
    }
}

static void apply_rendered(agent_t *a, const char *json)
{
    cJSON *dev = cJSON_Parse(json);
    if (dev == NULL) {
        fc_port_log(FC_LOG_WARN, "rendered device: invalid JSON");
        return;
    }
    const cJSON *ann = cJSON_GetObjectItem(cJSON_GetObjectItem(dev, "metadata"), "annotations");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(ann, RENDERED_ANNOTATION));
    const cJSON *spec = cJSON_GetObjectItem(dev, "spec");
    char err[160] = "";

    const cJSON *decom = cJSON_GetObjectItem(spec, "decommissioning");
    if (cJSON_IsObject(decom)) {
        request_decommission(a, decom);
        cJSON_Delete(dev);
        return;
    }

    apply_resources(cJSON_GetObjectItem(spec, "resources"), err, sizeof(err));

    /* The paths this spec gives, to remove those it no longer does. Not
     * known when a provider is not inline: then nothing is removed. */
    size_t paths_cap = 256, paths_len = 0;
    char *paths = calloc(1, paths_cap);
    int paths_known = paths != NULL;
    const cJSON *provider;
    cJSON_ArrayForEach(provider, cJSON_GetObjectItem(spec, "config")) {
        const cJSON *inline_files = cJSON_GetObjectItem(provider, "inline");
        const char *pname = cJSON_GetStringValue(cJSON_GetObjectItem(provider, "name"));
        if (!cJSON_IsArray(inline_files)) {
            snprintf(err, sizeof(err), "config provider %s: only inline providers are supported",
                     pname ? pname : "?");
            paths_known = 0;
            continue;
        }
        const cJSON *file;
        cJSON_ArrayForEach(file, inline_files) {
            const char *fpath = cJSON_GetStringValue(cJSON_GetObjectItem(file, "path"));
            if (paths_known && fpath != NULL && fpath[0] && !path_listed(paths, fpath)) {
                size_t n = strlen(fpath);
                char *room = paths;
                if (paths_len + n + 2 > paths_cap) {
                    paths_cap = 2 * (paths_len + n + 2);
                    room = realloc(paths, paths_cap);
                }
                if (room == NULL) {
                    paths_known = 0; /* the old buffer is freed below */
                } else {
                    paths = room;
                    memcpy(paths + paths_len, fpath, n);
                    paths_len += n;
                    paths[paths_len++] = '\n';
                    paths[paths_len] = '\0';
                }
            }
            if (apply_inline_file(a, file, err, sizeof(err)) == 0) {
                fc_port_log(FC_LOG_INFO, "config: wrote %s",
                            cJSON_GetStringValue(cJSON_GetObjectItem(file, "path")));
            }
        }
    }
    if (paths_known) {
        remove_dropped_files(a, paths);
    }
    free(paths);

    /* Downloads below take the connection's turn: the agent's is closed
     * while the registry's is open (turn_begin, fc_tls_pool.h). */

    /* WASM apps: replaced without reboot; a failed download keeps the old one. */
    char aerr[160] = "";
    int aretry = 0;
    turn_hold(a);
    if (fc_apps_apply(cJSON_GetObjectItem(spec, "applications"), aerr, sizeof(aerr), &aretry) != 0 && !err[0]) {
        snprintf(err, sizeof(err), "%s", aerr);
    }
    turn_end(a);

    const char *image = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(spec, "os"), "image"));
    char prev_rendered[32], new_digest[80] = "";
    int reboot = 0, retry = aretry;
    pthread_mutex_lock(&a->lock);
    /* "0" never matches a server version, so even a first update is not
     * reported as done before the new image is confirmed. */
    snprintf(prev_rendered, sizeof(prev_rendered), "%s", a->rendered_version[0] ? a->rendered_version : "0");
    int os_change = image && image[0] && strcmp(image, a->os_image) != 0;
    pthread_mutex_unlock(&a->lock);

    if (os_change && !err[0]) {
        if (strcmp(image, a->failed_os_image) == 0) {
            snprintf(err, sizeof(err), "firmware %.100s failed to start and was rolled back; not retried", image);
        } else {
            pthread_mutex_lock(&a->lock);
            snprintf(a->desired_os_image, sizeof(a->desired_os_image), "%s", image);
            snprintf(a->update_reason, sizeof(a->update_reason), "Downloading");
            a->update_error[0] = '\0';
            pthread_mutex_unlock(&a->lock);
            request_status(a);
            char oerr[160];
            int frc = fc_oci_fetch_firmware(image, new_digest, sizeof(new_digest), oerr, sizeof(oerr));
            if (frc != 0) {
                snprintf(err, sizeof(err), "%s", oerr);
                /* Registry or network trouble: try this spec again. An image
                 * the device refused would be refused again: it waits for
                 * the next spec, and the slot is not written every minute. */
                if (frc != FC_OCI_REFUSED) {
                    retry = 1;
                }
            } else {
                reboot = 1;
            }
        }
    }

    pthread_mutex_lock(&a->lock);
    snprintf(a->desired_os_image, sizeof(a->desired_os_image), "%s", image ? image : "");
    snprintf(a->update_error, sizeof(a->update_error), "%s", err);
    /* Like the Linux agent: a spec that failed to apply does not advance
     * status.config.renderedVersion, so the server shows the device OutOfDate
     * with the error. It is not retried until a newer version arrives. */
    if (version != NULL && !retry) {
        snprintf(a->attempted_version, sizeof(a->attempted_version), "%s", version);
        if (!err[0]) {
            snprintf(a->rendered_version, sizeof(a->rendered_version), "%s", version);
        }
    }
    pthread_mutex_unlock(&a->lock);

    if (version != NULL && !err[0]) {
        kv_store_str(KV_RENDERED, version);
    }
    if (reboot) {
        kv_store_str(KV_OS_PENDING, image);
        kv_store_str(KV_OS_PENDING_DIGEST, new_digest);
        kv_store_str(KV_PREV_RENDERED, prev_rendered);
        pthread_mutex_lock(&a->lock);
        snprintf(a->update_reason, sizeof(a->update_reason), "Rebooting");
        snprintf(a->reported_rendered, sizeof(a->reported_rendered), "%s", prev_rendered);
        pthread_mutex_unlock(&a->lock);
        request_status(a);
        /* The stack figure first: the Pico's USB console cuts long lines. */
        fc_port_log(FC_LOG_INFO, "stack never used: %ld bytes; firmware %s installed, rebooting",
                    fc_port_stack_unused(), image);
        sleep(3); /* let the status update go out */
        cJSON_Delete(dev);
        fc_port_reboot();
        return;
    }
    if (err[0]) {
        fc_port_log(FC_LOG_WARN, "spec %s applied with errors: %s", version ? version : "?", err);
    } else {
        fc_port_log(FC_LOG_INFO, "spec %s applied", version ? version : "?");
    }
    cJSON_Delete(dev);
    if (retry) {
        sleep(3 * RETRY_DELAY_S);
    }
}

static void *spec_thread(void *arg)
{
    agent_t *a = arg;
    time_t quiet_since = 0; /* only sessions seen before listed, since then */
    for (;;) {
        char known[32];
        long stack = fc_port_stack_unused();
        pthread_mutex_lock(&a->lock);
        a->stack_spec = stack_low(a->stack_spec, stack);
        snprintf(known, sizeof(known), "%s",
                 a->attempted_version[0] ? a->attempted_version : a->rendered_version);
        pthread_mutex_unlock(&a->lock);

        char path[192];
        if (known[0]) {
            snprintf(path, sizeof(path), API "/devices/%s/rendered?knownRenderedVersion=%s", a->id.name, known);
        } else {
            snprintf(path, sizeof(path), API "/devices/%s/rendered", a->id.name);
        }
        fc_http_response_t r;
        if (mgmt_request(a, a->sess, SPEC_POLL_TIMEOUT_S, "GET", path, NULL, NULL, 0, &r) != 0) {
            sleep(RETRY_DELAY_S);
            continue;
        }
        if (r.status == 200 && consoles(a, r.body, known)) {
            fc_http_free(&r);
            time_t now = time(NULL);
            if (!a->console_quiet) {
                quiet_since = 0;
            } else if (quiet_since == 0) {
                quiet_since = now;
            }
            sleep(quiet_since != 0 && now - quiet_since >= CONSOLE_STALE_AFTER_S ? CONSOLE_STALE_POLL_S : CONSOLE_POLL_S);
            continue;
        }
        quiet_since = 0;
        if (r.status == 200) {
            pthread_mutex_lock(&a->lock);
            a->applying = 1;
            pthread_mutex_unlock(&a->lock);
            apply_rendered(a, r.body);
            request_status(a);
            pthread_mutex_lock(&a->lock);
            a->applying = 0;
            pthread_mutex_unlock(&a->lock);
            pthread_mutex_lock(&a->lock);
            int decommissioning = a->decom != DECOM_NONE;
            pthread_mutex_unlock(&a->lock);
            if (decommissioning) {
                /* No spec matters any more: the status thread finishes. */
                fc_http_free(&r);
                return NULL;
            }
        } else if (r.status == 204) {
            fc_port_log(FC_LOG_DEBUG, "spec unchanged (%s)", known);
        } else {
            fc_port_log(FC_LOG_WARN, "get rendered: HTTP %d %s", r.status, r.body);
            sleep(RETRY_DELAY_S);
        }
        fc_http_free(&r);
    }
    return NULL;
}

/* ---- enrollment -------------------------------------------------------- */

static int create_enrollment_request(agent_t *a)
{
    fc_sysinfo_t si = {0};
    fc_port_sysinfo(&si);

    cJSON *er = cJSON_CreateObject();
    cJSON_AddStringToObject(er, "apiVersion", "v1beta1");
    cJSON_AddStringToObject(er, "kind", "EnrollmentRequest");
    cJSON *meta = cJSON_AddObjectToObject(er, "metadata");
    cJSON_AddStringToObject(meta, "name", a->id.name);
    cJSON *spec = cJSON_AddObjectToObject(er, "spec");
    cJSON_AddStringToObject(spec, "csr", a->id.csr_pem);
    cJSON_AddItemToObject(spec, "deviceStatus", build_status(a));
    cJSON *labels = cJSON_AddObjectToObject(spec, "labels");
    cJSON_AddStringToObject(labels, "alias", si.hostname);
    for (const char *const *l = a->cfg->labels; l && l[0] && l[1]; l += 2) {
        cJSON_AddStringToObject(labels, l[0], l[1]);
    }
    cJSON_AddStringToObject(spec, "osMode", "image");
    char *body = cJSON_PrintUnformatted(er);
    cJSON_Delete(er);
    if (body == NULL) {
        fc_port_log(FC_LOG_WARN, "enrollment request: out of memory");
        return -1;
    }

    fc_http_target_t t = enrollment_target(a, 60);
    fc_http_response_t r;
    int rc = fc_http_request(&t, "POST", API "/enrollmentrequests", "application/json", body, strlen(body), &r);
    free(body);
    if (rc != 0) {
        return -1;
    }
    if (r.status != 201 && r.status != 409) {
        fc_port_log(FC_LOG_WARN, "create enrollment request: HTTP %d %s", r.status, r.body);
        fc_http_free(&r);
        return -1;
    }
    fc_port_log(FC_LOG_INFO, "enrollment request %s (%s)", a->id.name,
                r.status == 201 ? "created" : "already exists");
    fc_http_free(&r);
    return 0;
}

/* Returns 1 when the management certificate was obtained, 0 if still
 * pending, -1 if denied/failed. */
static int poll_enrollment(agent_t *a)
{
    char path[128];
    snprintf(path, sizeof(path), API "/enrollmentrequests/%s", a->id.name);
    fc_http_target_t t = enrollment_target(a, 60);
    fc_http_response_t r;
    if (fc_http_request(&t, "GET", path, NULL, NULL, 0, &r) != 0) {
        return 0;
    }
    int result = 0;
    cJSON *er = r.status == 200 ? cJSON_Parse(r.body) : NULL;
    const cJSON *status = cJSON_GetObjectItem(er, "status");
    const cJSON *cond;
    cJSON_ArrayForEach(cond, cJSON_GetObjectItem(status, "conditions")) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "type"));
        if (type && (strcmp(type, "Denied") == 0 || strcmp(type, "Failed") == 0)) {
            fc_port_log(FC_LOG_ERROR, "enrollment %s: %s", type,
                        cJSON_GetStringValue(cJSON_GetObjectItem(cond, "message")));
            result = -1;
        }
    }
    const char *cert = cJSON_GetStringValue(cJSON_GetObjectItem(status, "certificate"));
    if (result == 0 && cert != NULL && cert[0]) {
        a->mgmt_cert_pem = strdup(cert);
        kv_store_str(KV_MGMT_CERT, cert);
        result = 1;
    }
    cJSON_Delete(er);
    fc_http_free(&r);
    return result;
}

static int enroll(agent_t *a)
{
    while (create_enrollment_request(a) != 0) {
        sleep(RETRY_DELAY_S);
    }
    fc_port_log(FC_LOG_INFO, "waiting for approval: flightctl approve enrollmentrequest/%s", a->id.name);
    for (;;) {
        int rc = poll_enrollment(a);
        if (rc != 0) {
            return rc > 0 ? 0 : -1;
        }
        sleep(RETRY_DELAY_S);
    }
}

/* After the reboot that follows a firmware download: either the new image
 * is running and must be confirmed, or the bootloader went back to the old
 * one (the new image crashed or was rolled back) and the update failed. */
static void resume_firmware_update(agent_t *a)
{
    char pending[256] = "";
    kv_load_str(KV_OS_PENDING, pending, sizeof(pending));
    if (pending[0] == '\0') {
        return;
    }
    if (fc_port_ota_pending_verify()) {
        snprintf(a->os_image, sizeof(a->os_image), "%s", pending);
        kv_load_str(KV_OS_PENDING_DIGEST, a->os_digest, sizeof(a->os_digest));
        a->verifying = 1;
        a->verify_deadline = time(NULL) + (a->cfg->ota_verify_timeout_s > 0 ? a->cfg->ota_verify_timeout_s
                                                                            : OTA_VERIFY_TIMEOUT_S);
        kv_load_str(KV_PREV_RENDERED, a->reported_rendered, sizeof(a->reported_rendered));
        fc_port_log(FC_LOG_INFO, "running new firmware %s: confirming once Flight Control is reached", pending);
        return;
    }
    fc_port_log(FC_LOG_WARN, "firmware %s failed to start: running %s again", pending,
                a->os_image[0] ? a->os_image : "the previous image");
    snprintf(a->update_error, sizeof(a->update_error), "firmware %.110s failed to start and was rolled back", pending);
    snprintf(a->failed_os_image, sizeof(a->failed_os_image), "%s", pending);
    kv_store_str(KV_OS_FAILED, pending);
    kv_load_str(KV_PREV_RENDERED, a->rendered_version, sizeof(a->rendered_version));
    kv_store_str(KV_RENDERED, a->rendered_version);
    fc_port_kv_del(KV_OS_PENDING);
    fc_port_kv_del(KV_OS_PENDING_DIGEST);
    fc_port_kv_del(KV_PREV_RENDERED);
}

int fc_agent_apps_start(const fc_agent_config_t *cfg)
{
    /* The TLS buffers before any app takes the heap: a port may start the
     * apps before the network. */
    fc_tls_pool_init();
    fc_apps_set_hw(cfg->hw);
    return fc_apps_init();
}

int fc_agent_run(const fc_agent_config_t *cfg)
{
    static agent_t a;
    memset(&a, 0, sizeof(a));
    a.stack_spec = a.stack_status = -1;
    a.export_low = -1;
    a.cfg = cfg;
    pthread_mutex_init(&a.lock, NULL);
    pthread_cond_init(&a.status_wake, NULL);
    pthread_cond_init(&a.status_sent, NULL);

    if (decommissioned_at_boot()) {
        /* Unmanaged: the apps go on (Unenroll), nothing talks to the server. */
        fc_agent_apps_start(cfg);
        for (;;) {
            sleep(3600);
        }
    }
    if (fc_http_global_init() != 0 || fc_identity_load(&a.id) != 0) {
        return -1;
    }
    fc_apps_set_device(a.id.name);
    fc_console_set_device(a.id.name);
    make_boot_id(a.boot_id);
    fc_oci_set_signing_key(cfg->signing_pub_pem);
    if (cfg->signing_pub_pem == NULL) {
        fc_port_log(FC_LOG_WARN, "no signing key: firmware and app signatures are not checked");
    }
    a.sess = fc_http_session_new();
    fc_oci_set_turn(turn_begin, turn_end, &a);
    kv_load_str(KV_RENDERED, a.rendered_version, sizeof(a.rendered_version));
    kv_load_str(KV_OS_IMAGE, a.os_image, sizeof(a.os_image));
    kv_load_str(KV_OS_DIGEST, a.os_digest, sizeof(a.os_digest));
    kv_load_str(KV_OS_FAILED, a.failed_os_image, sizeof(a.failed_os_image));
    load_resources();
    fc_port_log(FC_LOG_INFO, "device name: %s", a.id.name);
    resume_firmware_update(&a);

    size_t len = 0;
    if (fc_port_kv_get(KV_MGMT_CERT, &a.mgmt_cert_pem, &len) != 0) {
        if (enroll(&a) != 0) {
            return -1;
        }
    }
    if (cert_info(&a, a.mgmt_cert_pem, &a.cert_renew_at, &a.cert_not_after) == 0) {
        log_cert("management certificate", a.cert_renew_at, a.cert_not_after);
    } else {
        fc_port_log(FC_LOG_WARN, "cannot read the management certificate: it will not be renewed");
    }
    fc_port_log(FC_LOG_INFO, "enrolled, starting management loop");

    /* The spec loop runs on the caller's thread (the main task), which would
     * otherwise only wait: one stack less. Its stack has seen enrollment too. */
    a.stack_spec = fc_port_stack_unused();
    pthread_t status;
    /* The attribute object is kept for as long as the thread runs: on Zephyr
     * it owns the thread's stack. */
    pthread_attr_t status_attr;
    pthread_attr_init(&status_attr);
    /* The status thread's TLS handshakes (ECDSA, X.509 parsing) and its
     * certificate renewals: 4 KB of stack measured on the ESP32-S2, the
     * classic ESP32 and the Pico W. A board can say what it gives. */
#if defined(FC_AGENT_STACK_SIZE)
    size_t stack = FC_AGENT_STACK_SIZE;
#elif defined(ESP_PLATFORM) || (defined(__ZEPHYR__) && !defined(CONFIG_ARCH_POSIX))
    size_t stack = 12 * 1024;
#else
    size_t stack = 256 * 1024; /* 64-bit hosts need more */
#endif
    s_running = &a;
    /* The stack size too: where it cannot be had (Zephyr takes it from a
     * heap), the thread would start on a default stack of 1 KB. */
    if (pthread_attr_setstacksize(&status_attr, stack) != 0 ||
        pthread_create(&status, &status_attr, status_thread, &a) != 0) {
        fc_port_log(FC_LOG_ERROR, "cannot start the agent's status thread (a stack of %u KB)", (unsigned)(stack / 1024));
        return -1;
    }
    /* The saved apps start after the status thread, unless the port
     * started them earlier (fc_agent_apps_start, before the network): each
     * app thread's stack comes from the same heap as the status thread's
     * where the system has one (Zephyr). On a Pico W with two apps saved,
     * they took it all at boot, the status thread could not start and the
     * device was never heard from again. An app that finds no room for its
     * thread is in Error instead, which the status reports. */
    if (fc_agent_apps_start(cfg) != 0) {
        fc_port_log(FC_LOG_WARN, "WASM runtime unavailable: applications will be rejected");
    }
    pthread_mutex_lock(&a.lock);
    a.apps_ready = 1;
    pthread_cond_signal(&a.status_wake);
    pthread_mutex_unlock(&a.lock);
    /* The spec loop ends when the device is decommissioned; the status
     * thread then finishes it and restarts the device. */
    spec_thread(&a);
    pthread_join(status, NULL);
    return 0;
}
