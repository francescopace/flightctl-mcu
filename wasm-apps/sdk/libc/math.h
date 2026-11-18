/* Math (fc_libc.h). expf, logf and tanhf are the device's (from v3.14-rc12):
 * native, faster than C built into the app and with no code in it, their
 * time counted as the string functions' is. The others here are
 * WebAssembly instructions, or a few of them: no import. No other function of C's
 * math.h: built into the app, musl's expf, logf and tanhf take 2 KB. */
#pragma once
#include "fc_libc.h"
FC_LIBC(expf) float expf(float x);
FC_LIBC(logf) float logf(float x);
FC_LIBC(tanhf) float tanhf(float x);

#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VAL __builtin_huge_val()
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

static inline float sqrtf(float x) { return __builtin_sqrtf(x); }
static inline float fabsf(float x) { return __builtin_fabsf(x); }
static inline float floorf(float x) { return __builtin_floorf(x); }
static inline float ceilf(float x) { return __builtin_ceilf(x); }
static inline float truncf(float x) { return __builtin_truncf(x); }
static inline float rintf(float x) { return __builtin_rintf(x); }
static inline float nearbyintf(float x) { return __builtin_nearbyintf(x); }
static inline float copysignf(float a, float b) { return __builtin_copysignf(a, b); }
static inline double sqrt(double x) { return __builtin_sqrt(x); }
static inline double fabs(double x) { return __builtin_fabs(x); }
static inline double floor(double x) { return __builtin_floor(x); }
static inline double ceil(double x) { return __builtin_ceil(x); }
static inline double trunc(double x) { return __builtin_trunc(x); }
static inline double rint(double x) { return __builtin_rint(x); }
static inline double nearbyint(double x) { return __builtin_nearbyint(x); }
static inline double copysign(double a, double b) { return __builtin_copysign(a, b); }
/* C's: a NaN gives the other (WebAssembly's min and max give the NaN). */
static inline float fminf(float a, float b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static inline float fmaxf(float a, float b) { return a != a ? b : b != b ? a : a > b ? a : b; }
static inline double fmin(double a, double b) { return a != a ? b : b != b ? a : a < b ? a : b; }
static inline double fmax(double a, double b) { return a != a ? b : b != b ? a : a > b ? a : b; }
