/*
 * Metrics for Flight Control's Telemetry Gateway, which takes OpenTelemetry
 * (OTLP) over gRPC, adds the device's ID and organization to every point and
 * exports them to Prometheus. The gateway knows the device by a certificate
 * of its own, signed by flightctl.io/device-svc-client with the CN
 * "otel-<device name>" (fc_agent.c asks for it); the agent's management
 * certificate is not taken.
 *
 * Here: the apps' values (fc_metric), the request's encoding (an
 * ExportMetricsServiceRequest: one resource, one scope, gauges and
 * cumulative sums) and its sending, on a connection of its own.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fc_http.h"

#define FC_METRICS_PER_APP 8  /* FC_MAX_METRICS of fc_app.h */
#define FC_METRICS_APPS_MAX 32 /* every app's together */
#define FC_METRIC_NAME_MAX 32
#define FC_METRICS_EXPORT_PATH "/opentelemetry.proto.collector.metrics.v1.MetricsService/Export"

/* An app's value: a gauge (kind 0) is set, a counter (kind 1) added to.
 * The name: letters, digits, '_', '.', '-'. FC_HW_* codes. */
int fc_metrics_app_set(const char *app, const char *name, size_t nlen, int kind, double value);
/* The app stopped: its values go. */
void fc_metrics_app_drop(const char *app);

/* One point to send. */
typedef struct {
    char name[96];
    const char *unit;    /* UCUM, as OpenTelemetry has it ("By", "s"...), or "" */
    int sum;             /* 1: a monotonic cumulative sum (a counter); 0: a gauge */
    int is_int;          /* the value is i, else d */
    long long i;
    double d;
    const char *attr_key; /* one attribute, or NULL */
    const char *attr_val;
    uint64_t start_ns;    /* a sum's start */
} fc_metric_point_t;

/* The apps' values as points ("app.<app>.<name>"): how many were put in out. */
int fc_metrics_app_points(fc_metric_point_t *out, int max);

/* An ExportMetricsServiceRequest with the resource's service.name
 * "flightctl-mcu" and service.version, one scope, and the points, at now_ns.
 * Its length, or -1 when it does not fit in cap. */
long fc_metrics_encode(uint8_t *buf, size_t cap, const char *version, uint64_t now_ns, const fc_metric_point_t *pts,
                       int n);

/* Sends it to the gateway (t: its host and port, the CA, the device-svc-client
 * certificate and the device key; no session: a connection for this call,
 * closed after it). 0 once the gateway took it, -1 otherwise. */
int fc_metrics_send(const fc_http_target_t *t, const uint8_t *msg, size_t len);
