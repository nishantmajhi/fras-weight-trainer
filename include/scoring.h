/*
 * scoring.h - UTF-8 aware string-similarity primitives that reproduce what
 * FRAS::look() computes in src/FRAS.php, so weights fitted by this trainer
 * are valid for the ranking that actually runs in production.
 *
 * "Reproduce" is meant literally, and it is checked: tests/vectors.json
 * holds (query, field) -> {prefix, lcs, levenshtein} expectations that the
 * C suite asserts against and that tools/check_php.php replays through the
 * real PHP class. See tests/README.md for the one place the two still
 * disagree and why that disagreement is not this project's to fix.
 *
 * FRAS's default construction (no $enableShortCircuit) always computes
 * exact LCS/Levenshtein, so that's what the trainer computes too -- fitting
 * weights against approximated scores would teach the wrong lesson.
 */
#ifndef FRAS_SCORING_H
#define FRAS_SCORING_H

#include <stddef.h>
#include <stdint.h>

/* A decoded, lower-cased Unicode string: one array slot per codepoint
 * (mirrors PHP's mb_str_split, which is what src/FRAS.php uses). */
typedef struct {
    uint32_t *cp;
    size_t len;
} ustr;

/* Decodes UTF-8 `s` into codepoints and lower-cases each one, exactly as
 * PHP's mb_strtolower() + mb_str_split() pair does:
 *
 *   - Case folding uses the full simple lowercase mapping generated from
 *     mbstring itself (see tools/gen_unicode_tables.php), not an ASCII or
 *     Latin-1 approximation, so Greek, Cyrillic and the rest fold too.
 *   - The one context-sensitive rule mbstring applies is reproduced:
 *     Greek capital sigma lower-cases to U+03C2 at the end of a word and
 *     U+03C3 elsewhere.
 *   - U+0130 expands to two codepoints (U+0069 U+0307), which makes the
 *     lower-cased string *longer* than the input. Since string length is
 *     the denominator of both the LCS and Levenshtein scores, that
 *     expansion has to be reproduced or the scores drift.
 *   - Undecodable bytes become mbstring's substitute character '?' -- one
 *     per maximal invalid subpart, which is not the same as one per byte.
 *
 * Returns a ustr whose `cp` is NULL if allocation failed; check with
 * ustr_ok() before use. A failed ustr is safe to pass to ustr_free().
 */
ustr ustr_decode_lower(const char *s);
void ustr_free(ustr *u);
int ustr_ok(const ustr *u);

/* The three raw, per-pair-normalized similarity components FRAS blends,
 * each already in [0,1] (or very close to it for the prefix token bonus).
 * Computing these once per (query, field) pair -- independent of weights
 * -- is what lets the trainer's grid search be cheap: only a weighted sum
 * is redone per candidate weight combination. */
typedef struct {
    double prefix;
    double lcs;
    double levenshtein;
} fras_scores;

/* Writes the three components for `query` against `field`. Returns 0 if a
 * dynamic-programming buffer could not be allocated, in which case *out is
 * untouched. */
int fras_score_pair(const ustr *query, const ustr *field, fras_scores *out);

#endif
