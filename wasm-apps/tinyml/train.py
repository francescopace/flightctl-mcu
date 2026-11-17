#!/usr/bin/env python3
"""Trains the "tinyml" app's model and writes it as C with emlearn.

A random forest that tells, from a minute of the chip's temperature
(features.h: its slope, its spread and
how much it moves from one sample to the next), whether it is steady, warming, cooling
or unstable (swinging up and down, as with a fan that cycles). The data are
synthetic: windows drawn from each of the four behaviours, with noise and the
sensor's resolution, from a fixed seed. The features are computed as
features.h does, in integers.

Writes, next to this script:
  model.h    the model, as emlearn writes it (method "inline": C with only
             stdint.h), with model_predict(). Its thresholds are floats:
             emlearn's default, int16_t, truncates them (scikit-learn's
             x <= 12.5 becomes x < 12). features.h's model_predict_int()
             makes its x < t the same as scikit-learn's x <= t.
  vectors.h  test windows, their features and scikit-learn's prediction, for
             tests/unit/test_tinyml.c

Needs the versions in requirements.txt (setuptools: emlearn imports distutils,
which Python 3.12 and later no longer have):
  python3 -m venv /tmp/tinyml && /tmp/tinyml/bin/pip install -r wasm-apps/tinyml/requirements.txt
  /tmp/tinyml/bin/python wasm-apps/tinyml/train.py
With these versions, on Python 3.14 on macOS arm64, it wrote the committed files
byte for byte; another platform may round the synthetic data otherwise.
"""
import os
import sys

import emlearn
import numpy as np
import sklearn
from sklearn.ensemble import RandomForestClassifier

WINDOW = 12        # as features.h
SAMPLE_MS = 5000
CLASSES = ["steady", "warming", "cooling", "unstable"]
PER_CLASS = 600
TREES = 5
DEPTH = 5
SEED = 20261117
HERE = os.path.dirname(os.path.abspath(__file__))


def tdiv(a, b):
    """a / b rounded toward zero, as C's integer division."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def clamp16(v):
    return max(-32768, min(32767, v))


def features(s):
    """features.h's tml_features, on a window of WINDOW millidegree samples."""
    s = [int(x) for x in s]
    num = sum((2 * i - (WINDOW - 1)) * x for i, x in enumerate(s))
    steps = sum(abs(s[i] - s[i - 1]) for i in range(1, WINDOW))
    return [clamp16(tdiv(num * 600, 286 * SAMPLE_MS)),
            clamp16(tdiv(max(s) - min(s), 100)),
            clamp16(tdiv(steps, (WINDOW - 1) * 10))]


def window(rng, cls):
    """WINDOW samples of die_temp in millidegrees, for one behaviour."""
    t = np.arange(WINDOW) * SAMPLE_MS / 60000.0   # minutes
    base = rng.uniform(20.0, 80.0)
    if cls == "steady":
        temp = base + rng.uniform(-0.5, 0.5) * t
    elif cls == "warming":
        temp = base + rng.uniform(1.5, 15.0) * t
    elif cls == "cooling":
        temp = base - rng.uniform(1.5, 15.0) * t
    else:  # unstable: a swing of 1.5 to 6 degrees every 15 to 60 s
        amp, period = rng.uniform(1.5, 6.0), rng.uniform(0.25, 1.0)
        temp = base + rng.uniform(-0.5, 0.5) * t + amp * np.sin(2 * np.pi * t / period + rng.uniform(0, 2 * np.pi))
    temp = temp + rng.normal(0.0, rng.uniform(0.05, 0.4), WINDOW)
    step = rng.choice([1, 100, 250, 500])          # the sensor's resolution, in millidegrees
    return [int(round(v * 1000 / step)) * step for v in temp]


def dataset(rng, n):
    wins, labels = [], []
    for _ in range(n):
        for c, name in enumerate(CLASSES):
            wins.append(window(rng, name))
            labels.append(c)
    return wins, np.array(labels)


def c_array(ctype, name, rows):
    out = ["static const %s %s[][%d] = {" % (ctype, name, len(rows[0]))]
    out += ["    {%s}," % ", ".join(str(v) for v in r) for r in rows]
    out.append("};")
    return "\n".join(out)


def majority(model, x):
    """The class most of the trees vote for, the lowest on a tie: what
    emlearn's model_predict() computes. scikit-learn's own predict() averages
    the trees' probabilities instead, and parts from it on a close vote."""
    votes = np.zeros((len(x), len(CLASSES)), dtype=int)
    for e in model.estimators_:
        votes[np.arange(len(x)), e.predict(x.astype(np.float32)).astype(int)] += 1
    return votes.argmax(axis=1)


def main():
    rng = np.random.default_rng(SEED)
    train_w, train_y = dataset(rng, PER_CLASS)
    test_w, test_y = dataset(rng, 50)
    train_x = np.array([features(w) for w in train_w], dtype=np.int16)
    test_x = np.array([features(w) for w in test_w], dtype=np.int16)

    model = RandomForestClassifier(n_estimators=TREES, max_depth=DEPTH, random_state=SEED)
    model.fit(train_x, train_y)
    pred = majority(model, test_x)
    print("emlearn %s, scikit-learn %s, numpy %s: %d trees of depth %d, %d nodes, test accuracy %.3f"
          % (emlearn.__version__, sklearn.__version__, np.__version__, TREES, DEPTH,
             sum(e.tree_.node_count for e in model.estimators_), (pred == test_y).mean()), file=sys.stderr)

    emlearn.convert(model, method="inline", dtype="float").save(file=os.path.join(HERE, "model.h"), name="model")

    # Every threshold of the forest, probed just below, at and just above it,
    # with the other features from a test window: where emlearn's comparison
    # and scikit-learn's could part.
    probes = []
    for e in model.estimators_:
        for f, thr in zip(e.tree_.feature, e.tree_.threshold):
            if f < 0:
                continue
            for d in (-1, 0, 1):
                x = list(test_x[len(probes) % len(test_x)])
                x[f] = int(np.floor(thr)) + d
                probes.append(x)
    probe_x = np.array(probes, dtype=np.int16)

    with open(os.path.join(HERE, "vectors.h"), "w") as out:
        out.write("/* Generated by wasm-apps/tinyml/train.py (emlearn %s, scikit-learn %s, numpy %s): do not edit.\n"
                  " * Test windows (millidegrees), their features (features.h) and the class most\n"
                  " * of scikit-learn's trees vote for, then probes around every threshold. */\n"
                  % (emlearn.__version__, sklearn.__version__, np.__version__))
        out.write("#pragma once\n#include <stdint.h>\n\n")
        out.write("static const char *const tml_classes[] = {%s};\n\n"
                  % ", ".join('"%s"' % c for c in CLASSES))
        out.write(c_array("int32_t", "tml_windows", test_w) + "\n\n")
        out.write(c_array("int16_t", "tml_window_features", test_x.tolist()) + "\n\n")
        out.write("static const int32_t tml_window_class[] = {%s};\n\n"
                  % ", ".join(str(int(c)) for c in pred))
        out.write(c_array("int16_t", "tml_probes", probe_x.tolist()) + "\n\n")
        out.write("static const int32_t tml_probe_class[] = {%s};\n"
                  % ", ".join(str(int(c)) for c in majority(model, probe_x)))


if __name__ == "__main__":
    main()
