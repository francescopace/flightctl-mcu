/*
 * Agent state machine: enrollment, then two POSIX threads:
 *  - spec thread:   long-polls GET /devices/{name}/rendered and applies it
 *  - status thread: PUT /devices/{name}/status every interval or on change
 */
#include "fc_agent.h"

#include "fc_apps.h"
#include "fc_heap_hold.h"
#include "fc_console.h"

#include <pthread.h>
#include <stdbool.h>
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
#include "fc_mqtt.h"
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
/* And the heap an export takes above what the agent keeps for itself
 * (fc_apps_heap_floor): its second TLS connection took 8 to 22 KB more than
 * the agent alone on the boards (v3.14-rc7); on a C3 running CoreMark an
 * export and the agent went down to 52 KB below the heap left (v3.14-rc10). */
#ifndef FC_METRICS_HEAP_ROOM
#define FC_METRICS_HEAP_ROOM (20 * 1024)
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
    int waiting_primary; /* poll for a new spec until this one can finish */
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
    bool checked_legacy_metrics; /* spec thread, after the first complete config */
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
    long export_low; /* the heap's low during the last export (fc_heap_low_end), -1 unknown */
    int exporting; /* under lock: an export runs (fc_agent_busy) */
    int starting_saved; /* under lock: the status thread starts the saved apps (apply_begin waits) */
    int metrics_poke; /* under lock: METRICS_CONFIG changed, export now */
    int mqtt_poke;    /* spec thread: the broker's config or CA changed (fc_mqtt.h) */
    int mqtt_retry;   /* spec thread: the client could not start for want of memory */
    int apps_held;    /* spec thread: apps applied, waiting to start, the connection held closed (start_apps) */
    int status_once;  /* under lock: a status was accepted since boot (the agent's TLS is up) */
    int mqtt_connecting; /* under lock: the broker's handshake runs (claimed in idle_for_mqtt) */
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
                        const char *content_type, const char *body, size_t body_len, fc_http_response_t *r,
                        int (*interrupt_wait)(void), int discard_200_body)
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
    t.interrupt_wait = interrupt_wait;
    t.discard_200_body = discard_200_body;
    int rc = fc_http_request(&t, method, path, content_type, body, body_len, r);
    free(cert);
    return rc;
}

/* ---- a look into the rendered device without parsing it ---------------- */

/* The rendered device is parsed whole only when it is to be applied. While
 * a console session is listed the server sends it every poll, and Flight
 * Control may list stale sessions for good (docs/FLIGHTCTL-LIMITATIONS.md, 15): parsing
 * it each time took a tree several times its size from the heap every 15 s,
 * on a board with little to spare. These read the text in place; malformed
 * text gives NULL, and what they find is parsed by cJSON (the console list)
 * or copied (the version). */

static const char *jws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        p++;
    }
    return p;
}

/* Past the string p starts (at its quote). */
static const char *jstr_end(const char *p)
{
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && *++p == '\0') {
            return NULL;
        }
    }
    return *p ? p + 1 : NULL;
}

/* Past the value p starts; no recursion, however deep the nesting. */
static const char *jskip(const char *p)
{
    p = jws(p);
    if (*p == '"') {
        return jstr_end(p);
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                if ((p = jstr_end(p)) == NULL) {
                    return NULL;
                }
                continue;
            }
            if (*p == '{' || *p == '[') {
                depth++;
            } else if ((*p == '}' || *p == ']') && --depth == 0) {
                return p + 1;
            }
            p++;
        }
        return NULL;
    }
    const char *start = p;
    while (*p && !strchr(",}] \t\r\n", *p)) {
        p++;
    }
    return p == start ? NULL : p;
}

/* The value of member `key` of the object at p, or NULL. */
static const char *jmember(const char *p, const char *key)
{
    if (p == NULL || *(p = jws(p)) != '{') {
        return NULL;
    }
    size_t klen = strlen(key);
    for (p = jws(p + 1); *p == '"'; p = jws(p + 1)) {
        const char *k = p + 1, *end = jstr_end(p);
        if (end == NULL || *(p = jws(end)) != ':') {
            return NULL;
        }
        p = jws(p + 1);
        if ((size_t)(end - 1 - k) == klen && memcmp(k, key, klen) == 0) {
            return p;
        }
        if ((p = jskip(p)) == NULL || *(p = jws(p)) != ',') {
            return NULL;
        }
    }
    return NULL;
}

static int same_rendered_version(const char *json, const char *known)
{
    const char *v = jmember(jmember(jmember(json, "metadata"), "annotations"), RENDERED_ANNOTATION);
    const char *end = v && *v == '"' ? jstr_end(v) : NULL;
    size_t n = strlen(known);
    return n && end && (size_t)(end - v - 2) == n && !memchr(v + 1, '\\', n) && !memcmp(v + 1, known, n);
}

/* The spec's console sessions (fc_console.c), on the agent's connection.
 * Returns 1 if the spec is the one applied already: only a console made the
 * server answer, and nothing is to be applied. Only the list of sessions is
 * parsed: none, and nothing is. */
