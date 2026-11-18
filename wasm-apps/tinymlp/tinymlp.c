/*
 * "tinymlp" WASM app (docs/APPS.md): tinyml with a small neural network. A
 * multi-layer perceptron from emlearn (3 inputs, 16 and 8 tanh, softmax over
 * 4) reads the last minute of the chip's temperature, as tinyml's random
 * forest does, and says whether it is steady, warming, cooling or unstable,
 * with how sure it is: e.g. "warming 97% (52.4 C)".
 *
 * model.h is the C that emlearn writes (train.py makes it), and eml_*.h the
 * headers it needs, unmodified. The network needs expf, logf and tanhf: the
 * device's (sdk/libc/math.h, from v3.14-rc12), so the app has no math code
 * of its own. The features are tinyml's (../tinyml/features.h), in
 * hundredths.
 *
 * Grants: none needed. Config (/etc/fc-app/<name>.json, optional):
 *   sample_ms  as tinyml's: the period between samples, 5000 by default.
 * It has no tick(): a timer samples and the app sleeps in between.
 */
#include "fc_app.h"

#include "../tinyml/features.h"
#include "model.h"

FC_APP_ABI()

static const char *const names[] = {"steady", "warming", "cooling", "unstable"};
static int32_t window[WINDOW], count, sampler = -1;
static int32_t shown_cls = -2, shown_count, shown_tenths, shown_pct; /* what the state says */

/* The state, sent only when it changes: the class (-1 while the window
 * fills, with how full it is) with its probability in percent, and the
 * temperature to a tenth. */
static void show(int32_t cls, int32_t pct, int32_t mc)
{
    int32_t tenths = (mc + (mc < 0 ? -50 : 50)) / 100;
    if (cls == shown_cls && (cls >= 0 || count == shown_count) && pct == shown_pct && tenths == shown_tenths) {
        return;
    }
    shown_cls = cls;
    shown_count = count;
    shown_pct = pct;
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
        s[n++] = ' ';
        n += fc_fmt_int(s + n, pct);
        s[n++] = '%';
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
        show(-1, 0, t);
        return;
    }
    int16_t f[N_FEATURES];
    tml_features(window, f);
    float in[N_FEATURES], p[4];
    for (int32_t i = 0; i < N_FEATURES; i++) {
        in[i] = f[i] * 0.01f;
    }
    eml_net_regress(&mlp, in, N_FEATURES, p, 4); /* the softmax's probabilities */
    int32_t cls = 0;
    for (int32_t c = 1; c < 4; c++) {
        cls = p[c] > p[cls] ? c : cls; /* the first on a tie, as mlp_predict() */
    }
    show(cls, (int32_t)(p[cls] * 100.0f + 0.5f), t);
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
