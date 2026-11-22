/* DeltaBlue bench instrumentation. Each build changes one measurement at a time. */
#pragma once
#include <stdio.h>

enum phase { GRAPH, EXTRACT, EXECUTE, OTHER, PHASE_COUNT };
static const char *phase_names[] = {"graph", "extract", "execute", "other"};
static enum phase active_phase;
static unsigned phase_start;
static struct phase_stats {
    unsigned ms;
    unsigned calls;
    unsigned visits;
    unsigned malloc_ms;
} stats[PHASE_COUNT];

__attribute__((import_module("env"), import_name("clock_ms"))) int clock_ms(void);

static void start_group(unsigned now)
{
    for (int i = 0; i < PHASE_COUNT; i++) {
        stats[i] = (struct phase_stats){0};
    }
    active_phase = OTHER;
    phase_start = now;
}

static void enter_phase(enum phase next)
{
    unsigned now = clock_ms();
    stats[active_phase].ms += now - phase_start;
    phase_start = now;
    active_phase = next;
}

static void finish_group(int group, unsigned now)
{
    stats[active_phase].ms += now - phase_start;
    for (int i = 0; i < PHASE_COUNT; i++) {
        printf("DBP g%d %s ms=%u calls=%u visits=%u malloc_ms=%u\n",
               group, phase_names[i], stats[i].ms, stats[i].calls,
               stats[i].visits, stats[i].malloc_ms);
    }
}
