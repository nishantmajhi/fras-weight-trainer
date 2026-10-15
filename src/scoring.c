#include "scoring.h"
#include "unicode_tables.h"

#include <stdlib.h>
#include <string.h>

/* Allocation is routed through these so the test suite can force failures
 * and check that every site handles them. They are plain malloc/calloc
 * unless the translation unit is compiled with overrides. */
#ifndef SCORING_MALLOC
#define SCORING_MALLOC malloc
#endif
#ifndef SCORING_CALLOC
#define SCORING_CALLOC calloc
#endif

/* mbstring replaces undecodable bytes with its substitute character, which
 * is '?' by default -- not U+FFFD. Matching it matters because '?' is an
 * ordinary character that can go on to match a literal '?' in the other
 * string, so choosing a different one shifts scores. */
#define SUBSTITUTE_CHAR 0x003Fu

/* ------------------------------------------------------------------ */
/* Table lookups                                                       */
/* ------------------------------------------------------------------ */

static int in_ranges(const uint32_t (*ranges)[2], size_t count, uint32_t cp) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid][0]) hi = mid;
        else if (cp > ranges[mid][1]) lo = mid + 1;
        else return 1;
    }
    return 0;
}

static int is_cased(uint32_t cp) {
    return in_ranges(fras_cased_ranges, fras_cased_ranges_count, cp);
}

static int is_case_ignorable(uint32_t cp) {
    return in_ranges(fras_case_ignorable_ranges, fras_case_ignorable_ranges_count, cp);
}

static int is_unicode_space(uint32_t cp) {
    return in_ranges(fras_space_ranges, fras_space_ranges_count, cp);
}

/* Simple lowercase mapping; returns cp unchanged when there is none. */
static uint32_t simple_lower(uint32_t cp) {
    size_t lo = 0, hi = fras_lower_map_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < fras_lower_map[mid][0]) hi = mid;
        else if (cp > fras_lower_map[mid][0]) lo = mid + 1;
        else return fras_lower_map[mid][1];
    }
    return cp;
}

/* ------------------------------------------------------------------ */
/* UTF-8 decoding                                                      */
/* ------------------------------------------------------------------ */

/* Decodes one UTF-8 sequence at u[i]. On success writes the codepoint and
 * returns its length in bytes. On failure returns the negated length of
 * the "maximal subpart" -- the longest prefix that was still a possible
 * encoding -- which is how many bytes collapse into a single substitute
 * character. That rule is what mbstring implements, and getting it wrong
 * changes string *lengths*, which feed every normalisation denominator. */
static int decode_one(const unsigned char *u, size_t len, size_t i, uint32_t *out) {
    unsigned char b0 = u[i];

    if (b0 < 0x80) { *out = b0; return 1; }

    unsigned char lo2, hi2;
    int seq_len;
    if (b0 >= 0xC2 && b0 <= 0xDF)      { seq_len = 2; lo2 = 0x80; hi2 = 0xBF; }
    else if (b0 == 0xE0)               { seq_len = 3; lo2 = 0xA0; hi2 = 0xBF; }
    else if (b0 >= 0xE1 && b0 <= 0xEC) { seq_len = 3; lo2 = 0x80; hi2 = 0xBF; }
    else if (b0 == 0xED)               { seq_len = 3; lo2 = 0x80; hi2 = 0x9F; }
    else if (b0 >= 0xEE && b0 <= 0xEF) { seq_len = 3; lo2 = 0x80; hi2 = 0xBF; }
    else if (b0 == 0xF0)               { seq_len = 4; lo2 = 0x90; hi2 = 0xBF; }
    else if (b0 >= 0xF1 && b0 <= 0xF3) { seq_len = 4; lo2 = 0x80; hi2 = 0xBF; }
    else if (b0 == 0xF4)               { seq_len = 4; lo2 = 0x80; hi2 = 0x8F; }
    else return -1; /* C0, C1, F5-FF and stray continuation bytes */

    /* Second byte carries the range check that rejects overlong forms,
     * surrogates and anything above U+10FFFF. */
    if (i + 1 >= len || u[i+1] < lo2 || u[i+1] > hi2) return -1;
    for (int k = 2; k < seq_len; k++) {
        if (i + (size_t)k >= len || (u[i+k] & 0xC0) != 0x80) return -k;
    }

    uint32_t acc;
    switch (seq_len) {
        case 2:  acc = b0 & 0x1Fu; break;
        case 3:  acc = b0 & 0x0Fu; break;
        default: acc = b0 & 0x07u; break;
    }
    for (int k = 1; k < seq_len; k++) acc = (acc << 6) | (u[i+k] & 0x3Fu);
    *out = acc;
    return seq_len;
}

