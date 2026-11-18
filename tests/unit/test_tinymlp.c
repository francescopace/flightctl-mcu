/*
 * The "tinymlp" app's network (wasm-apps/tinymlp), compiled for the host:
 * the C that emlearn wrote, with the host's expf, logf and tanhf, gives
 * scikit-learn's class on tinyml's test windows, as the app computes its
 * input from them. test_apps.c runs the app itself, as WebAssembly, with
 * the device's functions.
 */
#include "test.h"

#include "../../wasm-apps/tinyml/features.h"
#include "../../wasm-apps/tinyml/vectors.h"
#include "../../wasm-apps/tinymlp/model.h"
#include "../../wasm-apps/tinymlp/vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* As the app computes it: the features in hundredths. */
static int32_t predict(const int16_t *f, float *p)
{
    float in[N_FEATURES];
    for (int i = 0; i < N_FEATURES; i++) {
        in[i] = f[i] * 0.01f;
    }
    CHECK_INT(eml_net_regress(&mlp, in, N_FEATURES, p, 4), EmlOk);
    return mlp_predict(in, N_FEATURES);
}

static void the_network_predicts_as_scikit_learn(void)
{
    CHECK_INT((int)N(mlp_window_class), (int)N(tml_window_features));
    size_t bad = 0;
    int seen[4] = {0};
    for (size_t i = 0; i < N(tml_window_features); i++) {
        float p[4];
        int32_t c = predict(tml_window_features[i], p);
        bad += c != mlp_window_class[i];
        seen[c & 3]++;
        /* The softmax: probabilities that add up to 1. */
        float sum = p[0] + p[1] + p[2] + p[3];
        CHECK(sum > 0.999f && sum < 1.001f && p[c] >= p[0] && p[c] >= p[1] && p[c] >= p[2] && p[c] >= p[3]);
    }
    CHECK_INT(bad, 0);
    for (int c = 0; c < 4; c++) {
        CHECK(seen[c] > 0);
    }
}

int main(void)
{
    RUN(the_network_predicts_as_scikit_learn);
    return TEST_RESULT();
}
