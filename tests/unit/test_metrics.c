/*
 * Metrics for the Telemetry Gateway (fc_metrics.c): the request as
 * OpenTelemetry's own protobuf classes write it (otlp_vectors.h, from
 * gen-otlp-vectors.py), the apps' values and their limits, and the gRPC call
 * against a fake stream.
 */
#include <stdlib.h>

static int fail_allocations;
static unsigned live_tables;

static void *table_malloc(size_t bytes)
{
    void *p = fail_allocations ? NULL : malloc(bytes);
    live_tables += p != NULL;
    return p;
}

static void table_free(void *p)
{
    live_tables -= p != NULL;
    free(p);
}

#define malloc table_malloc
#define free table_free
#include "../../core/src/fc_metrics.c"
#undef free
#undef malloc

#include "otlp_vectors.h"
#include "port_stub.h"
#include "test.h"

/* ---- a fake stream ------------------------------------------------------------- */

struct fc_http_stream {
    int unused;
};

static struct fc_http_stream f_stream;
static int f_open_ok = 1, f_http = 200, f_grpc = 0, f_closed, f_ended;
static const fc_http_target_t *f_target;
static uint8_t f_sent[4096];
static size_t f_sent_len;
static const uint8_t *f_answer;
static size_t f_answer_len, f_answer_at;

fc_http_stream_t *fc_http_stream_open(const fc_http_target_t *t, const char *path, const char *content_type,
                                      const char *const *headers)
{
    f_target = t;
    CHECK_STR(path, "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export");
    CHECK_STR(content_type, "application/grpc");
    return f_open_ok ? &f_stream : NULL;
}

int fc_http_stream_write(fc_http_stream_t *st, const void *data, size_t len, int end, int timeout_ms)
{
    if (f_sent_len + len <= sizeof(f_sent)) {
        memcpy(f_sent + f_sent_len, data, len);
        f_sent_len += len;
    }
    f_ended += end;
    return 0;
}

int fc_http_stream_read(fc_http_stream_t *st, void *buf, size_t cap, int timeout_ms)
{
    if (f_answer_at >= f_answer_len) {
        return -1;
    }
    size_t k = f_answer_len - f_answer_at < cap ? f_answer_len - f_answer_at : cap;
    memcpy(buf, f_answer + f_answer_at, k);
    f_answer_at += k;
    return (int)k;
}

void fc_http_stream_status(fc_http_stream_t *st, int *http_status, int *grpc_status)
{
    *http_status = f_http;
    *grpc_status = f_grpc;
}

void fc_http_stream_close(fc_http_stream_t *st)
{
    f_closed++;
}

static void fake_reset(void)
{
    f_open_ok = 1;
    f_http = 200;
    f_grpc = 0;
    f_closed = f_ended = 0;
    f_sent_len = 0;
    f_answer = NULL;
    f_answer_len = f_answer_at = 0;
}

/* ---- tests ----------------------------------------------------------------------- */

static void the_request_is_what_opentelemetry_writes(void)
{
    fc_metric_point_t p[4];
    memset(p, 0, sizeof(p));
    snprintf(p[0].name, sizeof(p[0].name), "device.memory.free");
    p[0].unit = "By";
    p[0].is_int = 1;
    p[0].i = 123456;
    snprintf(p[1].name, sizeof(p[1].name), "device.network.io");
    p[1].unit = "By";
    p[1].sum = 1;
    p[1].is_int = 1;
    p[1].i = 9790;
    p[1].attr_key = "direction";
    p[1].attr_val = "transmit";
    p[1].start_ns = 1700000000000000000ull;
    snprintf(p[2].name, sizeof(p[2].name), "device.cpu.utilization");
    p[2].unit = "%";
    p[2].d = 12.5;
    snprintf(p[3].name, sizeof(p[3].name), "app.trend.rate");
    p[3].unit = "";
    p[3].sum = 1;
    p[3].d = 2.5;
    p[3].start_ns = 1700000100000000000ull;
    uint8_t buf[1024];
    long n = fc_metrics_encode(buf, sizeof(buf), "v3.13", 1800000000123456789ull, p, 4);
    CHECK_INT(n, (long)sizeof(otlp_golden));
    CHECK(n == (long)sizeof(otlp_golden) && memcmp(buf, otlp_golden, sizeof(otlp_golden)) == 0);
    /* Too small a buffer: refused, not cut. */
    for (size_t cap = 0; cap < sizeof(otlp_golden); cap += 7) {
        CHECK_INT(fc_metrics_encode(buf, cap, "v3.13", 1800000000123456789ull, p, 4), -1);
    }
    /* A message over 127 bytes nested in another: two bytes of length. */
    fc_metric_point_t big;
    memset(&big, 0, sizeof(big));
    memset(big.name, 'x', 90);
    big.unit = "By";
    big.attr_key = "k";
    big.attr_val = "a value long enough to push this metric over one byte of length";
    n = fc_metrics_encode(buf, sizeof(buf), NULL, 1, &big, 1);
    CHECK(n > 0);
}

