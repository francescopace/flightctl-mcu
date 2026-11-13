/*
 * Resource monitors, as Flight Control's own agent runs them.
 *
 * spec.resources gives, for CPU, memory and disk, a sampling interval and
 * alert rules: a severity (Info, Warning, Critical), a percentage of use and
 * a duration. A rule fires once the use has stayed above its percentage for
 * that long, and clears as soon as it drops back. status.resources reports
 * the highest severity firing for each resource, and the summary turns
 * Degraded (Warning) or Error (Critical).
 *
 * Without spec.resources, CPU and disk take the Linux agent's defaults
 * (Warning above 80%, Critical above 90%). Memory does not: a
 * microcontroller's heap is mostly taken by the network stack and TLS at
 * all times, so its default is Warning under 10% free, at once. A fleet that
 * sets a Memory monitor replaces it.
 *
 * Times are seconds on a monotonic clock (fc_res_now): setting the wall clock
 * after boot moves it by years.
 */
#pragma once

#include "cJSON.h"
#include "fc_port.h"

long long fc_res_now(void);

/* Back to the defaults (at boot, and when the spec has no resources). */
void fc_res_reset(long long now);
/* spec.resources from the rendered spec: NULL means none. Monitors it does
 * not list keep their rules, as in the Linux agent. Returns 0, or -1 with
 * `err` set when a monitor is invalid (the others are still applied). */
int fc_res_apply(const cJSON *resources, long long now, char *err, size_t err_len);

/* When the next sample is due (monotonic seconds), -1 if no monitor has rules. */
long long fc_res_next_sample(void);
/* Samples the monitors that are due, from the port's figures. Returns 1 if a
 * rule started or stopped firing. */
int fc_res_sample(const fc_sysinfo_t *si, long long now);

/* Fills status.resources (cpu, memory, disk) and returns what the summary
 * becomes: 0 Online, 1 Degraded, 2 Error, with `info` naming the resources. */
int fc_res_status(cJSON *resources, char *info, size_t info_len);

/* CPU use at the last sample, in percent; -1 before two samples. */
int fc_res_cpu_percent(void);
