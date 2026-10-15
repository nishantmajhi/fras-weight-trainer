/*
 * Tests for src/fit.c -- the grid search over the weight simplex.
 *
 * These build training sets by hand, small enough that the right answer is
 * arguable from first principles rather than from running the code.
 */
#include "harness.h"
#include "fit.h"

#include <stdlib.h>

/* Builds a one-session training set from a literal candidate array. */
static training_session make_session(candidate_score *cands, size_t n) {
    training_session s;
    s.profile = NULL;
    s.candidates = cands;
    s.candidate_count = n;
    return s;
}

static void test_evaluate_ranking(void) {
    /* Candidate 0 is chosen and has the best prefix; candidate 1 has the
     * best LCS. Which one ranks first depends entirely on the weights. */
    candidate_score c[] = {
        /* prefix, lcs, lev, chosen */
        {1.0, 0.0, 0.0, 1},
        {0.0, 1.0, 0.0, 0},
    };
    training_session s = make_session(c, 2);
    size_t idx[] = {0};

    eval_result all_prefix = fras_evaluate(&s, idx, 1, 1.0, 0.0, 0.0);
    CHECK_NEAR(all_prefix.mrr, 1.0, "chosen candidate ranks first on prefix weighting");
    CHECK_NEAR(all_prefix.accuracy, 1.0, "top-1 accuracy is 1");

    eval_result all_lcs = fras_evaluate(&s, idx, 1, 0.0, 1.0, 0.0);
    CHECK_NEAR(all_lcs.mrr, 0.5, "chosen candidate ranks second on LCS weighting");
    CHECK_NEAR(all_lcs.accuracy, 0.0, "top-1 accuracy is 0");
}

/* Ties are resolved in the chosen candidate's favour, which is the best
 * case a stable sort could produce depending on record order. */
static void test_ties_favour_the_chosen_candidate(void) {
    candidate_score c[] = {
        {0.5, 0.5, 0.5, 1},
        {0.5, 0.5, 0.5, 0},
        {0.5, 0.5, 0.5, 0},
    };
    training_session s = make_session(c, 3);
    size_t idx[] = {0};

    eval_result r = fras_evaluate(&s, idx, 1, 0.4, 0.3, 0.3);
    CHECK_NEAR(r.mrr, 1.0, "a three-way tie still ranks the chosen candidate first");
    CHECK_NEAR(r.accuracy, 1.0, "and counts as a top-1 hit");
}

/* More than one candidate may be marked chosen; the best-ranked one is
 * what counts. */
static void test_multiple_chosen(void) {
    candidate_score c[] = {
        {0.0, 0.0, 0.0, 1},  /* chosen but hopeless */
        {1.0, 1.0, 1.0, 1},  /* chosen and best */
        {0.5, 0.5, 0.5, 0},
    };
    training_session s = make_session(c, 3);
    size_t idx[] = {0};

    eval_result r = fras_evaluate(&s, idx, 1, 0.34, 0.33, 0.33);
    CHECK_NEAR(r.mrr, 1.0, "the best-ranked chosen candidate is the one scored");
}

static void test_mrr_averages_over_sessions(void) {
    /* Session A: chosen ranks 1. Session B: chosen ranks 2.
     * MRR = (1/1 + 1/2) / 2 = 0.75, accuracy = 0.5. */
    candidate_score a[] = {{1.0, 0.0, 0.0, 1}, {0.0, 0.0, 0.0, 0}};
    candidate_score b[] = {{0.0, 0.0, 0.0, 1}, {1.0, 0.0, 0.0, 0}};
    training_session sessions[] = {make_session(a, 2), make_session(b, 2)};
    size_t idx[] = {0, 1};

    eval_result r = fras_evaluate(sessions, idx, 2, 1.0, 0.0, 0.0);
    CHECK_NEAR(r.mrr, 0.75, "MRR averages reciprocal ranks across sessions");
    CHECK_NEAR(r.accuracy, 0.5, "accuracy is the fraction ranked first");
}

/* The index array selects a subset, which is how per-profile fits are
 * computed without copying the sessions. */
static void test_index_subset(void) {
    candidate_score a[] = {{1.0, 0.0, 0.0, 1}, {0.0, 0.0, 0.0, 0}};
    candidate_score b[] = {{0.0, 0.0, 0.0, 1}, {1.0, 0.0, 0.0, 0}};
    training_session sessions[] = {make_session(a, 2), make_session(b, 2)};

    size_t only_first[] = {0};
    CHECK_NEAR(fras_evaluate(sessions, only_first, 1, 1.0, 0.0, 0.0).mrr, 1.0,
               "subset of one evaluates only that session");

    size_t only_second[] = {1};
    CHECK_NEAR(fras_evaluate(sessions, only_second, 1, 1.0, 0.0, 0.0).mrr, 0.5,
               "a different subset gives a different answer");
}

