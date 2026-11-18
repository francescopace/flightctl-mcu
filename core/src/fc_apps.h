#pragma once

#include <stddef.h>

#include "cJSON.h"
#include "fc_hw.h"

/* WASM applications (spec.applications, appType "container" whose image is a
 * WASM OCI artifact), run with WAMR, one thread per app. */

/* Starts the runtime, loads the board map and starts the apps saved by a
 * previous run, so they work before the network is up. */
int fc_apps_init(void);

/* Reconciles the running apps with spec.applications (NULL: none). On error
 * `err` is set; *retry is 1 when the failure may be transient (download). */
int fc_apps_apply(const cJSON *applications, char *err, size_t err_len, int *retry);

/* With `on`, the apps fc_apps_apply installs, updates or restarts for new env
 * vars are only made ready ("Preparing"), and fc_apps_start_pending starts
 * them: the agent calls it once the spec is freed, its connection still
 * closed, so that an app's memory, which comes in one block, is taken from a
 * heap where neither the spec nor the connection is in the way. Returns how
 * many it started. */
void fc_apps_defer_starts(int on);
int fc_apps_start_pending(void);

/* Between specs (the agent's spec thread, at each of its polls: a few
 * minutes apart at most, while nothing changes): an app whose instance did
 * not fit in the heap (no block for its memory, or the agent's floor) is
 * started again once the heap has room for it, looked at no more than every
 * 30 s. */
void fc_apps_poll(void);

/* The hardware the port gives to apps (may be NULL).
 * Set before fc_apps_init. */
void fc_apps_set_hw(const fc_hw_t *hw);

/* The device's name in Flight Control, for fc_info("device"), once known. */
void fc_apps_set_device(const char *name);

/* A config file from the spec: checked before it is stored (0, or -1 with
 * the reason), and its change applied once it is (a new board map restarts
 * the apps that use it; an app's own file raises FC_EV_CONFIG). */
int fc_apps_check_file(const char *path, const char *data, char *err, size_t err_len);
void fc_apps_file_changed(const char *path);

/* Fills status.applications (array), status.applicationsSummary (object)
 * and adds "app.<name>" entries (state set by the app) to customInfo. */
void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info);

/* The least stack any app's thread left unused so far (fc_port_stack_unused),
 * or -1 when no app has run or the port cannot tell. */
long fc_apps_stack_unused(void);
/* The heap free after the last app's instance, and the lowest since with and
 * without the blocks held (fc_heap_hold.h), as "after=.. low=.. raw=..
 * floor=.." in bytes: 1, or 0 when unknown. */
int fc_apps_heap_agent(char *text, size_t len);
/* The heap an app's memory must leave the agent (FC_APP_HEAP_FLOOR): what
 * else the agent starts (a metrics export) waits for room above it. */
long fc_apps_heap_floor(void);

/* The apps saved by a previous run: with fc_apps_hold_saved(1) before
 * fc_apps_init they are loaded but not started ("Preparing") until
 * fc_apps_start_saved, which the agent calls once fc_apps_saved_due says so:
 * a status accepted (fc_apps_status_ok), or none for 3 minutes. Their memory
 * is then taken with the heap at regime, under the floor's check. */
void fc_apps_hold_saved(int on);
int fc_apps_saved_due(void);
int fc_apps_start_saved(void);
/* Each accepted status: an app is past its start once one comes after it. */
void fc_apps_status_ok(void);
/* Before the agent restarts the device for want of an accepted status: with
 * the heap below the floor, the restart counts against the apps still in
 * their start, as a crash does (not run again after two in a row). */
void fc_apps_note_silent_restart(void);

/* MQTT for the apps (fc_mqtt.h): sets the client's hooks, so that messages
 * reach the apps' event queues and FC_EV_NET the apps granted "mqtt". The
 * client connects only when agent_idle(0) says so (no download, no spec being
 * applied, no metrics export) and the heap has room for a TLS handshake and
 * still leaves the agent its floor; then agent_idle(1) holds the agent for
 * the handshake (1) or refuses (0), and agent_connected() releases it once
 * the handshake is done or failed. Called once, before fc_mqtt_configure. */
void fc_apps_mqtt_hooks(int (*agent_idle)(int claim), void (*agent_connected)(void));
