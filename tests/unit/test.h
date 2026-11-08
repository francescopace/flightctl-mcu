/*
 * Minimal unit test helpers: no framework to install, works with any C
 * compiler. Each test file is its own executable (it often #includes the
 * source it tests, to reach static functions) and returns non-zero if a
 * check failed.
 */
#pragma once

#include <stdio.h>
#include <string.h>

static int t_failures;

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            fprintf(stderr, "  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);                       \
            t_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)

#define CHECK_INT(a, b)                                                                                      \
    do {                                                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                                                  \
        if (_a != _b) {                                                                                      \
            fprintf(stderr, "  %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #a, _a, _b);         \
            t_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)

#define CHECK_STR(a, b)                                                                                      \
    do {                                                                                                     \
        const char *_a = (a), *_b = (b);                                                                     \
        if (_a == NULL || _b == NULL || strcmp(_a, _b) != 0) {                                               \
            fprintf(stderr, "  %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a,              \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                                 \
            t_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)

#define CHECK_CONTAINS(hay, needle)                                                                          \
    do {                                                                                                     \
        const char *_h = (hay), *_n = (needle);                                                              \
        if (_h == NULL || strstr(_h, _n) == NULL) {                                                          \
            fprintf(stderr, "  %s:%d: %s == \"%s\", expected to contain \"%s\"\n", __FILE__, __LINE__, #hay, \
                    _h ? _h : "(null)", _n);                                                                 \
            t_failures++;                                                                                    \
        }                                                                                                    \
    } while (0)

#define RUN(test)                                                                                            \
    do {                                                                                                     \
        int _before = t_failures;                                                                            \
        test();                                                                                              \
        printf("%s %s\n", t_failures == _before ? "ok  " : "FAIL", #test);                                   \
    } while (0)

#define TEST_RESULT() (t_failures ? (printf("%d check(s) failed\n", t_failures), 1) : 0)