static void apps_set_add_and_lose_their_values(void)
{
    CHECK_INT(fc_metrics_app_count(), 0);
    CHECK_INT(fc_metrics_app_set("trend", "temp", 4, 0, 21.5), 0);
    CHECK_INT(fc_metrics_app_set("trend", "temp", 4, 0, 22.0), 0); /* a gauge is set */
    CHECK_INT(fc_metrics_app_set("trend", "samples", 7, 1, 2), 0);
    CHECK_INT(fc_metrics_app_set("trend", "samples", 7, 1, 3), 0); /* a counter adds */
    CHECK_INT(fc_metrics_app_set("trend", "samples", 7, 0, 3), FC_HW_EINVAL); /* stays a counter */
    CHECK_INT(fc_metrics_app_set("trend", "samples", 7, 1, -1), FC_HW_EINVAL);
    CHECK_INT(fc_metrics_app_set("trend", "bad name", 8, 0, 1), FC_HW_EINVAL);
    CHECK_INT(fc_metrics_app_set("trend", "", 0, 0, 1), FC_HW_EINVAL);
    CHECK_INT(fc_metrics_app_set("trend", "x", 1, 2, 1), FC_HW_EINVAL);
    CHECK_INT(fc_metrics_app_set("trend", "x", 1, 0, NAN), FC_HW_EINVAL);
    CHECK_INT(fc_metrics_app_set("trend", "x", 1, 0, INFINITY), FC_HW_EINVAL);
    fc_metric_point_t p[FC_METRICS_APPS_MAX];
    int n = fc_metrics_app_points(p, FC_METRICS_APPS_MAX);
    CHECK_INT(n, 2);
    CHECK_INT(fc_metrics_app_count(), n);
    CHECK_STR(p[0].name, "app.trend.temp");
    CHECK(p[0].sum == 0 && p[0].d == 22.0);
    CHECK_STR(p[1].name, "app.trend.samples");
    CHECK(p[1].sum == 1 && p[1].d == 5.0 && p[1].start_ns > 0);
    /* FC_METRICS_PER_APP each. */
    char name[8];
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof(name), "m%d", i);
        CHECK_INT(fc_metrics_app_set("trend", name, strlen(name), 0, i), 0);
    }
    CHECK_INT(fc_metrics_app_set("trend", "m6", 2, 0, 6), FC_HW_ENOSPC);
    CHECK_INT(fc_metrics_app_set("alarm", "m6", 2, 0, 6), 0); /* another app has its own */
    fc_metrics_app_drop("trend");
    n = fc_metrics_app_points(p, FC_METRICS_APPS_MAX);
    CHECK_INT(n, 1);
    CHECK_INT(fc_metrics_app_count(), n);
    CHECK_STR(p[0].name, "app.alarm.m6");
    fc_metrics_app_drop("alarm");
    CHECK_INT(fc_metrics_app_points(p, FC_METRICS_APPS_MAX), 0);
    /* All the apps together: FC_METRICS_APPS_MAX. */
    int ok = 0;
    for (int a = 0; a < 6; a++) {
        char app[8];
        snprintf(app, sizeof(app), "a%d", a);
        for (int i = 0; i < FC_METRICS_PER_APP; i++) {
            snprintf(name, sizeof(name), "m%d", i);
            ok += fc_metrics_app_set(app, name, strlen(name), 0, 1) == 0;
        }
    }
    CHECK_INT(ok, FC_METRICS_APPS_MAX);
    CHECK_INT(fc_metrics_app_count(), ok);
    for (int a = 0; a < 6; a++) {
        char app[8];
        snprintf(app, sizeof(app), "a%d", a);
        fc_metrics_app_drop(app);
    }
}

