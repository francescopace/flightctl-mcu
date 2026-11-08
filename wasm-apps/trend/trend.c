/*
 * "trend" WASM app: the second example, without the LED. It smooths the
 * temperature and reports whether it is rising, falling or steady, with the
 * rate in tenths of a degree per minute, e.g. "rising +0.8 C/min (41.2 C)".
 * The state is sent only when the trend or the rounded value changes, and a
 * log line is written when the trend changes.
 */
#include "fc_app.h"

FC_APP_ABI()

#define SAMPLE_MS 5000 /* one sample every 5 s, whatever TICK_MS is */
#define WINDOW 12      /* slope over the last minute */

static int32_t samples[WINDOW], count, next_at, last_key = -1, last_trend = -2;
static int32_t avg_mc; /* exponential moving average */

static int32_t append(char *p, const char *s)
{
    int32_t n = 0;
    while (s[n]) {
        p[n] = s[n];
        n++;
    }
    return n;
}

/* "41.2" from milli-degrees (one decimal, rounded). */
static int32_t fmt_tenths(char *p, int32_t tenths)
{
    int32_t n = 0;
    if (tenths < 0) {
        p[n++] = '-';
        tenths = -tenths;
    }
    n += fc_fmt_int(p + n, tenths / 10);
    p[n++] = '.';
    p[n++] = (char)('0' + tenths % 10);
    return n;
}

FC_EXPORT("tick") void tick(int32_t now_ms)
{
    if (count > 0 && (int32_t)((uint32_t)now_ms - (uint32_t)next_at) < 0) {
        return;
    }
    next_at = now_ms + SAMPLE_MS;
    int32_t t = fc_temp_mc();
    avg_mc = count == 0 ? t : avg_mc + (t - avg_mc) / 4;
    for (int i = WINDOW - 1; i > 0; i--) {
        samples[i] = samples[i - 1];
    }
    samples[0] = avg_mc;
    if (count < WINDOW) {
        count++;
    }
    /* Rate over the samples we have, in tenths of a degree per minute. */
    int32_t span_s = (count - 1) * SAMPLE_MS / 1000;
    int32_t rate = span_s > 0 ? (samples[0] - samples[count - 1]) * 60 / span_s / 100 : 0;
    int32_t trend = rate >= 3 ? 1 : rate <= -3 ? -1 : 0;

    int32_t key = trend * 100000 + rate * 1000 + (avg_mc + 50) / 100;
    if (key == last_key) {
        return;
    }
    last_key = key;
    char s[64];
    int32_t n = append(s, trend > 0 ? "rising " : trend < 0 ? "falling " : "steady ");
    if (trend != 0) {
        if (rate > 0) {
            s[n++] = '+';
        }
        n += fmt_tenths(s + n, rate);
        n += append(s + n, " C/min ");
    }
    s[n++] = '(';
    n += fmt_tenths(s + n, (avg_mc + 50) / 100);
    n += append(s + n, " C)");
    fc_set_state(s, n);
    if (trend != last_trend) {
        fc_log(s, n);
        last_trend = trend;
    }
}
