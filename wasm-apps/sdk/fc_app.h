/*
 * flightctl-mcu WASM app interface, version 1.
 *
 * An app is a WebAssembly module (wasm32, no libc, no WASI) deployed by
 * Flight Control as a container application whose image is a WASM OCI
 * artifact. The device runs it with WAMR, one thread per app.
 *
 * What the app exports:
 *   tick(now_ms)       required. Called every TICK_MS milliseconds (the app's
 *                      envVars.TICK_MS in the fleet, default 100). now_ms is a
 *                      monotonic clock that wraps after 49 days.
 *   fc_abi_version()   optional, declared with FC_APP_ABI(). A device that
 *                      implements an older version refuses the app instead of
 *                      failing on a missing function later.
 *
 * What the device provides (module "env"):
 *   from the agent, on every device:
 *     fc_set_state(s, len)  free text shown in Flight Control as
 *                           status.systemInfo.customInfo["app.<name>"]; send
 *                           it when it changes, not on every tick
 *     fc_log(s, len)        a line in the device log
 *   from the firmware (firmware/main/alarm.c; the host simulator mimics them):
 *     fc_temp_mc()          temperature in milli-degrees C
 *     fc_threshold_mc()     threshold_c from the fleet config, in milli-degrees C
 *     fc_led(r, g, b)       the board LED, 0-255 per channel (single-colour
 *                           LEDs light for any non-zero value)
 *
 * A trap (out-of-bounds access, unreachable, ...) stops the app; the device
 * restarts it after a few seconds and counts it in
 * status.applications[].restarts. Memory: link with wasm-apps/build.sh, which
 * keeps the module to a few KB of linear memory (see the flags there).
 *
 * Compatibility: within version 1, functions are only added. Removing or
 * changing one means version 2.
 */
#pragma once

#include <stdint.h>

#define FC_APP_ABI_VERSION 1

#define FC_IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define FC_EXPORT(name) __attribute__((export_name(name)))

/* Declares the interface version the app was built against. */
#define FC_APP_ABI()                                                                                         \
    FC_EXPORT("fc_abi_version") int32_t fc_abi_version(void) { return FC_APP_ABI_VERSION; }

FC_IMPORT("set_state") void fc_set_state(const char *s, int32_t len);
FC_IMPORT("log") void fc_log(const char *s, int32_t len);

FC_IMPORT("temp_mc") int32_t fc_temp_mc(void);
FC_IMPORT("threshold_mc") int32_t fc_threshold_mc(void);
FC_IMPORT("led") void fc_led(int32_t r, int32_t g, int32_t b);

/* String literals without strlen (no libc). */
#define FC_STATE(lit) fc_set_state(lit, sizeof(lit) - 1)
#define FC_LOG(lit)   fc_log(lit, sizeof(lit) - 1)

/* Small integer formatting, for states and log lines without printf.
 * Writes at most 12 characters; returns the number written. */
static inline int32_t fc_fmt_int(char *out, int32_t v)
{
    char tmp[12];
    int32_t n = 0, len = 0;
    uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
    do {
        tmp[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && n < 11);
    if (v < 0) {
        out[len++] = '-';
    }
    while (n) {
        out[len++] = tmp[--n];
    }
    return len;
}