static void each_publisher_owns_one_table(void)
{
    char first[FC_APP_NAME_MAX + 1], second[sizeof(first)], metric[FC_METRIC_NAME_MAX + 1];
    memset(first, 'a', sizeof(first) - 1);
    first[FC_APP_NAME_MAX] = '\0';
    memcpy(second, first, sizeof(first));
    second[FC_APP_NAME_MAX - 1] = 'b';
    memset(metric, 'm', FC_METRIC_NAME_MAX);
    metric[FC_METRIC_NAME_MAX] = '\0';

    char too_long[sizeof(first) + 1];
    snprintf(too_long, sizeof(too_long), "%sx", first);
    CHECK_INT(live_tables, 0);
    CHECK_INT(fc_metrics_app_set(too_long, metric, FC_METRIC_NAME_MAX, 0, 0), FC_HW_EINVAL);
    CHECK_INT(fc_metrics_app_set(first, "?", 1, 0, 0), FC_HW_EINVAL);
    CHECK_INT(live_tables, 0);
    CHECK_INT(fc_metrics_app_set(first, metric, FC_METRIC_NAME_MAX, 1, 2), 0);
    CHECK_INT(live_tables, 1);
    fail_allocations = 1;
    CHECK_INT(fc_metrics_app_set(second, metric, FC_METRIC_NAME_MAX, 0, 1), FC_HW_ENOSPC);
    CHECK_INT(fc_metrics_app_set(first, metric, FC_METRIC_NAME_MAX, 1, 3), 0);
    CHECK_INT(live_tables, 1);
    fail_allocations = 0;
    CHECK_INT(fc_metrics_app_set(second, metric, FC_METRIC_NAME_MAX, 0, 7), 0);
    CHECK_INT(live_tables, 2);

    fc_metric_point_t p[2];
    CHECK_INT(fc_metrics_app_points(p, 1), 1);
    CHECK_INT(fc_metrics_app_points(p, 2), 2);
    char expected[sizeof(p[0].name)];
    snprintf(expected, sizeof(expected), "app.%s.%s", first, metric);
    CHECK_STR(p[0].name, expected);
    CHECK(p[0].sum && p[0].d == 5);
    fc_metrics_app_drop(first);
    CHECK_INT(live_tables, 1);
    CHECK_INT(fc_metrics_app_points(p, 1), 1);
    snprintf(expected, sizeof(expected), "app.%s.%s", second, metric);
    CHECK_STR(p[0].name, expected);
    CHECK(!p[0].sum && p[0].d == 7);
    CHECK_INT(fc_metrics_app_set(first, metric, FC_METRIC_NAME_MAX, 0, 1), 0);
    fc_metrics_app_drop(first);
    fc_metrics_app_drop(first);
    fc_metrics_app_drop(second);
    CHECK_INT(live_tables, 0);
}

static void the_export_is_one_grpc_call(void)
{
    fc_http_target_t t = {.host = "telemetry.example", .port = 443, .timeout_s = 2};
    const uint8_t msg[] = {0x0a, 0x00};
    fake_reset();
    /* ExportMetricsServiceResponse{}, in its gRPC frame. */
    static const uint8_t empty[] = {0, 0, 0, 0, 0};
    f_answer = empty;
    f_answer_len = sizeof(empty);
    CHECK_INT(fc_metrics_send(&t, msg, sizeof(msg)), 0);
    CHECK_INT(f_sent_len, 5 + sizeof(msg));
    CHECK(memcmp(f_sent, "\0\0\0\0\2\x0a\0", 7) == 0);
    CHECK_INT(f_ended, 1);
    CHECK_INT(f_closed, 1);
    CHECK(f_target == &t);
    /* A partial success: taken, and the refused points logged. */
    fake_reset();
    static const uint8_t partial[] = {0, 0, 0, 0, 9, 0x0a, 0x07, 0x08, 0x03, 0x12, 0x03, 'b', 'a', 'd'};
    f_answer = partial;
    f_answer_len = sizeof(partial);
    CHECK_INT(fc_metrics_send(&t, msg, sizeof(msg)), 0);
    /* Refused (grpc-status 16, unauthenticated), not reached, an HTTP error. */
    fake_reset();
    f_grpc = 16;
    CHECK_INT(fc_metrics_send(&t, msg, sizeof(msg)), -1);
    CHECK_INT(f_closed, 1);
    fake_reset();
    f_open_ok = 0;
    CHECK_INT(fc_metrics_send(&t, msg, sizeof(msg)), -1);
    fake_reset();
    f_http = 503;
    f_grpc = -1;
    CHECK_INT(fc_metrics_send(&t, msg, sizeof(msg)), -1);
}

int main(void)
{
    RUN(the_request_is_what_opentelemetry_writes);
    RUN(apps_set_add_and_lose_their_values);
    RUN(each_publisher_owns_one_table);
    RUN(the_export_is_one_grpc_call);
    return TEST_RESULT();
}
