#pragma once

#include <stddef.h>

#include "cJSON.h"

/* WASM applications (spec.applications, appType "container" whose image is a
 * WASM OCI artifact), run with WAMR, one thread per app. */

/* Starts the runtime, registers the application's host functions (a WAMR
 * NativeSymbol array for module "env", may be NULL) and starts the apps
 * saved by a previous run, so they work before the network is up. */
int fc_apps_init(const void *natives, int n_natives);

/* Reconciles the running apps with spec.applications (NULL: none). On error
 * `err` is set; *retry is 1 when the failure may be transient (download). */
int fc_apps_apply(const cJSON *applications, char *err, size_t err_len, int *retry);

/* Fills status.applications (array), status.applicationsSummary (object)
 * and adds "app.<name>" entries (state set by the app) to customInfo. */
void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info);
