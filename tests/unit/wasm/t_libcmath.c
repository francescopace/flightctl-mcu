/* math.h of wasm-apps/sdk/libc: expf, logf and tanhf are the device's, the
 * rest WebAssembly instructions. The state has a bit set for each check that
 * failed, bad=0 if none did. The values are compared to C's within a few
 * ulps, as the device's C library is not the host's; the inputs are volatile,
 * so that clang does not compute the calls itself. */
#include <math.h>
#include <stdio.h>

#include "fc_app.h"

FC_APP_ABI()

static int near(float got, float want)
{
    float d = fabsf(got - want);
    return d <= fabsf(want) * 4e-7f || d <= 1e-37f;
}

int32_t init(void)
{
    volatile float zero = 0.0f, one = 1.0f, two = 2.0f, half = 0.5f, minus = -3.0f, huge = 100.0f, inf = INFINITY;
    int bad = 0, bit = 1;
#define CHECK(c) (bad |= (c) ? 0 : bit, bit <<= 1)
    CHECK(near(expf(one), 2.71828183f) && near(expf(minus), 0.0497870684f) && expf(zero) == 1.0f);
    CHECK(near(logf(two), 0.693147181f) && logf(one) == 0.0f && near(logf(half), -0.693147181f));
    CHECK(near(tanhf(half), 0.462117157f) && near(tanhf(minus), -0.995054754f) && tanhf(huge) == 1.0f);
    /* At the edges: C's values. */
    CHECK(expf(-inf) == 0.0f && expf(huge) == INFINITY && expf(inf) == INFINITY);
    CHECK(logf(zero) == -INFINITY && isnan(logf(minus)) && logf(inf) == INFINITY);
    CHECK(tanhf(-inf) == -1.0f && isnan(tanhf(NAN)) && isnan(expf(NAN)));
    /* The instructions. */
    CHECK(sqrtf(two * two) == 2.0f && fabsf(minus) == 3.0f && floorf(-half) == -1.0f && ceilf(half) == 1.0f);
    CHECK(truncf(-1.5f * one) == -1.0f && rintf(2.5f * one) == 2.0f && copysignf(two, minus) == -2.0f);
    CHECK(fminf(NAN, one) == 1.0f && fmaxf(one, NAN) == 1.0f && fminf(one, two) == 1.0f && fmaxf(one, two) == 2.0f);
    CHECK(fmin(one, NAN) == 1.0 && fmax(NAN, two) == 2.0 && sqrt(two * 8.0) == 4.0 && floor(-0.5 * one) == -1.0);
    CHECK(isinf(inf) && !isfinite(inf) && isfinite(one) && signbit(minus) && !signbit(one));
    char out[64];
    int n = snprintf(out, sizeof(out), "bad=%x %.4f", bad, (double)(expf(one) + logf(two) + tanhf(half)));
    fc_set_state(out, n);
    return 0;
}

void tick(void) {}
