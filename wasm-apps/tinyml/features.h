/*
 * The features the "tinyml" model reads, from a window of WINDOW samples of
 * die_temp (millidegrees, oldest first) taken every SAMPLE_MS, in integers
 * that fit an int16_t.
 * train.py computes the same features in Python to train the model, and
 * tests/unit/test_tinyml.c checks that both agree.
 *
 *   features[0]  slope of the least-squares line, in tenths of a degree per minute
 *   features[1]  max - min, in tenths of a degree
 *   features[2]  mean step from one sample to the next (its absolute value),
 *                in hundredths of a degree
 *
 * Not the temperature itself: the behaviour is the same at 30 and at 70 C.
 */
#pragma once
#include <stdint.h>

#define WINDOW 12        /* one minute at the default period */
#define SAMPLE_MS 5000   /* the period the model reads the window at */
#define N_FEATURES 3

/* Divisions round toward zero, as C's do; train.py rounds the same way. */
static inline int64_t tml_clamp16(int64_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

static inline void tml_features(const int32_t *s, int16_t *f)
{
    int64_t steps = 0, num = 0;
    int32_t lo = s[0], hi = s[0];
    for (int32_t i = 0; i < WINDOW; i++) {
        if (i > 0) {
            steps += s[i] > s[i - 1] ? (int64_t)s[i] - s[i - 1] : (int64_t)s[i - 1] - s[i];
        }
        /* Weights 2i - (WINDOW - 1): centred, so num / 286 is the slope per
         * sample for WINDOW 12 (the sum of their squares is 572). */
        num += (int64_t)(2 * i - (WINDOW - 1)) * s[i];
        lo = s[i] < lo ? s[i] : lo;
        hi = s[i] > hi ? s[i] : hi;
    }
    f[0] = (int16_t)tml_clamp16(num * 600 / (286 * SAMPLE_MS));
    f[1] = (int16_t)tml_clamp16(((int64_t)hi - lo) / 100);
    f[2] = (int16_t)tml_clamp16(steps / ((WINDOW - 1) * 10));
}

/* The model's prediction for integer features. emlearn's C goes left where
 * a feature is less than a threshold, scikit-learn's where it is less or
 * equal; the thresholds lie between integers or on one (24.0 when the
 * training data had 23 and 25). A quarter less makes the two the same for an
 * integer feature, so the model on the device says what it said in train.py.
 * After #include "model.h". */
#define model_predict_int(f)                                                                    \
    model_predict((const float[N_FEATURES]){(f)[0] - 0.25f, (f)[1] - 0.25f, (f)[2] - 0.25f}, N_FEATURES)
