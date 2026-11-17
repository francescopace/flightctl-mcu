/*
 * "tinyml" WASM app (docs/APPS.md): a TinyML model from emlearn on the
 * device. A random forest reads the last minute of the chip's temperature
 * (12 samples of die_temp, 5 s apart) and says whether it is steady, warming,
 * cooling or unstable (swinging up and down), e.g. "warming (52.4 C)".
 *
 * model.h is the C that emlearn writes, unmodified (train.py makes it); it
 * needs only stdint.h. features.h computes what the model reads, in integers,
 * and passes them to it as floats (WebAssembly has them without imports).
 * The app imports only from "fc".
 *
 * Grants: none needed. Config (/etc/fc-app/<name>.json, optional):
 *   sample_ms  the period between samples, 5000 by default. The model reads
 *              the 12 samples as 5 s apart whatever it is: 500 makes a
 *              minute pass in 6 s, for a demo or a test.
 * It has no tick(): a timer samples and the app sleeps in between.
 */
#include "fc_app.h"

#include "features.h"
#include "model.h"

FC_APP_ABI()

static const char *const names[] = {"steady", "warming", "cooling", "unstable"};
static int32_t window[WINDOW], count, sampler = -1;
static int32_t shown_cls = -2, shown_count, shown_tenths; /* what the state says */

/* The state, sent only when it changes: the class (-1 while the window
 * fills, with how full it is) and the temperature to a tenth. */
static void show(int32_t cls, int32_t mc)
{
    int32_t tenths = (mc + (mc < 0 ? -50 : 50)) / 100;
    if (cls == shown_cls && (cls >= 0 || count == shown_count) && tenths == shown_tenths) {
        return;
    }
    shown_cls = cls;
    shown_count = count;
    shown_tenths = tenths;
    char s[48];
    int32_t n = 0;
    if (cls < 0) {
        for (const char *p = "sampling "; *p; p++) {
            s[n++] = *p;
        }
        n += fc_fmt_int(s + n, count);
        s[n++] = '/';
        n += fc_fmt_int(s + n, WINDOW);
    } else {
        for (const char *p = names[cls]; *p; p++) {
            s[n++] = *p;
        }
    }
    s[n++] = ' ';
    s[n++] = '(';
    if (tenths < 0) {
        s[n++] = '-';
        tenths = -tenths;
    }
    n += fc_fmt_int(s + n, tenths / 10);
    s[n++] = '.';
    s[n++] = (char)('0' + tenths % 10);
    s[n++] = ' ';
    s[n++] = 'C';
    s[n++] = ')';
    fc_set_state(s, n);
}

static void sample(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) < 0) {
        FC_LOG(FC_LOG_WARN, "cannot read die_temp");
        return;
    }
    for (int32_t i = 0; i < WINDOW - 1; i++) {
        window[i] = window[i + 1];
    }
    window[WINDOW - 1] = t;
    if (count < WINDOW) {
        count++;
    }
    if (count < WINDOW) {
        show(-1, t);
        return;
    }
    int16_t f[N_FEATURES];
    tml_features(window, f);
    show(model_predict_int(f), t);
}

static void configure(void)
{
    int32_t ms = fc_config_int(FC_STR("sample_ms"), SAMPLE_MS);
    if (ms < 10 || ms > 600000) {
        ms = SAMPLE_MS;
    }
    if (sampler >= 0) {
        fc_close(sampler);
    }
    sampler = fc_timer_start(ms, 1);
    count = 0; /* a new period: a new window */
    shown_cls = -2;
}

int32_t init(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) == FC_ENOTSUP) {
        FC_STATE("no temperature sensor on this chip");
        return 0; /* no timer: the app stays idle */
    }
    configure();
    sample();
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    (void)value;
    if (type == FC_EV_TIMER && h == sampler) {
        sample();
    } else if (type == FC_EV_CONFIG) {
        configure();
        sample();
    }
}
