/*
 * flightctl-mcu: a Flight Control agent for microcontrollers.
 *
 * Talks to the Flight Control agent API (api/agent/v1beta1): enrollment,
 * rendered device spec (long-poll) and device status.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Agent endpoint, e.g. host "agent-api.192.0.2.10.nip.io", port 8443.
     * The host is also used for SNI and certificate hostname checks. */
    const char *server_host;
    int server_port;

    /* PEM strings, NUL terminated. */
    const char *ca_pem;
    const char *enrollment_cert_pem;
    const char *enrollment_key_pem;
    /* Public key (ECDSA P-256) that firmware and WASM apps must be signed
     * with (cosign format). NULL: signatures are not checked. */
    const char *signing_pub_pem;

    /* Labels requested at enrollment (approver may override). May be NULL. */
    const char *const *labels; /* { "k1", "v1", "k2", "v2", NULL } */

    const char *agent_version;
    int status_interval_s; /* default 60 */
    /* The counters in customInfo (heap, traffic, uptime: fc_port_metrics)
     * are read again only after this many seconds; the statuses in between
     * repeat them. Since 1.4 Flight Control logs an event for every change
     * in the status. Default 3600. */
    int metrics_interval_s;
    /* A new firmware must reach Flight Control within this time, or it is
     * rolled back. Default 150 s. */
    int ota_verify_timeout_s;

    /* Optional application hooks (may be NULL), both called with app_ctx.
     * on_config_file: called on the spec thread after an inline config file
     *   was stored; data is NUL terminated. A non-zero return rejects the file
     *   and is reported as an update error (device Degraded / OutOfDate).
     * custom_info: adds string fields to the cJSON object
     *   status.systemInfo.customInfo, in every status: each change in the
     *   status is an event in Flight Control, so only values that change
     *   seldom. */
    int (*on_config_file)(const char *path, const char *data, size_t len, void *app_ctx);
    void (*custom_info)(void *cjson_obj, void *app_ctx);
    void *app_ctx;

    /* Host functions for WASM apps: a WAMR NativeSymbol array registered
     * under module "env" (may be NULL). */
    const void *wasm_natives;
    int wasm_natives_count;
} fc_agent_config_t;

/* Runs the agent: enrolls if needed, then loops forever. Returns only on a
 * fatal configuration error. */
int fc_agent_run(const fc_agent_config_t *cfg);

/* Starts the WASM runtime and the apps saved by a previous run. Optional:
 * call it before the network is up so apps run offline; fc_agent_run calls
 * it too (idempotent). */
int fc_agent_apps_start(const fc_agent_config_t *cfg);

/* Asks the running agent to send a status update soon (e.g. an alarm
 * changed). Calls are grouped: at most one status every 15 s, however often
 * the value changes. Safe to call from any task; a no-op before fc_agent_run
 * starts. */
void fc_agent_notify_status(void);

#ifdef __cplusplus
}
#endif
