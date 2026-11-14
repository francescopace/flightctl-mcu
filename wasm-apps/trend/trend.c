/*
 * "trend" WASM app (docs/APPS.md). It smooths the chip's temperature and
 * reports whether it is rising, falling or steady, with the rate in tenths
 * of a degree per minute, e.g. "rising +0.8 C/min (41.2 C)". The state is
 * sent only when the trend or the rounded value changes, and a log line is
 * written when the trend changes.
 *
 * Grants: none needed. With "metrics" in FC_ALLOW it sends the smoothed
 * temperature and the rate as metrics, once the firmware has them. It has no
 * tick(): a timer samples every SAMPLE_MS and the app sleeps in between.
 */
#include "fc_app.h"

FC_APP_ABI()

#define SAMPLE_MS 5000 /* one sample every 5 s */
#define WINDOW 12      /* slope over the last minute */

static int32_t samples[WINDOW], count, last_key = -1, last_trend = -2;
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

/* "41.2" from tenths (one decimal). */
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

static void sample(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) < 0) {
        FC_LOG(FC_LOG_WARN, "cannot read die_temp");
        return;
    }
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

    /* FC_EPERM without "metrics" in FC_ALLOW: nothing to do about it. */
    fc_metric(FC_STR("temp_c"), FC_METRIC_GAUGE, avg_mc / 1000.0);
    fc_metric(FC_STR("rate_c_per_min"), FC_METRIC_GAUGE, rate / 10.0);

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
        fc_log(FC_LOG_INFO, s, n);
        last_trend = trend;
    }
}

int32_t init(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) == FC_ENOTSUP) {
        FC_STATE("no temperature sensor on this chip");
        return 0; /* no timer: the app stays idle */
    }
    if (fc_timer_start(SAMPLE_MS, 1) < 0) {
        return 1;
    }
    sample();
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    (void)h;
    (void)value;
    if (type == FC_EV_TIMER) {
        sample();
    }
}
