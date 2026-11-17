/*
 * The device's log, kept in RAM: the lines fc_port_log writes (the agent's
 * and the apps'), in a ring of FC_LOG_RING bytes reserved at build time, so
 * that keeping a line never takes heap. The oldest lines go first. The
 * remote console shows it (`log`, and the `journalctl` the UI's Logs tab
 * runs, fc_console.c). This boot only: the ring is not kept across a restart.
 */
#pragma once

#include <stdint.h>

#include "fc_port.h"

#ifndef FC_LOG_RING
#define FC_LOG_RING 4096
#endif
#define FC_LOG_TEXT_MAX 240 /* a longer line is cut */

typedef struct {
    uint32_t seq;  /* its number since the start: one more for each line */
    uint8_t level; /* fc_log_level_t */
    uint32_t up_s; /* the uptime when it was written, in seconds */
    char text[FC_LOG_TEXT_MAX + 1];
} fc_log_entry_t;

/* Keeps a line (formatted, without its newline). The ports call it from
 * fc_port_log for the lines they print. */
void fc_log_put(fc_log_level_t level, const char *text);

/* The line numbered *seq or, if it is gone, the oldest after it: copied into
 * e, *seq set to the one after. 0, or -1 when there is no such line yet. */
int fc_log_next(uint32_t *seq, fc_log_entry_t *e);

/* The number the next line will have. */
uint32_t fc_log_end(void);

/* The uptime in seconds, as up_s counts it. */
uint32_t fc_log_uptime(void);
