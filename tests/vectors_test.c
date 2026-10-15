/*
 * Replays tests/vectors.json through this project's scoring.
 *
 * The expected numbers in that file were produced by the real
 * FRAS::look(), not by this code, so this is the check that the trainer
 * still scores the way production does. It is the whole reason the
 * vectors file exists -- see tests/README.md.
 *
 * Entries under "divergent", if any, are asserted against their recorded "c"
 * values. If a change here makes one of them agree with PHP, this test
 * still passes but tools/check_php.php will fail and say the entry should
 * move into "agreed".
 */
#include "harness.h"
#include "scoring.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static void check_group(const json_value *arr, const char *key, const char *label) {
    if (!json_is_array(arr)) {
        CHECK(0, "vectors.json has no \"%s\" array", label);
        return;
    }
    for (size_t i = 0; i < arr->as.array.count; i++) {
        const json_value *e = arr->as.array.items[i];
        const char *q = json_as_string(json_object_get(e, "query"), NULL);
        const char *f = json_as_string(json_object_get(e, "field"), NULL);
        const json_value *want = json_object_get(e, key);
        if (!q || !f || !json_is_object(want)) {
            CHECK(0, "%s vector %zu is malformed", label, i);
            continue;
        }

        ustr uq = ustr_decode_lower(q);
        ustr uf = ustr_decode_lower(f);
        fras_scores got;
        int ok = ustr_ok(&uq) && ustr_ok(&uf) && fras_score_pair(&uq, &uf, &got);
        ustr_free(&uq);
        ustr_free(&uf);
        if (!ok) {
            CHECK(0, "%s vector %zu could not be scored", label, i);
            continue;
        }

        /* The file stores six decimal places, so compare at that scale
         * rather than at the harness's usual tolerance. */
        double wp = json_as_number(json_object_get(want, "prefix"), -1);
        double wl = json_as_number(json_object_get(want, "lcs"), -1);
        double wv = json_as_number(json_object_get(want, "levenshtein"), -1);

        tests_run++;
        if (!(fabs(got.prefix - wp) < 5e-7 &&
              fabs(got.lcs - wl) < 5e-7 &&
              fabs(got.levenshtein - wv) < 5e-7)) {
            tests_failed++;
            fprintf(stderr,
                    "FAIL %s:%d: %s vector %zu q=\"%s\" f=\"%s\"\n"
                    "       got  prefix=%.6f lcs=%.6f lev=%.6f\n"
                    "       want prefix=%.6f lcs=%.6f lev=%.6f\n",
                    __FILE__, __LINE__, label, i, q, f,
                    got.prefix, got.lcs, got.levenshtein, wp, wl, wv);
        }
    }
}

void run_vector_tests(const char *path) {
    char *text = slurp(path);
    if (!text) {
        CHECK(0, "could not read %s", path);
        return;
    }

    char *err = NULL;
    json_value *root = json_parse(text, &err);
    free(text);
    if (!root) {
        CHECK(0, "could not parse %s: %s", path, err ? err : "no message");
        free(err);
        return;
    }
    free(err);

    const json_value *agreed = json_object_get(root, "agreed");
    const json_value *divergent = json_object_get(root, "divergent");

    check_group(agreed, "expected", "agreed");
    check_group(divergent, "c", "divergent");

    /* Guard against the file being silently emptied or truncated: the
     * suite would otherwise pass by asserting nothing. */
    CHECK(json_is_array(agreed) && agreed->as.array.count >= 30,
          "vectors.json still holds a meaningful number of agreed vectors");
    /* The one known divergence (FRAS::look() was case-sensitive in two of
     * its three components) was fixed on 2026-09-30. Both sides now agree on
     * every pair, so an entry here is a new disagreement nobody has explained,
     * and weights fitted while it stands are fitted to the wrong function. */
    CHECK(json_is_array(divergent) && divergent->as.array.count == 0,
          "vectors.json records no pair on which this trainer and FRAS disagree");

    json_free(root);
}
