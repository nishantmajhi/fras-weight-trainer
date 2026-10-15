/*
 * Test entry point. Each module contributes a run_*_tests() function and
 * the shared counters live here.
 *
 * Output is unbuffered so that a run against unfixed code -- which is
 * exactly when a segfault part way through is likely -- still shows which
 * check it had reached.
 */
#include "harness.h"

DEFINE_HARNESS_COUNTERS();

void run_scoring_tests(void);
void run_json_tests(void);
void run_fit_tests(void);
void run_vector_tests(const char *path);

int main(int argc, char **argv) {
    const char *vectors = (argc > 1) ? argv[1] : "tests/vectors.json";

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    int before;

    before = tests_run; run_scoring_tests();
    printf("scoring:  %d checks\n", tests_run - before);

    before = tests_run; run_json_tests();
    printf("json:     %d checks\n", tests_run - before);

    before = tests_run; run_fit_tests();
    printf("fit:      %d checks\n", tests_run - before);

    before = tests_run; run_vector_tests(vectors);
    printf("vectors:  %d checks\n", tests_run - before);

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