static int consoles(agent_t *a, const char *json, const char *known)
{
    a->console_quiet = 0;
    const char *at = jmember(jmember(json, "spec"), "consoles");
    const char *end = at != NULL && *at == '[' ? jskip(at) : NULL;
    if (end == NULL || *jws(at + 1) == ']') {
        return 0;
    }
    cJSON *list = cJSON_ParseWithLength(at, (size_t)(end - at));
    int same = 0;
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
            same = listed && same_rendered_version(json, known);
            free(cert);
        }
    }
    cJSON_Delete(list);
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
    const char *names[] = {"spec", "status", "apps", "mqtt"};
    long v[] = {a->stack_spec, a->stack_status, fc_apps_stack_unused(), fc_mqtt_stack_unused()};
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
    /* heapAgentBytes: what the agent took of the heap since the last app's
     * instance (fc_apps_heap_agent), for the floor. */
    if (fc_apps_heap_agent(text, sizeof(text))) {
        cJSON_AddStringToObject(m, "heapAgentBytes", text);
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
    /* Flight Control returns the whole Device. Only an error body is used. */
    int rc = mgmt_request(a, a->sess, 60, "PUT", path, "application/json", body, strlen(body), &r, NULL, 1);
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
        a->verifying = 0; /* under the lock: wait_confirmed */
        pthread_mutex_unlock(&a->lock);
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
 * asked for ("Downloading"), goes out first, and a metrics export or the
 * broker's handshake ends; then the connection is closed, and the status
 * thread waits until the pull is over. Pulls nest in a hold
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
    while ((a->putting || a->status_urgent || a->exporting || a->mqtt_connecting) && time(NULL) < until.tv_sec) {
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

/* A refused primary retries in the spec thread. Keep a new status handshake
 * from occupying the block just released by closing idle management TLS
 * until the runner has tried its instance. An active network request keeps
 * its turn, and an open console keeps its stream. */
static int poll_apps(agent_t *a)
{
    if (!fc_apps_primary_retry_due()) {
        return fc_apps_poll(0);
    }
    pthread_mutex_lock(&a->lock);
    int hold = !a->downloading && !a->putting && !a->status_urgent && !a->exporting &&
               !a->mqtt_connecting && !a->verifying && !a->starting_saved && a->decom == DECOM_NONE;
    if (hold) {
        a->downloading = 1;
    }
    pthread_mutex_unlock(&a->lock);
    int released = 0;
    if (hold) {
        long free_before = fc_port_heap_free(), largest_before = fc_port_heap_largest();
        released = fc_http_session_close_idle(a->sess);
        if (released) {
            fc_port_log(FC_LOG_INFO, "primary retry with idle TLS closed: free %ld to %ld B, largest %ld to %ld B",
                        free_before, fc_port_heap_free(), largest_before, fc_port_heap_largest());
        }
    }
    int ready = fc_apps_poll(released);
    if (hold) {
        pthread_mutex_lock(&a->lock);
        a->downloading = 0;
        pthread_cond_signal(&a->status_wake);
        pthread_mutex_unlock(&a->lock);
    }
    return ready;
}

/* A spec is about to be applied: no export and no handshake for the apps'
 * broker starts from now on (both see `applying`), and one under way ends
 * first. Parsing a spec, writing its files and starting its apps take the
 * heap that a TLS handshake holds: on a C6, a spec that came during an
 * export left 1 KB. */
static void apply_begin(agent_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->applying = 1;
    struct timespec until = {.tv_sec = time(NULL) + TURN_WAIT_S};
    while (a->starting_saved) {
        pthread_cond_wait(&a->status_sent, &a->lock);
    }
    while ((a->exporting || a->mqtt_connecting) && time(NULL) < until.tv_sec) {
        pthread_cond_timedwait(&a->status_sent, &a->lock, &until);
    }
    pthread_mutex_unlock(&a->lock);
}

static void apply_end(agent_t *a)
{
    pthread_mutex_lock(&a->lock);
    a->applying = 0;
    pthread_cond_signal(&a->status_wake);
    pthread_mutex_unlock(&a->lock);
}

static agent_t *s_running;

/* fc_mqtt's may_connect, through fc_apps_mqtt_hooks: no handshake for the
 * broker before the agent's own connection is up (at boot the two together
 * left a C3 with 8 KB of heap), nor while a download has the turn, a spec is
 * applied, an export runs, a status is due or a new firmware is being
 * verified. With `claim`, an idle agent is held for the handshake in the
 * same breath (mqtt_connecting, until mqtt_connected): a spec, a pull or an
 * export that comes meanwhile waits for it, instead of starting beside it. */
static int idle_for_mqtt(int claim)
{
    agent_t *a = s_running;
    if (a == NULL) {
        return 0;
    }
    pthread_mutex_lock(&a->lock);
    int busy = !a->status_once || a->downloading || a->turn_depth > 0 || a->applying || a->exporting || a->status_urgent ||
               a->putting || a->verifying || a->decom != DECOM_NONE;
    if (!busy && claim) {
        a->mqtt_connecting = 1;
    }
    pthread_mutex_unlock(&a->lock);
    return !busy;
}

/* The broker's handshake is over, done or failed: what waits for it goes on. */
static void mqtt_connected(void)
{
    agent_t *a = s_running;
    if (a == NULL) {
        return;
    }
    pthread_mutex_lock(&a->lock);
    a->mqtt_connecting = 0;
    pthread_cond_broadcast(&a->status_sent);
    pthread_mutex_unlock(&a->lock);
}

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
    int busy = a->status_urgent || a->status_dirty || a->putting || a->applying || a->exporting || a->mqtt_connecting;
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

typedef enum { CERT_MANAGEMENT, CERT_TELEMETRY } cert_role_t;

static int gateway_subject(const mbedtls_x509_crt *crt, const char *device)
{
    char subject[128], expected[80];
    snprintf(expected, sizeof(expected), "CN=otel-%s", device);
    return mbedtls_x509_dn_gets(subject, sizeof(subject), &crt->subject) >= 0 && strcmp(subject, expected) == 0;
}

/* Checks that a PEM certificate is for this device's key, and returns when
 * its renewal is due and when it expires. */
static int cert_info(const agent_t *a, cert_role_t role, const char *pem, time_t *renew_at, time_t *not_after)
{
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    char name[64];
    int rc = -1;
    if (mbedtls_x509_crt_parse(&crt, (const unsigned char *)pem, strlen(pem) + 1) == 0 &&
        fc_identity_name_of(&crt.pk, name, sizeof(name)) == 0 && strcmp(name, a->id.name) == 0 &&
        (role == CERT_MANAGEMENT || gateway_subject(&crt, name))) {
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

/* Pending (0), refused or unreadable (-1), or an allocated certificate (1). */
static int csr_certificate(const char *json, const char *name, char **pem, size_t *len)
{
    cJSON *csr = cJSON_Parse(json);
    const cJSON *status = cJSON_GetObjectItem(csr, "status"), *cond;
    int result = 0;
    cJSON_ArrayForEach(cond, cJSON_GetObjectItem(status, "conditions")) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "type"));
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "status"));
        if (type && st && strcmp(st, "True") == 0 && (strcmp(type, "Denied") == 0 || strcmp(type, "Failed") == 0)) {
            const char *message = cJSON_GetStringValue(cJSON_GetObjectItem(cond, "message"));
            fc_port_log(FC_LOG_WARN, "certificate request %s %s: %s", name, type, message ? message : "");
            result = -1;
            break;
        }
    }
    const unsigned char *b64 = (const unsigned char *)cJSON_GetStringValue(cJSON_GetObjectItem(status, "certificate"));
    if (result == 0 && b64 && *b64) {
        size_t n = strlen((const char *)b64);
        result = -1;
        if (mbedtls_base64_decode(NULL, 0, len, b64, n) == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
            *pem = malloc(*len + 1);
            if (*pem && mbedtls_base64_decode((unsigned char *)*pem, *len, len, b64, n) == 0) {
                (*pem)[*len] = '\0';
                result = 1;
            }
        }
    }
    cJSON_Delete(csr);
    return result;
}

