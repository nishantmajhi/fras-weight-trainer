#include "fit.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

eval_result fras_evaluate(const training_session *all, const size_t *idx, size_t n,
                          double wp, double wl, double wv) {
    double rr_sum = 0.0, acc_sum = 0.0;

    for (size_t s = 0; s < n; s++) {
        const training_session *sess = &all[idx[s]];
        double chosen_best_sim = -1.0;
        for (size_t c = 0; c < sess->candidate_count; c++) {
            if (!sess->candidates[c].chosen) continue;
            const candidate_score *cs = &sess->candidates[c];
            double sim = wp * cs->prefix + wl * cs->lcs + wv * cs->levenshtein;
            if (sim > chosen_best_sim) chosen_best_sim = sim;
        }
        size_t better_count = 0;
        for (size_t c = 0; c < sess->candidate_count; c++) {
            const candidate_score *cs = &sess->candidates[c];
            double sim = wp * cs->prefix + wl * cs->lcs + wv * cs->levenshtein;
            if (sim > chosen_best_sim) better_count++;
        }
        size_t rank = better_count + 1;
        rr_sum += 1.0 / (double)rank;
        if (rank == 1) acc_sum += 1.0;
    }

    eval_result r;
    r.mrr = rr_sum / (double)n;
    r.accuracy = acc_sum / (double)n;
    return r;
}

int fras_is_better(double mrr, double acc, long i, long j,
                   double best_mrr, double best_acc, long best_i, long best_j, int have_best) {
    if (!have_best) return 1;
    const double eps = 1e-12;
    if (mrr > best_mrr + eps) return 1;
    if (mrr < best_mrr - eps) return 0;
    if (acc > best_acc + eps) return 1;
    if (acc < best_acc - eps) return 0;
    if (i != best_i) return i < best_i;
    return j < best_j;
}

typedef struct {
    const training_session *all;
    const size_t *idx;
    size_t n;
    long N;              /* grid resolution: weights are i/N, j/N, (N-i-j)/N */
    long i_start, i_end; /* inclusive range of the outer index this thread owns */
    long best_i, best_j;
    double best_mrr, best_acc;
    int has_result; /* 0 if this slice was empty, so the merge can skip it */
} worker_args;

static void *worker_main(void *arg) {
    worker_args *w = (worker_args *)arg;
    int have_best = w->has_result;
    for (long i = w->i_start; i <= w->i_end; i++) {
        for (long j = 0; i + j <= w->N; j++) {
            long k = w->N - i - j;
            double wp = (double)i / (double)w->N;
            double wl = (double)j / (double)w->N;
            double wv = (double)k / (double)w->N;
            eval_result r = fras_evaluate(w->all, w->idx, w->n, wp, wl, wv);
            if (fras_is_better(r.mrr, r.accuracy, i, j,
                               w->best_mrr, w->best_acc, w->best_i, w->best_j, have_best)) {
                w->best_mrr = r.mrr;
                w->best_acc = r.accuracy;
                w->best_i = i;
                w->best_j = j;
                have_best = 1;
            }
        }
    }
    w->has_result = have_best;
    return NULL;
}

fit_result fras_fit_weights(const training_session *all, const size_t *idx, size_t n,
                            double step, int threads) {
    fit_result result;
    memset(&result, 0, sizeof result);
    if (n == 0) return result;

    long N = (long)lround(1.0 / step);
    if (N < 1) N = 1;
    if (threads < 1) threads = 1;
    if (threads > N + 1) threads = (int)(N + 1);

    pthread_t *tids = malloc((size_t)threads * sizeof(pthread_t));
    worker_args *args = malloc((size_t)threads * sizeof(worker_args));
    int *started = calloc((size_t)threads, sizeof(int));
    if (!tids || !args || !started) { free(tids); free(args); free(started); return result; }

    long total_i = N + 1;
    long chunk = total_i / threads;
    long remainder = total_i % threads;
    long cursor = 0;

    for (int t = 0; t < threads; t++) {
        long this_chunk = chunk + (t < remainder ? 1 : 0);
        args[t].all = all;
        args[t].idx = idx;
        args[t].n = n;
        args[t].N = N;
        args[t].i_start = cursor;
        args[t].i_end = cursor + this_chunk - 1;
        args[t].best_mrr = -1.0;
        args[t].best_acc = -1.0;
        args[t].best_i = 0;
        args[t].best_j = 0;
        args[t].has_result = 0;
        cursor += this_chunk;
    }

    /* A slice whose thread fails to start is run here instead. Skipping it
     * would leave its weight range unsearched while its empty result still
     * took part in the merge below -- a silently wrong fit rather than a
     * visible failure. */
    for (int t = 0; t < threads; t++) {
        if (args[t].i_start > args[t].i_end) continue; /* more threads than work */
        started[t] = (pthread_create(&tids[t], NULL, worker_main, &args[t]) == 0);
        if (!started[t]) worker_main(&args[t]);
    }
    for (int t = 0; t < threads; t++) {
        if (started[t]) pthread_join(tids[t], NULL);
    }
    free(started);

    int have_best = 0;
    double best_mrr = -1.0, best_acc = -1.0;
    long best_i = 0, best_j = 0;
    for (int t = 0; t < threads; t++) {
        if (!args[t].has_result) continue;
        if (fras_is_better(args[t].best_mrr, args[t].best_acc, args[t].best_i, args[t].best_j,
                           best_mrr, best_acc, best_i, best_j, have_best)) {
            best_mrr = args[t].best_mrr;
            best_acc = args[t].best_acc;
            best_i = args[t].best_i;
            best_j = args[t].best_j;
            have_best = 1;
        }
    }

    free(tids);
    free(args);

    if (!have_best) return result; /* result.ok is still 0 */

    result.prefix = (double)best_i / (double)N;
    result.lcs = (double)best_j / (double)N;
    result.levenshtein = (double)(N - best_i - best_j) / (double)N;
    result.mrr = best_mrr;
    result.accuracy = best_acc;
    result.ok = 1;
    return result;
}