/* ------------------------------------------------------------------ */
/* Decode + lower-case                                                 */
/* ------------------------------------------------------------------ */

ustr ustr_decode_lower(const char *s) {
    ustr result;
    result.cp = NULL;
    result.len = 0;

    size_t byte_len = strlen(s);
    const unsigned char *u = (const unsigned char *)s;

    /* Decode first, case-map second. They have to be separate passes: the
     * final-sigma rule needs to see the neighbouring codepoints, and those
     * neighbours are only known once the whole string is decoded. */
    uint32_t *raw = SCORING_MALLOC((byte_len + 1) * sizeof(uint32_t));
    if (!raw) return result;

    size_t n = 0;
    size_t i = 0;
    while (i < byte_len) {
        uint32_t cp = 0; /* decode_one only writes this when it returns > 0 */
        int got = decode_one(u, byte_len, i, &cp);
        if (got > 0) {
            raw[n++] = cp;
            i += (size_t)got;
        } else {
            raw[n++] = SUBSTITUTE_CHAR;
            i += (size_t)(-got);
        }
    }

    /* U+0130 is the only mapping that yields two codepoints, so 2n+1 is a
     * sufficient bound for the lower-cased result. */
    uint32_t *out = SCORING_MALLOC((2 * n + 1) * sizeof(uint32_t));
    if (!out) { free(raw); return result; }

    size_t out_n = 0;
    for (size_t k = 0; k < n; k++) {
        uint32_t cp = raw[k];

        if (cp == FRAS_DOTTED_I) {
            out[out_n++] = FRAS_DOTTED_I_LOWER_0;
            out[out_n++] = FRAS_DOTTED_I_LOWER_1;
            continue;
        }

        if (cp == FRAS_CAPITAL_SIGMA) {
            /* Final_Sigma: preceded by a cased character (skipping
             * case-ignorable ones) and not followed by one. This is the
             * only context-sensitive rule mb_strtolower applies. */
            int before = 0;
            for (size_t b = k; b-- > 0; ) {
                if (is_case_ignorable(raw[b])) continue;
                before = is_cased(raw[b]);
                break;
            }
            int after = 0;
            for (size_t a = k + 1; a < n; a++) {
                if (is_case_ignorable(raw[a])) continue;
                after = is_cased(raw[a]);
                break;
            }
            out[out_n++] = (before && !after) ? FRAS_FINAL_SIGMA : FRAS_SMALL_SIGMA;
            continue;
        }

        out[out_n++] = simple_lower(cp);
    }

    free(raw);

    result.cp = out;
    result.len = out_n;
    return result;
}

void ustr_free(ustr *u) {
    free(u->cp);
    u->cp = NULL;
    u->len = 0;
}

int ustr_ok(const ustr *u) {
    return u->cp != NULL;
}

/* ------------------------------------------------------------------ */
/* Similarity components                                               */
/* ------------------------------------------------------------------ */

/* Mirrors PHP's preg_split('/[\s\-_,.\/]+/u', ...): \s under the /u
 * modifier is Unicode-aware, so a non-breaking or ideographic space
 * separates tokens exactly as an ASCII space does. */
static int is_delim(uint32_t cp) {
    switch (cp) {
        case '-': case '_': case ',': case '.': case '/':
            return 1;
        default:
            return is_unicode_space(cp);
    }
}

/* Lossless short-circuit: if query and field share no codepoints at all,
 * the LCS is guaranteed to be 0 and the DP can be skipped entirely (this
 * mirrors the always-on check in FRAS::look()). Both strings here are
 * typically short (search queries / single field values), so a simple
 * O(q*f) nested scan is plenty fast and avoids building a hash set. */
static int shares_any_char(const ustr *a, const ustr *b) {
    for (size_t i = 0; i < a->len; i++) {
        for (size_t j = 0; j < b->len; j++) {
            if (a->cp[i] == b->cp[j]) return 1;
        }
    }
    return 0;
}

/* Returns -1 if the DP buffers could not be allocated. */
static int lcs_length(const ustr *a, const ustr *b) {
    if (a->len == 0 || b->len == 0) return 0;
    if (!shares_any_char(a, b)) return 0;

    size_t m = a->len, n = b->len;
    /* Rolling two-row DP: O(min(m,n)) memory. Keep the shorter string as
     * the "row" dimension. */
    const ustr *shortS = (m <= n) ? a : b;
    const ustr *longS = (m <= n) ? b : a;
    size_t rowLen = shortS->len;

    int *prev = SCORING_CALLOC(rowLen + 1, sizeof(int));
    int *cur = SCORING_CALLOC(rowLen + 1, sizeof(int));
    if (!prev || !cur) { free(prev); free(cur); return -1; }

    for (size_t i = 1; i <= longS->len; i++) {
        for (size_t j = 1; j <= rowLen; j++) {
            if (longS->cp[i - 1] == shortS->cp[j - 1]) {
                cur[j] = prev[j - 1] + 1;
            } else {
                cur[j] = prev[j] > cur[j - 1] ? prev[j] : cur[j - 1];
            }
        }
        int *tmp = prev;
        prev = cur;
        cur = tmp;
    }

    int result = prev[rowLen];
    free(prev);
    free(cur);
    return result;
}