static int handle_csr(agent_t *a, cert_role_t role, const char *json)
{
    const int gateway = role == CERT_TELEMETRY;
    const char *name = gateway ? a->otel_csr_name : a->csr_name;
    char *pem = NULL;
    size_t len = 0;
    int result = csr_certificate(json, name, &pem, &len);
    if (result != 1) {
        free(pem);
        return result;
    }
    time_t renew_at, not_after;
    if (cert_info(a, role, pem, &renew_at, &not_after) != 0) {
        fc_port_log(FC_LOG_WARN, "certificate request %s: certificate does not match", name);
        free(pem);
        return -1;
    }
    if (fc_port_kv_set(gateway ? KV_OTEL_CERT : KV_MGMT_CERT, pem, len) != 0) {
        fc_port_log(FC_LOG_WARN, "could not store certificate for %s", name);
    }
    if (gateway) {
        free(a->otel_cert_pem);
        a->otel_cert_pem = pem;
        a->otel_renew_at = renew_at;
        a->otel_not_after = not_after;
    } else {
        pthread_mutex_lock(&a->lock);
        free(a->mgmt_cert_pem);
        a->mgmt_cert_pem = pem;
        a->cert_renew_at = renew_at;
        a->cert_not_after = not_after;
        pthread_mutex_unlock(&a->lock);
    }
    log_cert(gateway ? "Telemetry Gateway certificate" : "management certificate renewed", renew_at, not_after);
    return 1;
}

/* All variable fields here are generated locally: the name is a device ID
 * with a random hex suffix, the signer a protocol constant, the CSR Base64.
 * None needs JSON escaping. Encode directly into the request body. */
