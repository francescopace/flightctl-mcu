/*
 * What a crash was, kept across the restart it causes: the record a port's
 * fatal error handler fills in RAM that a restart does not clear, and the
 * text the status gives at the next boot (customInfo.lastFatalError):
 *
 *   <reason> (<code>) in thread "<name>", <ms> ms after boot, pc 0x<pc> <addresses>
 *
 * The addresses, decoded with addr2line against the ELF of the firmware that
 * crashed, are what was calling: Arm's lr, Xtensa's backtrace (each frame's
 * return address), or, on RISC-V without frame pointers, ra and the words of
 * the stack that point into code (candidates, as idf.py monitor decodes a
 * stack dump: some may be stale). When the firmware that crashed is not the
 * one running (a rollback after a crash), its version follows.
 *
 * The handler writes the fixed part first and seals it, then adds the
 * addresses one at a time, sealing after each: a fault while it walks the
 * stack still leaves a record. Header only: the ports and the unit tests
 * include it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FC_FATAL_MAGIC 0x46435231u /* "FCR1" */
#define FC_FATAL_ADDRS 8

typedef enum {
    FC_FATAL_LR = 0,        /* addrs[0]: the link register (Arm) */
    FC_FATAL_BACKTRACE = 1, /* return addresses, innermost first (Xtensa) */
    FC_FATAL_RA_STACK = 2,  /* addrs[0]: ra, then code addresses found on the stack (RISC-V) */
} fc_fatal_kind_t;

typedef struct {
    uint32_t magic;
    uint32_t code;      /* the port's: Zephyr's reason, the exception cause */
    uint32_t uptime_ms; /* since the boot that crashed */
    uint32_t pc;
    uint32_t addrs[FC_FATAL_ADDRS];
    uint8_t naddrs;
    uint8_t kind; /* fc_fatal_kind_t */
    char reason[34];
    char thread[18];
    char firmware[32]; /* the version of the firmware that crashed, or empty */
    uint32_t check;
} fc_fatal_t;

/* Inlined: a fault handler in IRAM must not call code in flash. */
#define FC_FATAL_INLINE static inline __attribute__((always_inline))

FC_FATAL_INLINE uint32_t fc_fatal_sum(const fc_fatal_t *f)
{
    /* FNV-1a over everything before check: cheap enough for a fault handler. */
    const uint8_t *p = (const uint8_t *)f;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(fc_fatal_t, check); i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

FC_FATAL_INLINE void fc_fatal_seal(fc_fatal_t *f)
{
    f->magic = FC_FATAL_MAGIC;
    f->check = fc_fatal_sum(f);
}

FC_FATAL_INLINE int fc_fatal_valid(const fc_fatal_t *f)
{
    return f->magic == FC_FATAL_MAGIC && f->check == fc_fatal_sum(f) && f->naddrs <= FC_FATAL_ADDRS &&
           f->kind <= FC_FATAL_RA_STACK;
}

/* Copies a string a fault handler was given, bounded and without the C
 * library (which may be in flash that cannot be read then). */
FC_FATAL_INLINE void fc_fatal_copy(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; src != NULL && i + 1 < n && src[i] != '\0'; i++) {
        dst[i] = src[i] >= ' ' && src[i] != '"' && src[i] < 0x7f ? src[i] : '?';
    }
    dst[i] = '\0';
}

/* The text of a valid record; running: the version of the firmware running
 * now (a different one is named), or NULL. */
static inline void fc_fatal_format(const fc_fatal_t *f, const char *running, char *buf, size_t n)
{
    char reason[sizeof(f->reason)], thread[sizeof(f->thread)], fw[sizeof(f->firmware)];
    fc_fatal_copy(reason, sizeof(reason), f->reason);
    fc_fatal_copy(thread, sizeof(thread), f->thread);
    fc_fatal_copy(fw, sizeof(fw), f->firmware);
    if (reason[0] == '\0') {
        snprintf(reason, sizeof(reason), "fatal error"); /* the handler could not read it */
    }
    int at = snprintf(buf, n, "%s (%u) in thread \"%s\", %u ms after boot, pc 0x%08x", reason, (unsigned)f->code,
                      thread, (unsigned)f->uptime_ms, (unsigned)f->pc);
    for (int i = 0; i < f->naddrs && at > 0 && (size_t)at < n; i++) {
        const char *label = "";
        if (i == 0) {
            label = f->kind == FC_FATAL_LR ? " lr" : f->kind == FC_FATAL_BACKTRACE ? " backtrace" : " ra";
        } else if (i == 1 && f->kind == FC_FATAL_RA_STACK) {
            label = ", stack";
        }
        at += snprintf(buf + at, n - (size_t)at, "%s 0x%08x", label, (unsigned)f->addrs[i]);
    }
    if (fw[0] != '\0' && running != NULL && strcmp(fw, running) != 0 && at > 0 && (size_t)at < n) {
        snprintf(buf + at, n - (size_t)at, ", firmware %s", fw);
    }
}
