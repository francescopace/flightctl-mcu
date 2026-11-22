/*
 * flightctl-mcu: a Flight Control agent for microcontrollers.
 *
 * Talks to the Flight Control agent API (api/agent/v1beta1): enrollment,
 * rendered device spec (long-poll) and device status.
 */
#pragma once

#include <stddef.h>

#include "fc_hw.h"

#define FC_AGENT_VERSION "flightctl-mcu/0.9.7"

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
    int status_interval_s; /* seconds between local status samples, default 60 */
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

    /* The hardware WASM apps reach (fc_hw.h); may be NULL. */
    const fc_hw_t *hw;
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

/* 1 while the agent has work on hand: a status update due or being sent,
 * a new spec being applied, a metrics export or the handshake of the apps'
 * broker. An app run once starts when this has stayed 0 for a while
 * (fc_apps.c). 1 before the agent has started. */
int fc_agent_busy(void);

/* Seconds since the agent's last accepted status update (or since its status
 * thread started), on a monotonic clock; -1 before it starts. This age can
 * grow while unchanged status is suppressed and spec polls succeed. */
long fc_agent_status_age(void);

/* Restarts the device for want of an accepted status: if the heap is below
 * the apps' floor, the apps in their start are blamed as for a crash (not run
 * again after two in a row). For a port's watchdog. */
void fc_agent_restart_silent(void);

/* Seconds since the spec thread last received an unchanged or parseable
 * rendered device, or since the thread started. Returns -1 before startup
 * and once decommissioning stops the poller. */
long fc_agent_spec_age(void);

/* For the port's watchdog, outside the agent's threads, every minute or so:
 * what has not happened for longer than FC_AGENT_WATCHDOG_S ("status checked
 * or delivered", "rendered spec taken"), with `age` set to how long, or NULL. The
 * device is then to be restarted. */
#define FC_AGENT_WATCHDOG_S 1800
const char *fc_agent_watchdog(long *age);

#ifdef __cplusplus
}
#endif
