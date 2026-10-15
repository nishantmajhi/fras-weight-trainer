/*
 * probe - prints this project's three similarity components for
 * (query, field) pairs read from stdin as alternating lines.
 *
 * It exists so the C implementation can be diffed against the PHP one
 * mechanically: tools/gen_vectors.php drives this binary and the real
 * FRAS::look() over the same pairs and records where they agree.
 *
 * Deliberately line-based and dumb. Pairs containing a newline cannot be
 * expressed; the generator skips those, and tests/scoring_test.c covers
 * anything that cannot travel through this protocol.
 */
#include <stdio.h>
#include <string.h>

#include "scoring.h"

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

int main(void) {
    char q[4096], f[4096];

    /* Unbuffered: if this crashes part way through a comparison run, the
     * rows it did produce are the interesting evidence. */
    setvbuf(stdout, NULL, _IONBF, 0);

    while (fgets(q, sizeof q, stdin)) {
        if (!fgets(f, sizeof f, stdin)) break;
        trim_nl(q);
        trim_nl(f);

        ustr uq = ustr_decode_lower(q);
        ustr uf = ustr_decode_lower(f);
        if (!ustr_ok(&uq) || !ustr_ok(&uf)) {
            fprintf(stderr, "probe: out of memory\n");
            ustr_free(&uq);
            ustr_free(&uf);
            return 1;
        }

        fras_scores s;
        if (!fras_score_pair(&uq, &uf, &s)) {
            fprintf(stderr, "probe: out of memory scoring\n");
            ustr_free(&uq);
            ustr_free(&uf);
            return 1;
        }
        printf("prefix=%.6f lcs=%.6f lev=%.6f\n", s.prefix, s.lcs, s.levenshtein);

        ustr_free(&uq);
        ustr_free(&uf);
    }
    return 0;
}
