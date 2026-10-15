/*
 * A minimal assertion harness. The project has no test framework and
 * pulling one in for a tool that builds with a single cc invocation would
 * cost more than it gives, so this is a header of macros and two counters.
 */
#ifndef FRAS_TEST_HARNESS_H
#define FRAS_TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern int tests_run;
extern int tests_failed;

#define DEFINE_HARNESS_COUNTERS() int tests_run = 0; int tests_failed = 0

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        tests_run++;                                                       \
        if (!(cond)) {                                                     \
            tests_failed++;                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, "\n");                                         \
        }                                                                  \
    } while (0)

#define CHECK_INT(got, want, ...)                                          \
    do {                                                                   \
        long long g_ = (long long)(got), w_ = (long long)(want);           \
        tests_run++;                                                       \
        if (g_ != w_) {                                                    \
            tests_failed++;                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, " -- got %lld, want %lld\n", g_, w_);          \
        }                                                                  \
    } while (0)

/* Relative tolerance, because these are ratios that can legitimately be
 * tiny: an absolute epsilon would accept 0.0 for a small-but-correct
 * value, which is exactly the wrong answer worth catching. */
#define CHECK_NEAR(got, want, ...)                                         \
    do {                                                                   \
        double g_ = (double)(got), w_ = (double)(want);                    \
        double scale_ = fabs(w_) > 1.0 ? fabs(w_) : 1.0;                   \
        tests_run++;                                                       \
        if (!(fabs(g_ - w_) <= 1e-9 * scale_)) {                           \
            tests_failed++;                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, " -- got %.10f, want %.10f\n", g_, w_);        \
        }                                                                  \
    } while (0)

#define CHECK_STR(got, want, ...)                                          \
    do {                                                                   \
        const char *g_ = (got), *w_ = (want);                              \
        tests_run++;                                                       \
        if (!g_ || !w_ || strcmp(g_, w_) != 0) {                           \
            tests_failed++;                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fprintf(stderr, " -- got \"%s\", want \"%s\"\n",               \
                    g_ ? g_ : "(null)", w_ ? w_ : "(null)");               \
        }                                                                  \
    } while (0)

#endif
