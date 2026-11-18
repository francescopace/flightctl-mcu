/*
 * Metrics for the Telemetry Gateway (fc_metrics.h): the apps' values, an
 * OTLP protobuf encoder, and a unary gRPC call.
 */
#include "fc_metrics.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fc_hw.h"
#include "fc_port.h"

/* ---- the apps' values --------------------------------------------------------------- */

typedef struct {
    char name[FC_METRIC_NAME_MAX + 1];
    uint8_t len, kind;
    double value;
    uint64_t start_ns; /* a counter's start: when the app first added to it */
} app_metric_t;

typedef struct app_metrics {
    struct app_metrics *next;
    unsigned count;
    app_metric_t metrics[FC_METRICS_PER_APP];
    char app[]; /* the owning app's name, in the same allocation */
} app_metrics_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static app_metrics_t *s_apps;

/* Under s_lock: the link to this app, or where it can be added. */
static app_metrics_t **app_link(const char *app)
{
    app_metrics_t **link = &s_apps;
    for (unsigned i = 0; i < FC_MAX_APPS; i++) {
        if (*link == NULL || strcmp((*link)->app, app) == 0) {
            return link;
        }
        link = &(*link)->next;
    }
    return NULL;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int fc_metrics_app_set(const char *app, const char *name, size_t nlen, int kind, double value)
{
    size_t app_len = strlen(app);
    if (nlen == 0 || nlen > FC_METRIC_NAME_MAX || (kind != 0 && kind != 1) || !isfinite(value) ||
        (kind == 1 && value < 0) || app_len == 0 || app_len > FC_APP_NAME_MAX) {
        return FC_HW_EINVAL;
    }
    for (size_t i = 0; i < nlen; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-')) {
            return FC_HW_EINVAL;
        }
    }
    pthread_mutex_lock(&s_lock);
    app_metrics_t **link = app_link(app);
    if (link == NULL) {
        pthread_mutex_unlock(&s_lock);
        return FC_HW_ENOSPC;
    }
    app_metrics_t *group = *link;
    if (group == NULL) {
        group = malloc(sizeof(*group) + app_len + 1);
        if (group == NULL) {
            pthread_mutex_unlock(&s_lock);
            return FC_HW_ENOSPC;
        }
        group->next = NULL;
        group->count = 0;
        memcpy(group->app, app, app_len + 1);
        *link = group;
    }
    unsigned i = 0;
    while (i < group->count && (group->metrics[i].len != nlen || memcmp(group->metrics[i].name, name, nlen) != 0)) {
        i++;
    }
    int rc = FC_HW_ENOSPC;
    if (i < FC_METRICS_PER_APP) {
        app_metric_t *m = &group->metrics[i];
        if (i == group->count) {
            memcpy(m->name, name, nlen);
            m->name[nlen] = '\0';
            m->len = (uint8_t)nlen;
            m->kind = (uint8_t)kind;
            m->start_ns = now_ns();
            m->value = 0;
            group->count++;
        }
        if (m->kind == kind) {
            m->value = kind == 1 ? m->value + value : value;
            rc = 0;
        } else {
            rc = FC_HW_EINVAL;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

void fc_metrics_app_drop(const char *app)
{
    pthread_mutex_lock(&s_lock);
    app_metrics_t **link = app_link(app);
    if (link != NULL && *link != NULL) {
        app_metrics_t *removed = *link;
        *link = removed->next;
        free(removed);
    }
    pthread_mutex_unlock(&s_lock);
}

int fc_metrics_app_count(void)
{
    int n = 0;
    pthread_mutex_lock(&s_lock);
    for (const app_metrics_t *group = s_apps; group != NULL; group = group->next) {
        n += group->count;
    }
    pthread_mutex_unlock(&s_lock);
    return n;
}

int fc_metrics_app_points(fc_metric_point_t *out, int max)
{
    int n = 0;
    pthread_mutex_lock(&s_lock);
    for (const app_metrics_t *group = s_apps; group != NULL && n < max; group = group->next) {
        for (unsigned i = 0; i < group->count && n < max; i++) {
            const app_metric_t *m = &group->metrics[i];
            fc_metric_point_t *p = &out[n++];
            memset(p, 0, sizeof(*p));
            snprintf(p->name, sizeof(p->name), "app.%s.%s", group->app, m->name);
            p->unit = "";
            p->sum = m->kind == 1;
            p->d = m->value;
            p->start_ns = m->start_ns;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return n;
}

/* ---- protobuf ------------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t cap, len;
    int err;
    size_t mark[8]; /* where each open message's content starts */
    int depth;
} pb_t;

#define LEN_ROOM 3 /* a nested message's length, at most 3 bytes of varint (2 MB) */

static void pb_byte(pb_t *pb, uint8_t b)
{
    if (pb->len < pb->cap) {
        pb->buf[pb->len++] = b;
    } else {
        pb->err = 1;
    }
}

static void pb_varint(pb_t *pb, uint64_t v)
{
    do {
        pb_byte(pb, (uint8_t)((v & 0x7f) | (v >= 0x80 ? 0x80 : 0)));
        v >>= 7;
    } while (v != 0);
}

static void pb_key(pb_t *pb, int field, int wire)
{
    pb_varint(pb, (uint64_t)field << 3 | (uint64_t)wire);
}

static void pb_bytes(pb_t *pb, int field, const void *data, size_t n)
{
    pb_key(pb, field, 2);
    pb_varint(pb, n);
    if (pb->len + n > pb->cap) {
        pb->err = 1;
        return;
    }
    memcpy(pb->buf + pb->len, data, n);
    pb->len += n;
}

static void pb_string(pb_t *pb, int field, const char *s)
{
    if (s != NULL && s[0] != '\0') { /* proto3: an empty string is not written */
        pb_bytes(pb, field, s, strlen(s));
    }
}

static void pb_fixed64(pb_t *pb, int field, uint64_t v)
{
    pb_key(pb, field, 1);
    for (int i = 0; i < 8; i++) {
        pb_byte(pb, (uint8_t)(v >> (8 * i)));
    }
}

static void pb_double(pb_t *pb, int field, double d)
{
    uint64_t v;
    memcpy(&v, &d, sizeof(v));
    pb_fixed64(pb, field, v);
}

/* A nested message: its key, then room for its length, set by pb_end. */
static void pb_begin(pb_t *pb, int field)
{
    pb_key(pb, field, 2);
    if (pb->depth == (int)(sizeof(pb->mark) / sizeof(pb->mark[0])) || pb->len + LEN_ROOM > pb->cap) {
        pb->err = 1;
        return;
    }
    pb->len += LEN_ROOM;
    pb->mark[pb->depth++] = pb->len;
}

static void pb_end(pb_t *pb)
{
    if (pb->err || pb->depth == 0) {
        pb->err = 1;
        return;
    }
    size_t start = pb->mark[--pb->depth], n = pb->len - start;
    uint8_t v[LEN_ROOM];
    size_t k = 0;
    size_t x = n;
    do {
        v[k++] = (uint8_t)((x & 0x7f) | (x >= 0x80 ? 0x80 : 0));
        x >>= 7;
    } while (x != 0 && k < LEN_ROOM);
    if (x != 0) {
        pb->err = 1;
        return;
    }
    /* The content moves back over the room the length did not take. */
    memmove(pb->buf + start - LEN_ROOM + k, pb->buf + start, n);
    memcpy(pb->buf + start - LEN_ROOM, v, k);
    pb->len -= LEN_ROOM - k;
}

/* KeyValue{key, AnyValue{string_value}} as field `field`. */
static void pb_attr(pb_t *pb, int field, const char *key, const char *value)
{
    pb_begin(pb, field);
    pb_string(pb, 1, key);
    pb_begin(pb, 2);
    pb_bytes(pb, 1, value, strlen(value));
    pb_end(pb);
    pb_end(pb);
}

long fc_metrics_encode(uint8_t *buf, size_t cap, const char *version, uint64_t now, const fc_metric_point_t *pts, int n)
{
    pb_t pb = {.buf = buf, .cap = cap};
    pb_begin(&pb, 1); /* ExportMetricsServiceRequest.resource_metrics */
    pb_begin(&pb, 1); /* ResourceMetrics.resource */
    pb_attr(&pb, 1, "service.name", "flightctl-mcu");
    if (version != NULL && version[0]) {
        pb_attr(&pb, 1, "service.version", version);
    }
    pb_end(&pb);
    pb_begin(&pb, 2); /* ResourceMetrics.scope_metrics */
    pb_begin(&pb, 1); /* ScopeMetrics.scope */
    pb_string(&pb, 1, "flightctl-mcu");
    pb_string(&pb, 2, version);
    pb_end(&pb);
    for (int i = 0; i < n; i++) {
        const fc_metric_point_t *p = &pts[i];
        pb_begin(&pb, 2); /* ScopeMetrics.metrics */
        pb_string(&pb, 1, p->name);
        pb_string(&pb, 3, p->unit);
        pb_begin(&pb, p->sum ? 7 : 5); /* Metric.sum, Metric.gauge */
        pb_begin(&pb, 1);              /* data_points */
        if (p->sum) {
            pb_fixed64(&pb, 2, p->start_ns);
        }
        pb_fixed64(&pb, 3, now);
        if (p->is_int) {
            pb_fixed64(&pb, 6, (uint64_t)p->i); /* as_int, sfixed64 */
        } else {
            pb_double(&pb, 4, p->d);
        }
        if (p->attr_key != NULL) {
            pb_attr(&pb, 7, p->attr_key, p->attr_val);
        }
        pb_end(&pb);
        if (p->sum) {
            pb_key(&pb, 2, 0); /* aggregation_temporality: CUMULATIVE */
            pb_varint(&pb, 2);
            pb_key(&pb, 3, 0); /* is_monotonic */
            pb_varint(&pb, 1);
        }
        pb_end(&pb);
        pb_end(&pb);
    }
    pb_end(&pb);
    pb_end(&pb);
    return pb.err || pb.depth != 0 ? -1 : (long)pb.len;
}

/* ---- the gRPC call ------------------------------------------------------------------- */

/* ExportMetricsServiceResponse{partial_success{rejected_data_points = 1,
 * error_message = 2}}: the points refused, logged. */
static void read_answer(const uint8_t *p, size_t n)
{
    if (n < 5 || p[0] != 0) {
        return;
    }
    p += 5;
    n -= 5;
    /* partial_success: field 1, a message. */
    if (n < 2 || p[0] != 0x0a) {
        return;
    }
    size_t len = p[1], off = 2;
    if (len >= 0x80 || len > n - 2) {
        return;
    }
    long long rejected = 0;
    char msg[96] = "";
    while (off < 2 + len) {
        uint8_t key = p[off++];
        if (key == 0x08) { /* rejected_data_points */
            uint64_t v = 0;
            int shift = 0;
            while (off < 2 + len && shift < 64) {
                v |= (uint64_t)(p[off] & 0x7f) << shift;
                shift += 7;
                if (!(p[off++] & 0x80)) {
                    break;
                }
            }
            rejected = (long long)v;
        } else if (key == 0x12 && off < 2 + len) { /* error_message */
            size_t m = p[off++];
            if (m >= 0x80 || off + m > 2 + len) {
                break;
            }
            snprintf(msg, sizeof(msg), "%.*s", (int)m, (const char *)p + off);
            off += m;
        } else {
            break;
        }
    }
    if (rejected > 0 || msg[0]) {
        fc_port_log(FC_LOG_WARN, "metrics: the gateway refused %lld points: %s", rejected, msg);
    }
}

int fc_metrics_send(const fc_http_target_t *t, const uint8_t *msg, size_t len)
{
    fc_http_stream_t *st = fc_http_stream_open(t, FC_METRICS_EXPORT_PATH, "application/grpc", NULL);
    if (st == NULL) {
        fc_port_log(FC_LOG_WARN, "metrics: cannot reach %s:%d", t->host, t->port);
        return -1;
    }
    uint8_t head[5] = {0, (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    int rc = fc_http_stream_write(st, head, sizeof(head), 0, 5000) == 0 &&
                     fc_http_stream_write(st, msg, len, 1, 5000) == 0
                 ? 0
                 : -1;
    uint8_t answer[256];
    size_t got = 0;
    time_t until = time(NULL) + (t->timeout_s > 0 ? t->timeout_s : 15);
    while (rc == 0 && time(NULL) < until) {
        uint8_t buf[128];
        int n = fc_http_stream_read(st, buf, sizeof(buf), 500);
        if (n < 0) {
            break;
        }
        if (n > 0 && got + (size_t)n <= sizeof(answer)) {
            memcpy(answer + got, buf, (size_t)n);
            got += (size_t)n;
        }
    }
    int http = -1, grpc = -1;
    fc_http_stream_status(st, &http, &grpc);
    fc_http_stream_close(st);
    if (rc != 0 || http != 200 || grpc != 0) {
        fc_port_log(FC_LOG_WARN, "metrics: export to %s:%d failed (HTTP %d, grpc-status %d)", t->host, t->port, http,
                    grpc);
        return -1;
    }
    read_answer(answer, got);
    return 0;
}
