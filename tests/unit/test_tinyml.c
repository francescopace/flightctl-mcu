/*
 * The "tinyml" app's model (wasm-apps/tinyml), compiled for the host: the C
 * that emlearn wrote agrees with scikit-learn, and features.h with the
 * features train.py trained it on. The vectors are train.py's (vectors.h).
 * test_apps.c runs the app itself, as WebAssembly.
 */
#include "test.h"

#include "../../wasm-apps/tinyml/features.h"
#include "../../wasm-apps/tinyml/model.h"
#include "../../wasm-apps/tinyml/vectors.h"

#define N(a) (sizeof(a) / sizeof((a)[0]))

/* As the app calls it (features.h). */
static int32_t predict(const int16_t *f)
{
    return model_predict_int(f);
}

/* features.h computes from each test window what train.py computed. */
static void the_features_are_the_ones_trained_on(void)
{
    for (size_t i = 0; i < N(tml_windows); i++) {
        int16_t f[N_FEATURES];
        tml_features(tml_windows[i], f);
        for (int j = 0; j < N_FEATURES; j++) {
            if (f[j] != tml_window_features[i][j]) {
                fprintf(stderr, "  window %zu, feature %d: %d, expected %d\n", i, j, f[j],
                        tml_window_features[i][j]);
                t_failures++;
            }
        }
    }
}

/* The model as emlearn wrote it predicts what scikit-learn does, on the
 * test windows and just below, at and just above every threshold. */
static void the_model_predicts_as_scikit_learn(void)
{
    size_t bad = 0;
    for (size_t i = 0; i < N(tml_window_features); i++) {
        bad += predict(tml_window_features[i]) != tml_window_class[i];
    }
    for (size_t i = 0; i < N(tml_probes); i++) {
        bad += predict(tml_probes[i]) != tml_probe_class[i];
    }
    CHECK_INT(bad, 0);
    CHECK(N(tml_probes) > 100);
}

/* Each behaviour is in the test windows, and the app's names are train.py's. */
static void every_class_is_tested(void)
{
    int seen[4] = {0};
    for (size_t i = 0; i < N(tml_window_class); i++) {
        CHECK(tml_window_class[i] >= 0 && tml_window_class[i] < 4);
        seen[tml_window_class[i] & 3]++;
    }
    for (int c = 0; c < 4; c++) {
        CHECK(seen[c] > 0);
    }
    CHECK_INT(N(tml_classes), 4);
    CHECK_STR(tml_classes[1], "warming");
}

int main(void)
{
    RUN(the_features_are_the_ones_trained_on);
    RUN(the_model_predicts_as_scikit_learn);
    RUN(every_class_is_tested);
    return TEST_RESULT();
}
