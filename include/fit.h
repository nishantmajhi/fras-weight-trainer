/*
 * fit.h - the weight-fitting decision, separated from the CLI.
 *
 * Everything here works on precomputed per-candidate scores and knows
 * nothing about JSON, files, or argv. That split is what makes the search
 * testable: a training set can be built as a literal in a test and fitted
 * without a file on disk, and the ranking rules (how ties are resolved,
 * how a winner is chosen across threads) can be asserted directly.
 */
#ifndef FRAS_FIT_H
#define FRAS_FIT_H

#include <stddef.h>

/* One candidate in a labelled session, already scored against the query.
 * The three components do not depend on the weights, which is the whole
 * reason the grid search is cheap. */
typedef struct {
    double prefix;
    double lcs;
    double levenshtein;
    int chosen;
} candidate_score;

typedef struct {
    char *profile; /* NULL if the session has no profile tag */
    candidate_score *candidates;
    size_t candidate_count;
} training_session;

typedef struct {
    double mrr;
    double accuracy;
} eval_result;

typedef struct {
    double prefix, lcs, levenshtein;
    double mrr, accuracy;
    int ok; /* 0 if the fit could not be performed (allocation failure) */
} fit_result;

/* Evaluates one weight triple against the sessions named by idx[0..n).
 * Ranks each session's candidates by the weighted sum and scores the
 * best-ranked chosen candidate: rank 1 means the right answer came top.
 * Ties count in the chosen candidate's favour, matching the best case a
 * stable sort could produce depending on record order.
 *
 * n must be greater than zero. */
eval_result fras_evaluate(const training_session *all, const size_t *idx, size_t n,
                          double wp, double wl, double wv);

/* Deterministic comparison used to pick a winner: higher MRR first, then
 * higher accuracy, then the lexicographically smallest (i, j). The last
 * clause is what makes the result independent of how the grid was split
 * across threads. Returns 1 if the first triple should win. */
int fras_is_better(double mrr, double acc, long i, long j,
                   double best_mrr, double best_acc, long best_i, long best_j, int have_best);

/* Exhaustive search over the weight simplex at resolution `step`, split
 * over `threads` workers. Returns a result whose `ok` is 0 if a worker
 * array could not be allocated. */
fit_result fras_fit_weights(const training_session *all, const size_t *idx, size_t n,
                            double step, int threads);

#endif