static void test_is_better_ordering(void) {
    /* Nothing beats having no incumbent. */
    CHECK_INT(fras_is_better(0.0, 0.0, 5, 5, 1.0, 1.0, 0, 0, 0), 1, "first result always wins");

    /* Higher MRR wins. */
    CHECK_INT(fras_is_better(0.9, 0.0, 0, 0, 0.8, 1.0, 0, 0, 1), 1, "higher MRR wins");
    CHECK_INT(fras_is_better(0.7, 1.0, 0, 0, 0.8, 0.0, 0, 0, 1), 0, "lower MRR loses");

    /* Equal MRR: higher accuracy wins. */
    CHECK_INT(fras_is_better(0.8, 0.9, 5, 5, 0.8, 0.8, 0, 0, 1), 1, "accuracy breaks an MRR tie");
    CHECK_INT(fras_is_better(0.8, 0.7, 0, 0, 0.8, 0.8, 5, 5, 1), 0, "lower accuracy loses the tie");

    /* Equal on both: smallest i, then smallest j. This is what makes the
     * result independent of how the grid was split across threads. */
    CHECK_INT(fras_is_better(0.8, 0.8, 1, 9, 0.8, 0.8, 2, 0, 1), 1, "smaller i breaks a full tie");
    CHECK_INT(fras_is_better(0.8, 0.8, 2, 0, 0.8, 0.8, 1, 9, 1), 0, "larger i loses a full tie");
    CHECK_INT(fras_is_better(0.8, 0.8, 1, 3, 0.8, 0.8, 1, 4, 1), 1, "smaller j breaks an i tie");
    CHECK_INT(fras_is_better(0.8, 0.8, 1, 4, 0.8, 0.8, 1, 3, 1), 0, "larger j loses an i tie");

    /* Differences below the epsilon count as equal. */
    CHECK_INT(fras_is_better(0.8 + 1e-15, 0.8, 5, 5, 0.8, 0.8, 1, 1, 1), 0,
              "a sub-epsilon MRR difference does not win");
}

/* The chosen candidate can only be ranked first by putting essentially all
 * the weight on prefix: the rival is very slightly worse on prefix but
 * perfect on the other two, so any weight spent elsewhere loses.
 *
 * Note what this test had to be built to avoid. A rival that is merely
 * *neutral* on levenshtein does not pin the answer down, because the
 * search then has a whole family of winning triples and the tie-break
 * hands the spare weight to levenshtein -- a correct result that looks
 * wrong. The discriminating rival is what makes the expectation unique. */
static void test_fit_finds_the_discriminating_weight(void) {
    candidate_score a[] = {{1.0, 0.0, 0.0, 1}, {0.99, 1.0, 1.0, 0}};
    candidate_score b[] = {{1.0, 0.0, 0.0, 1}, {0.99, 1.0, 1.0, 0}};
    training_session sessions[] = {make_session(a, 2), make_session(b, 2)};
    size_t idx[] = {0, 1};

    fit_result r = fras_fit_weights(sessions, idx, 2, 0.1, 1);
    CHECK_INT(r.ok, 1, "fit succeeds");
    CHECK_NEAR(r.prefix, 1.0, "all weight goes to prefix");
    CHECK_NEAR(r.lcs, 0.0, "no weight on lcs");
    CHECK_NEAR(r.levenshtein, 0.0, "no weight on levenshtein");
    CHECK_NEAR(r.mrr, 1.0, "and it gets every session right");
}

/* When several weight triples score identically, the smallest (i, j) wins.
 * Here the rival is neutral on levenshtein, so every triple with
 * prefix >= lcs ranks the chosen candidate first -- and the search must
 * return the lexicographically smallest of them, which is all the weight
 * on levenshtein. */
static void test_fit_tie_break_is_lexicographic(void) {
    candidate_score a[] = {{1.0, 0.0, 0.0, 1}, {0.0, 1.0, 0.0, 0}};
    training_session sessions[] = {make_session(a, 2)};
    size_t idx[] = {0};

    fit_result r = fras_fit_weights(sessions, idx, 1, 0.1, 1);
    CHECK_INT(r.ok, 1, "fit succeeds");
    CHECK_NEAR(r.mrr, 1.0, "the chosen candidate is ranked first");
    CHECK_NEAR(r.prefix, 0.0, "smallest prefix index wins the tie");
    CHECK_NEAR(r.lcs, 0.0, "then the smallest lcs index");
    CHECK_NEAR(r.levenshtein, 1.0, "leaving the remainder on levenshtein");
}

