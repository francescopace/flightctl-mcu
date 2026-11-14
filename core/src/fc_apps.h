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
