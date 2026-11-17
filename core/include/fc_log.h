/*
 * The device's log, kept in RAM: the lines fc_port_log writes (the agent's
 * and the apps'), and the system's own (ESP-IDF's or Zephyr's: WiFi, TLS, a
 * fatal error), in a ring of FC_LOG_RING bytes reserved at build time, so
 * that keeping a line never takes heap. The oldest lines go first. The
 * remote console shows it (`log`, and the `journalctl` the UI's Logs tab
 * runs, fc_console.c).
 *
 * Built with FC_LOG_KEEP (the ESP-IDF and Zephyr ports), the ring is in RAM
 * that a restart does not clear: a restart (a crash, a watchdog, an update)
 * keeps the previous boot's lines, journalctl's boot -1, up to the last one
 * written. A power cut loses them; so does a ring that does not check out
 * after the restart (another firmware's layout, RAM that was overwritten).
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
    uint8_t system;   /* the system's line, "tag: text"; else the agent's */
    uint8_t previous; /* the previous boot's */
    uint32_t up_s; /* the uptime of its boot when it was written, in seconds */
    int64_t when;  /* the time it was written, or 0 when not known */
    char text[FC_LOG_TEXT_MAX + 1];
} fc_log_entry_t;

/* Keeps a line (formatted, without its newline). The ports call it from
 * fc_port_log for the lines they print. */
void fc_log_put(fc_log_level_t level, const char *text);

/* Keeps a line of the system's, "tag: text" as it printed it. */
void fc_log_put_system(fc_log_level_t level, const char *text);

/* The same, from a fatal error's handler: without the lock, which may be
 * held by the code that crashed. Nothing runs after it but the restart. */
void fc_log_put_fatal(const char *text);

/* The line numbered *seq or, if it is gone, the oldest after it: copied into
 * e, *seq set to the one after. 0, or -1 when there is no such line yet. */
int fc_log_next(uint32_t *seq, fc_log_entry_t *e);

/* The number the next line will have. */
uint32_t fc_log_end(void);

/* The uptime in seconds, as up_s counts it. */
uint32_t fc_log_uptime(void);

/* For the tests: as if the device restarted with its RAM kept. */
void fc_log_restart(void);