static int min3(int a, int b, int c) {
    int m = a < b ? a : b;
    return m < c ? m : c;
}

/* Returns -1 if the DP row could not be allocated. */
static int levenshtein_distance(const ustr *a, const ustr *b) {
    size_t m = a->len, n = b->len;
    if (m == 0) return (int)n;
    if (n == 0) return (int)m;

    /* Classic Wagner-Fischer with a rolling row, O(min(m,n)) memory. */
    const ustr *shortS = (m <= n) ? a : b;
    const ustr *longS = (m <= n) ? b : a;
    size_t rowLen = shortS->len;

    int *row = SCORING_MALLOC((rowLen + 1) * sizeof(int));
    if (!row) return -1;
    for (size_t j = 0; j <= rowLen; j++) row[j] = (int)j;

    for (size_t i = 1; i <= longS->len; i++) {
        int prevDiag = row[0];
        row[0] = (int)i;
        for (size_t j = 1; j <= rowLen; j++) {
            int deleteCost = row[j] + 1;
            int insertCost = row[j - 1] + 1;
            int substCost = prevDiag + (longS->cp[i - 1] != shortS->cp[j - 1]);
            int newVal = min3(deleteCost, insertCost, substCost);
            prevDiag = row[j];
            row[j] = newVal;
        }
    }

    int result = row[rowLen];
    free(row);
    return result;
}

/* Literal prefix score: how much of the query matches the field starting
 * at position 0, as a fraction of the query length. */
static double literal_prefix_score(const ustr *query, const ustr *field) {
    if (query->len == 0) return 0.0;
    size_t limit = query->len < field->len ? query->len : field->len;
    size_t matched = 0;
    for (; matched < limit; matched++) {
        if (query->cp[matched] != field->cp[matched]) break;
    }
    return (double)matched / (double)query->len;
}

/* Token-boundary bonus: best match from the start of any word, decayed by
 * how far into the field that word appears. Token 0 counts at full weight:
 * when the field starts with it, literal_prefix_score is already at least
 * as high, but after a leading delimiter this is the only check that sees
 * the first word. */
static double token_bonus_score(const ustr *query, const ustr *field) {
    if (query->len == 0) return 0.0;

    double best = 0.0;
    size_t tokenIndex = 0;
    size_t i = 0;
    int inToken = 0;
    size_t tokenStart = 0;

    /* Walk the field once, identifying [tokenStart, i) runs of non-delimiter
     * codepoints, exactly like PHP's preg_split on [\s\-_,.\/]+ with
     * PREG_SPLIT_NO_EMPTY (so leading delimiters produce no empty token). */
    for (i = 0; i <= field->len; i++) {
        int atDelim = (i == field->len) || is_delim(field->cp[i]);
        if (!atDelim && !inToken) {
            inToken = 1;
            tokenStart = i;
        } else if (atDelim && inToken) {
            inToken = 0;
            size_t tokenLen = i - tokenStart;
            size_t limit = query->len < tokenLen ? query->len : tokenLen;
            size_t matched = 0;
            for (; matched < limit; matched++) {
                if (query->cp[matched] != field->cp[tokenStart + matched]) break;
            }
            if (matched > 0) {
                double decay = 1.0 / (1.0 + (double)tokenIndex);
                double score = ((double)matched / (double)query->len) * decay;
                if (score > best) best = score;
            }
            tokenIndex++;
        }
    }

    return best;
}

int fras_score_pair(const ustr *query, const ustr *field, fras_scores *out) {
    size_t qLen = query->len, fLen = field->len;
    size_t minLen = qLen < fLen ? qLen : fLen;
    size_t maxLen = qLen > fLen ? qLen : fLen;

    int lcsLen = lcs_length(query, field);
    if (lcsLen < 0) return 0;
    out->lcs = minLen > 0 ? (double)lcsLen / (double)minLen : 0.0;

    int levDist = levenshtein_distance(query, field);
    if (levDist < 0) return 0;
    out->levenshtein = maxLen > 0 ? 1.0 - ((double)levDist / (double)maxLen) : 1.0;

    double literal = literal_prefix_score(query, field);
    double bonus = token_bonus_score(query, field);
    out->prefix = literal > bonus ? literal : bonus;

    return 1;
}