static char *csr_body(const char *name, const char *signer, const char *pem)
{
    static const char prefix[] = "{\"apiVersion\":\"v1beta1\",\"kind\":\"CertificateSigningRequest\","
                                 "\"metadata\":{\"name\":\"%s\"},\"spec\":{\"signerName\":\"%s\","
                                 "\"usages\":[\"clientAuth\",\"CA:false\"],\"request\":\"";
    static const char end[] = "\"}}";
    size_t plen = strlen(pem), blen = 0;
    if (mbedtls_base64_encode(NULL, 0, &blen, (const unsigned char *)pem, plen) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return NULL;
    }
    size_t cap = sizeof(prefix) + strlen(name) + strlen(signer) + blen + sizeof(end);
    char *body = malloc(cap);
    if (body != NULL) {
        int off = snprintf(body, cap, prefix, name, signer);
        if (off < 0 || (size_t)off + blen + sizeof(end) > cap ||
            mbedtls_base64_encode((unsigned char *)body + off, blen, &blen, (const unsigned char *)pem, plen) != 0) {
            free(body);
            return NULL;
        }
        memcpy(body + off + blen, end, sizeof(end));
    }
    return body;
}

/* Create or poll the outstanding request for this signer. A failed poll
 * transport leaves its name intact so the next round polls the same CSR. */