/* The fitted triple must not depend on the thread count. This is the
 * property the deterministic tie-break exists for, and it is the kind of
 * thing that only shows up on a machine with a different core count. */
static void test_result_is_independent_of_thread_count(void) {
    candidate_score a[] = {{0.9, 0.2, 0.4, 1}, {0.8, 0.9, 0.3, 0}, {0.1, 0.1, 0.9, 0}};
    candidate_score b[] = {{0.3, 0.8, 0.5, 0}, {0.7, 0.4, 0.6, 1}, {0.2, 0.7, 0.2, 0}};
    candidate_score c[] = {{0.5, 0.5, 0.5, 1}, {0.6, 0.4, 0.5, 0}};
    training_session sessions[] = {make_session(a, 3), make_session(b, 3), make_session(c, 2)};
    size_t idx[] = {0, 1, 2};

    fit_result one = fras_fit_weights(sessions, idx, 3, 0.05, 1);
    CHECK_INT(one.ok, 1, "single-threaded fit succeeds");

    int counts[] = {2, 3, 4, 7, 16, 64};
    for (size_t k = 0; k < sizeof counts / sizeof counts[0]; k++) {
        fit_result many = fras_fit_weights(sessions, idx, 3, 0.05, counts[k]);
        CHECK_INT(many.ok, 1, "fit with %d threads succeeds", counts[k]);
        CHECK_NEAR(many.prefix, one.prefix, "prefix weight matches with %d threads", counts[k]);
        CHECK_NEAR(many.lcs, one.lcs, "lcs weight matches with %d threads", counts[k]);
        CHECK_NEAR(many.levenshtein, one.levenshtein, "lev weight matches with %d threads", counts[k]);
        CHECK_NEAR(many.mrr, one.mrr, "MRR matches with %d threads", counts[k]);
    }
}

/* More threads than grid rows: the extra workers get empty slices and must
 * be left out of the merge rather than contributing a zeroed result. */
static void test_more_threads_than_work(void) {
    candidate_score a[] = {{1.0, 0.0, 0.0, 1}, {0.0, 1.0, 0.0, 0}};
    training_session sessions[] = {make_session(a, 2)};
    size_t idx[] = {0};

    /* step 1.0 gives N = 1, so only two grid rows exist. */
    fit_result r = fras_fit_weights(sessions, idx, 1, 1.0, 32);
    CHECK_INT(r.ok, 1, "fit succeeds with far more threads than rows");
    CHECK_NEAR(r.prefix + r.lcs + r.levenshtein, 1.0, "weights still sum to one");
    CHECK_NEAR(r.mrr, 1.0, "and the answer is still right");
}

static void test_weights_always_sum_to_one(void) {
    candidate_score a[] = {{0.4, 0.6, 0.5, 1}, {0.5, 0.5, 0.5, 0}};
    training_session sessions[] = {make_session(a, 2)};
    size_t idx[] = {0};

    double steps[] = {0.5, 0.25, 0.1, 0.05, 0.02};
    for (size_t k = 0; k < sizeof steps / sizeof steps[0]; k++) {
        fit_result r = fras_fit_weights(sessions, idx, 1, steps[k], 2);
        CHECK_INT(r.ok, 1, "fit succeeds at step %g", steps[k]);
        CHECK_NEAR(r.prefix + r.lcs + r.levenshtein, 1.0, "weights sum to one at step %g", steps[k]);
        CHECK(r.prefix >= 0.0 && r.lcs >= 0.0 && r.levenshtein >= 0.0,
              "no negative weight at step %g", steps[k]);
    }
}

static void test_empty_session_set_is_refused(void) {
    fit_result r = fras_fit_weights(NULL, NULL, 0, 0.1, 1);
    CHECK_INT(r.ok, 0, "fitting zero sessions reports failure rather than dividing by zero");
}

void run_fit_tests(void) {
    test_evaluate_ranking();
    test_ties_favour_the_chosen_candidate();
    test_multiple_chosen();
    test_mrr_averages_over_sessions();
    test_index_subset();
    test_is_better_ordering();
    test_fit_finds_the_discriminating_weight();
    test_fit_tie_break_is_lexicographic();
    test_result_is_independent_of_thread_count();
    test_more_threads_than_work();
    test_weights_always_sum_to_one();
    test_empty_session_set_is_refused();
}