static int request_cert(agent_t *a, cert_role_t role)
{
    int gateway = role == CERT_TELEMETRY;
    char *name = gateway ? a->otel_csr_name : a->csr_name;
    fc_http_response_t r;
    if (*name == '\0') {
        char cn[80], *generated = NULL;
        snprintf(cn, sizeof(cn), "%s%s", gateway ? "otel-" : "", a->id.name);
        if (gateway && fc_identity_csr(&a->id, cn, &generated) != 0) {
            fc_port_log(FC_LOG_WARN, "metrics: no certificate request with the device key");
            return -1;
        }
        unsigned char rnd[4];
        fc_random(rnd, sizeof(rnd));
        snprintf(name, gateway ? sizeof(a->otel_csr_name) : sizeof(a->csr_name), "%s-%02x%02x%02x%02x",
                 cn, rnd[0], rnd[1], rnd[2], rnd[3]);
        char *body = csr_body(name, gateway ? OTEL_SIGNER : RENEWAL_SIGNER, gateway ? generated : a->id.csr_pem);
        free(generated);
        if (body == NULL) {
            *name = '\0';
            return -1;
        }
        fc_port_log(FC_LOG_INFO, "requesting certificate (%s)", name);
        int rc = mgmt_request(a, a->sess, 60, "POST", API "/certificatesigningrequests", "application/json", body,
                              strlen(body), &r, NULL, 0);
        free(body);
        if (rc != 0) {
            *name = '\0';
            return -1;
        }
        if (r.status != 200 && r.status != 201) {
            fc_port_log(FC_LOG_WARN, "certificate request %s: HTTP %d %s", name, r.status, r.body);
            fc_http_free(&r);
            *name = '\0';
            return -1;
        }
    } else {
        char path[160];
        snprintf(path, sizeof(path), API "/certificatesigningrequests/%s", name);
        if (mgmt_request(a, a->sess, 60, "GET", path, NULL, NULL, 0, &r, NULL, 0) != 0) {
            return 0;
        }
        if (r.status != 200) {
            fc_port_log(FC_LOG_WARN, "certificate request %s: HTTP %d", name, r.status);
            fc_http_free(&r);
            *name = '\0';
            return -1;
        }
    }
    int rc = handle_csr(a, role, r.body);
    fc_http_free(&r);
    if (rc != 0) {
        *name = '\0';
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
    int rc = request_cert(a, CERT_MANAGEMENT);
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

/* The certificate, from storage or asked for: 0 when there is one to use. */
static int otel_cert(agent_t *a, time_t now)
{
    if (a->otel_cert_pem == NULL) {
        char *pem = NULL;
        size_t len = 0;
        if (fc_port_kv_get(KV_OTEL_CERT, &pem, &len) == 0 && pem != NULL) {
            if (cert_info(a, CERT_TELEMETRY, pem, &a->otel_renew_at, &a->otel_not_after) == 0) {
                a->otel_cert_pem = pem;
            } else {
                free(pem); /* an older key's: asked for again */
            }
        }
    }
    int due = a->otel_cert_pem == NULL || now >= a->otel_renew_at;
    if (due && now >= a->otel_next_try) {
        int rc = request_cert(a, CERT_TELEMETRY);
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
     * for a status: turn_begin), a firmware being verified, a status due, a
     * spec being applied or the broker's handshake. */
    pthread_mutex_lock(&a->lock);
    int busy = a->downloading || a->turn_depth > 0 || a->verifying || a->status_urgent || a->applying ||
               a->mqtt_connecting || a->decom != DECOM_NONE;
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
    /* And with room above the agent's floor: an app's memory (CoreMark's
     * 64 KB on a C3) may leave the agent its floor and no more. */
    long free_now = fc_port_heap_free_now();
    long room = fc_apps_heap_floor() + FC_METRICS_HEAP_ROOM;
    if (!busy && free_now >= 0 && free_now < room) {
        if (a->metrics_skipped % 10 == 0) {
            fc_port_log(FC_LOG_WARN, "metrics: %ld bytes free, under %ld (the agent's %ld and the export's %d): export skipped",
                        free_now, room, fc_apps_heap_floor(), FC_METRICS_HEAP_ROOM);
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
    int max = (int)DEVICE_POINTS + fc_metrics_app_count();
    int stretch = fc_heap_low_begin(); /* include point collection and encoding as well as the send */
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
    }
    long low = fc_heap_low_end(stretch, NULL);
    if (low >= 0 && (a->export_low < 0 || low < a->export_low - 1024)) {
        fc_port_log(FC_LOG_INFO, "metrics: %s; internal heap at its lowest %ld bytes during the export",
                    rc == 0 ? "sent" : "not sent", low);
    }
    a->export_low = low;
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

/* When the last status update was accepted, in seconds of CLOCK_MONOTONIC;
 * 0 before the status thread runs (fc_agent_status_age). A long: 32-bit
 * Xtensa has no 8-byte atomics (Zephyr's link fails on __atomic_store_8). */
static long s_status_ok_mono;

static long mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec + 1; /* never 0 */
}

long fc_agent_status_age(void)
{
    long t = __atomic_load_n(&s_status_ok_mono, __ATOMIC_RELAXED);
    return t == 0 ? -1 : mono_s() - t;
}

/* When the spec thread last took a rendered device: told it was unchanged,
 * or got one it could parse (applied, or to be tried again); 0 before it
 * runs, and once decommissioning (fc_agent_spec_age). */
static long s_spec_ok_mono;

static void spec_taken(void)
{
    __atomic_store_n(&s_spec_ok_mono, mono_s(), __ATOMIC_RELAXED);
}

long fc_agent_spec_age(void)
{
    long t = __atomic_load_n(&s_spec_ok_mono, __ATOMIC_RELAXED);
    return t == 0 ? -1 : mono_s() - t;
}

const char *fc_agent_watchdog(long *age)
{
    long s = fc_agent_status_age(), r = fc_agent_spec_age();
    if (s > FC_AGENT_WATCHDOG_S) {
        *age = s;
        return "status update accepted";
    }
    if (r > FC_AGENT_WATCHDOG_S) {
        *age = r;
        return "rendered spec taken";
    }
    return NULL;
}

static void *status_thread(void *arg)
{
    agent_t *a = arg;
    int interval = a->cfg->status_interval_s > 0 ? a->cfg->status_interval_s : 60;
    time_t last_ok = time(NULL), last_put = 0;
    int retry_status_s = 0;
    __atomic_store_n(&s_status_ok_mono, mono_s(), __ATOMIC_RELAXED);
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
            int saved_delay = fc_apps_saved_delay_s();
            if (saved_delay == 0 && !a->applying && !a->downloading && a->turn_depth == 0) {
                a->starting_saved = 1;
                pthread_mutex_unlock(&a->lock);
                fc_apps_start_saved();
                pthread_mutex_lock(&a->lock);
                a->starting_saved = 0;
                pthread_cond_broadcast(&a->status_sent);
                a->status_urgent = 1;
                continue;
            }
            time_t due = last_put + (a->verifying ? RETRY_DELAY_S : retry_status_s ? retry_status_s : interval);
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
            if (saved_delay > 0 && now + saved_delay < until.tv_sec) {
                until.tv_sec = now + saved_delay;
            }
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
            fc_agent_restart_silent();
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
            retry_status_s = 0;
            last_ok = last_put;
            __atomic_store_n(&s_status_ok_mono, mono_s(), __ATOMIC_RELAXED);
            fc_apps_status_ok();
            pthread_mutex_lock(&a->lock);
            a->status_once = 1;
            pthread_mutex_unlock(&a->lock);
        } else {
            if (!retry_status_s) {
                retry_status_s = RETRY_DELAY_S < interval ? RETRY_DELAY_S : interval;
            } else if (retry_status_s <= interval / 2) {
                retry_status_s *= 2;
            } else {
                retry_status_s = interval;
            }
            fc_port_log(FC_LOG_WARN, "status update failed (free %ld B, largest %ld B), retry in %d s",
                        fc_port_heap_free_now(), fc_port_heap_largest(), a->verifying ? RETRY_DELAY_S : retry_status_s);
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

static void config_file_changed(agent_t *a, const char *path)
{
    fc_apps_file_changed(path);
    if (strcmp(path, FC_MQTT_CONFIG) == 0 || strcmp(path, FC_MQTT_CA_FILE) == 0) {
        a->mqtt_poke = 1;
    }
    if (strcmp(path, METRICS_CONFIG) == 0) {
        /* A new gateway, or a corrected one: tried now, not after the
         * backoff the old one earned. */
        pthread_mutex_lock(&a->lock);
        a->metrics_poke = 1;
        pthread_cond_signal(&a->status_wake);
        pthread_mutex_unlock(&a->lock);
    }
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
    } else if (!same && (rc = fc_port_write_file(path, data, len)) != 0) {
        snprintf(err, err_len, "%s: write failed", path);
    } else if (!same) {
        fc_port_log(FC_LOG_INFO, "config: wrote %s", path);
        config_file_changed(a, path);
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

typedef struct {
    char *text;
    size_t len, cap;
} path_list_t;

static int add_path(path_list_t *list, const char *path)
{
    if (path_listed(list->text, path)) {
        return 0;
    }
    size_t n = strlen(path);
    size_t need = list->len + n + 2;
    if (need > list->cap) {
        size_t cap = need > 2 * list->cap ? need : 2 * list->cap;
        char *room = realloc(list->text, cap);
        if (room == NULL) {
            return -1;
        }
        list->text = room;
        list->cap = cap;
    }
    memcpy(list->text + list->len, path, n);
    list->len += n;
    list->text[list->len++] = '\n';
    list->text[list->len] = '\0';
    return 1;
}

/* Record new paths before their files are written. If power or a removal
 * fails, the next spec still knows which files it owns. Only a complete
 * inline configuration can retire files. */
static int apply_files(agent_t *a, const cJSON *config, char *err, size_t err_len)
{
    path_list_t owned = {0}, desired = {0};
    int rc = fc_port_kv_get(KV_CONFIG_FILES, &owned.text, &owned.len);
    if (rc != 0 && rc != FC_KV_MISSING) {
        goto storage_error;
    }
    owned.cap = owned.text ? owned.len + 1 : 0;
    size_t previous_len = owned.len;
    const cJSON *provider, *file;
    cJSON_ArrayForEach(provider, config) {
        const cJSON *files = cJSON_GetObjectItem(provider, "inline");
        if (!cJSON_IsArray(files)) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(provider, "name"));
            snprintf(err, err_len, "config provider %s: only inline providers are supported", name ? name : "?");
            continue;
        }
        cJSON_ArrayForEach(file, files) {
            const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(file, "path"));
            if (!path || !*path || strchr(path, '\n') || !cJSON_IsString(cJSON_GetObjectItem(file, "content"))) {
                snprintf(err, err_len, "invalid inline file path or content");
                goto done;
            }
            if (add_path(&desired, path) < 0 || add_path(&owned, path) < 0) {
                goto storage_error;
            }
        }
    }
    if (owned.len != previous_len && fc_port_kv_set(KV_CONFIG_FILES, owned.text, owned.len) != 0) {
        goto storage_error;
    }
    cJSON_ArrayForEach(provider, config) {
        const cJSON *files = cJSON_GetObjectItem(provider, "inline");
        if (cJSON_IsArray(files)) {
            cJSON_ArrayForEach(file, files) {
                apply_inline_file(a, file, err, err_len);
            }
        }
    }
    if (err[0]) {
        goto done;
    }
    if (!a->checked_legacy_metrics && !path_listed(owned.text, METRICS_CONFIG)) {
        /* The fleet gateway predates the catalog. No other untracked paths
         * have known ownership. Removal is idempotent on every port. */
        if (fc_port_remove_file(METRICS_CONFIG) != 0) {
            goto storage_error;
        }
        pthread_mutex_lock(&a->lock);
        a->metrics_poke = 1;
        pthread_cond_signal(&a->status_wake);
        pthread_mutex_unlock(&a->lock);
    }
    a->checked_legacy_metrics = true;
    if (owned.len != desired.len) {
        char *next;
        for (char *line = strtok_r(owned.text, "\n", &next); line; line = strtok_r(NULL, "\n", &next)) {
            if (!path_listed(desired.text, line)) {
                if (fc_port_remove_file(line) != 0) {
                    goto storage_error;
                }
                fc_port_log(FC_LOG_INFO, "config: removed %s", line);
                config_file_changed(a, line);
            }
        }
        if (fc_port_kv_set(KV_CONFIG_FILES, desired.text ? desired.text : "", desired.len) != 0) {
            goto storage_error;
        }
    }
done:
    free(owned.text);
    free(desired.text);
    return 0;

storage_error:
    snprintf(err, err_len, "cannot reconcile stored config files");
    free(owned.text);
    free(desired.text);
    return -1;
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

/* Returns 1 when the spec is to be tried again (after a while). */
static int apply_rendered(agent_t *a, const char *json)
{
    cJSON *dev = cJSON_Parse(json);
    if (dev == NULL) {
        fc_port_log(FC_LOG_WARN, "rendered device: invalid JSON");
        return 0;
    }
    spec_taken();
    const cJSON *ann = cJSON_GetObjectItem(cJSON_GetObjectItem(dev, "metadata"), "annotations");
    const char *version = cJSON_GetStringValue(cJSON_GetObjectItem(ann, RENDERED_ANNOTATION));
    const cJSON *spec = cJSON_GetObjectItem(dev, "spec");
    char err[160] = "";

    const cJSON *decom = cJSON_GetObjectItem(spec, "decommissioning");
    if (cJSON_IsObject(decom)) {
        request_decommission(a, decom);
        cJSON_Delete(dev);
        return 0;
    }

    const cJSON *applications = cJSON_GetObjectItem(spec, "applications");
    fc_apps_plan_t plan;
    const cJSON *config = cJSON_GetObjectItem(spec, "config");
    int bad_config = config != NULL && !cJSON_IsArray(config);
    int prepared = !cJSON_IsObject(spec) || bad_config ? -1 : fc_apps_prepare(applications, &plan);
    if (prepared != 0) {
        int retry_catalog = prepared == FC_APPS_CATALOG_ERROR;
        const char *reason = retry_catalog ? "cannot store the app catalog" :
                             bad_config ? "spec.config must be an array" : "invalid spec or application list";
        fc_port_log(FC_LOG_WARN, "rendered device: %s", reason);
        pthread_mutex_lock(&a->lock);
        a->waiting_primary = 0;
        snprintf(a->update_error, sizeof(a->update_error), "%s", reason);
        if (version != NULL && !retry_catalog) {
            snprintf(a->attempted_version, sizeof(a->attempted_version), "%s", version);
        }
        pthread_mutex_unlock(&a->lock);
        cJSON_Delete(dev);
        return retry_catalog || version == NULL;
    }
    apply_resources(cJSON_GetObjectItem(spec, "resources"), err, sizeof(err));

    int retry_files = apply_files(a, config, err, sizeof(err)) != 0;
    if (a->mqtt_poke) {
        /* A new broker, a new CA, or none: once both files are written. */
        a->mqtt_poke = 0;
        a->mqtt_retry = fc_mqtt_configure(a->id.name) == -2;
    }

    /* Downloads below take the connection's turn: the agent's is closed
     * while the registry's is open (turn_begin, fc_tls_pool.h). */

    /* A new firmware comes first, and the apps of the same spec are checked
     * by it, after the reboot: an app may need what only the new firmware
     * has (an import), which the running one would refuse, and the spec
     * with it. Its config files are written already: the registry's CA and
     * password may be among them. */
    const char *image = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(spec, "os"), "image"));
    char prev_rendered[32], new_digest[80] = "";
    int reboot = 0, retry = retry_files;
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

    /* WASM apps: replaced without reboot; a failed download keeps the old
     * one. Not now if a new firmware is installed (its own reboot applies
     * this spec again), nor while its download is to be tried again; with a
     * firmware that is not coming (refused, or rolled back), the running
     * one's apps are updated all the same. */
    if (!reboot && !retry) {
        char aerr[160] = "";
        turn_hold(a);
        if (fc_apps_apply_prepared(&plan, aerr, sizeof(aerr), &retry) != 0 && !err[0]) {
            snprintf(err, sizeof(err), "%s", aerr);
        }
        a->apps_held = 1; /* the hold ends once they are started (start_apps) */
    }

    pthread_mutex_lock(&a->lock);
    snprintf(a->desired_os_image, sizeof(a->desired_os_image), "%s", image ? image : "");
    snprintf(a->update_error, sizeof(a->update_error), "%s", err);
    /* Like the Linux agent: a spec that failed to apply does not advance
     * status.config.renderedVersion, so the server shows the device OutOfDate
     * with the error. It is not retried until a newer version arrives.
     * A spec whose firmware reboots the device is not applied yet: its apps
     * come after the reboot, when the new firmware asks for it again (from
     * the version before it). */
    a->waiting_primary = retry == FC_APPS_WAIT_PRIMARY;
    if (version != NULL && (!retry || a->waiting_primary) && !reboot) {
        snprintf(a->attempted_version, sizeof(a->attempted_version), "%s", version);
        if (!err[0]) {
            snprintf(a->rendered_version, sizeof(a->rendered_version), "%s", version);
        }
    }
    pthread_mutex_unlock(&a->lock);

    if (version != NULL && !err[0] && !reboot) {
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
        return 0;
    }
    if (err[0]) {
        fc_port_log(FC_LOG_WARN, "spec %s applied with errors: %s", version ? version : "?", err);
    } else {
        fc_port_log(FC_LOG_INFO, "spec %s applied", version ? version : "?");
    }
    cJSON_Delete(dev);
    return retry == 1;
}

/* The apps the spec installed or changed are started here, once the spec,
 * parsed and as text, is freed, with the connection still closed
 * (fc_apps_defer_starts): their memory, which comes in one block, finds the
 * heap with neither in the way. A spec held 16 KB on an ESP32-C5 and left
 * its largest block 34 KB for CoreMark's 68. Then the connection opens
 * again. */
static void start_apps(agent_t *a)
{
    if (a->apps_held) {
        a->apps_held = 0;
        fc_apps_start_pending();
        turn_end(a);
    }
}

/* A new firmware is judged on its own: the spec that came with it, whose
 * apps it applies after its reboot, waits until it is confirmed. An app
 * pull before would close the connection that confirms it (turn_begin),
 * and an app that crashes the device would roll the firmware back. If it
 * never is, the status thread rolls it back (verify_deadline). */
static void wait_confirmed(agent_t *a)
{
    pthread_mutex_lock(&a->lock);
    while (a->verifying) {
        struct timespec until = {.tv_sec = time(NULL) + RETRY_DELAY_S};
        pthread_cond_timedwait(&a->status_sent, &a->lock, &until);
    }
    pthread_mutex_unlock(&a->lock);
}

static int spec_work_due(void)
{
    return fc_console_finished() || fc_apps_poll_due();
}

static void *spec_thread(void *arg)
{
    agent_t *a = arg;
    time_t quiet_since = 0; /* only sessions seen before listed, since then */
    spec_taken();
    for (;;) {
        if (fc_console_finished()) {
            fc_console_stop();
        }
        if (a->mqtt_retry) {
            a->mqtt_retry = fc_mqtt_configure(a->id.name) == -2;
        }
        if (poll_apps(a) && a->waiting_primary) {
            /* Ask for the pending spec again. While the primary needed
             * the heap, knownRenderedVersion still admitted newer specs. */
            pthread_mutex_lock(&a->lock);
            a->attempted_version[0] = '\0';
            a->waiting_primary = 0;
            pthread_mutex_unlock(&a->lock);
        }
        char known[32];
        long stack = fc_port_stack_unused();
        pthread_mutex_lock(&a->lock);
        a->stack_spec = stack_low(a->stack_spec, stack);
        snprintf(known, sizeof(known), "%s",
                 a->attempted_version[0] ? a->attempted_version : a->rendered_version);
        pthread_mutex_unlock(&a->lock);

        /* Repeated local wakeups can outlast a whole server poll. Read the
         * current version then, so only a real response feeds the watchdog. */
        int refresh = known[0] && fc_agent_spec_age() >= SPEC_POLL_TIMEOUT_S;
        char path[192];
        if (known[0] && !refresh) {
            snprintf(path, sizeof(path), API "/devices/%s/rendered?knownRenderedVersion=%s", a->id.name, known);
        } else {
            snprintf(path, sizeof(path), API "/devices/%s/rendered", a->id.name);
        }
        fc_http_response_t r;
        int rc = mgmt_request(a, a->sess, SPEC_POLL_TIMEOUT_S, "GET", path, NULL, NULL, 0, &r,
                              known[0] && !refresh ? spec_work_due : NULL, 0);
        if (rc == FC_HTTP_INTERRUPTED) {
            continue;
        }
        if (rc != 0) {
            sleep(RETRY_DELAY_S);
            continue;
        }
        if (r.status == 200 && consoles(a, r.body, known)) {
            spec_taken();
            fc_http_free(&r);
            time_t now = time(NULL);
            if (!a->console_quiet) {
                quiet_since = 0;
            } else if (quiet_since == 0) {
                quiet_since = now;
            }
            int delay = quiet_since != 0 && now - quiet_since >= CONSOLE_STALE_AFTER_S
                            ? CONSOLE_STALE_POLL_S : CONSOLE_POLL_S;
            /* A completed console worker or one-shot can release memory
             * for another app before the next network poll. */
            int check_workers = !a->console_quiet || fc_apps_awaiting_primary_completion();
            while (delay > 0 && !spec_work_due()) {
                int step = check_workers ? 1 : delay;
                sleep(step);
                delay -= step;
            }
            continue;
        }
        quiet_since = 0;
        if (refresh && r.status == 200 && same_rendered_version(r.body, known)) {
            spec_taken();
            fc_http_free(&r);
            continue;
        }
        if (r.status == 200) {
            wait_confirmed(a);
            apply_begin(a);
            int again = apply_rendered(a, r.body);
            fc_http_free(&r);
            start_apps(a);
            request_status(a);
            apply_end(a);
            pthread_mutex_lock(&a->lock);
            int decommissioning = a->decom != DECOM_NONE;
            pthread_mutex_unlock(&a->lock);
            if (decommissioning) {
                /* No spec matters any more: the status thread finishes. */
                __atomic_store_n(&s_spec_ok_mono, 0, __ATOMIC_RELAXED);
                return NULL;
            }
            if (again) {
                sleep(3 * RETRY_DELAY_S);
            }
        } else if (r.status == 204) {
            spec_taken();
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
     * apps before the network. The saved apps are loaded now, and started
     * once the agent is up (fc_apps_saved_delay_s, status thread). */
    fc_tls_pool_init();
    fc_apps_set_hw(cfg->hw);
    fc_apps_hold_saved(1);
    return fc_apps_init();
}

void fc_agent_restart_silent(void)
{
    fc_apps_note_silent_restart();
    fc_port_reboot();
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
        fc_apps_start_saved();
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
    if (cert_info(&a, CERT_MANAGEMENT, a.mgmt_cert_pem, &a.cert_renew_at, &a.cert_not_after) == 0) {
        log_cert("management certificate", a.cert_renew_at, a.cert_not_after);
    } else {
        fc_port_log(FC_LOG_WARN, "cannot read the management certificate: it will not be renewed");
    }
    fc_port_log(FC_LOG_INFO, "enrolled, starting management loop");
    /* The apps' broker, if the fleet gave one: its thread and its pool
     * before the apps, whose heap check then counts them; it connects once
     * the agent is idle (idle_for_mqtt). */
    fc_apps_mqtt_hooks(idle_for_mqtt, mqtt_connected);
    fc_apps_defer_starts(1); /* started by start_apps, once the spec is freed */
    a.mqtt_retry = fc_mqtt_configure(a.id.name) == -2;

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
#elif defined(ESP_PLATFORM) && !defined(__ZEPHYR__) && defined(__riscv)
    size_t stack = 8 * 1024; /* ESP-IDF on RISC-V (C3, C5, C6): 4.6 KB at most used, v3.14-rc7 */
#elif defined(ESP_PLATFORM) && !defined(__ZEPHYR__)
    size_t stack = 10 * 1024; /* ESP-IDF on Xtensa (ESP32, S3): 5 KB at most used, v3.14-rc7 */
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
