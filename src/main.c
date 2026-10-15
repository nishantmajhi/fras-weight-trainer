/*
 * fras-weight-trainer
 *
 * Reads labeled search sessions (query + candidate field values + which
 * one the user actually picked) and fits [prefix, lcs, levenshtein]
 * weights that make FRAS::look() rank the chosen candidate as highly as
 * possible, on average. Writes a JSON file in the exact shape FRAS's
 * $weightsFile constructor argument expects (see FRAS README.md, section
 * "Learned weights").
 *
 * This file is the plumbing: argument parsing, reading the training file,
 * turning it into scored sessions, and writing the result. The two things
 * that actually decide anything live next door and are tested directly --
 * src/scoring.c computes the three similarity components, and src/fit.c
 * searches the weight simplex.
 *
 * Design note kept here because it explains the shape of the whole tool:
 * the three components for a given (query, field) pair don't depend on the
 * weights at all, only the final weighted sum does. So each pair's
 * components are computed exactly once -- that's the expensive DP work --
 * and the grid search over candidate weight triples is then just a cheap
 * weighted sum and compare per candidate. That is what makes an exhaustive
 * simplex search fast enough to beat reaching for a gradient method on a
 * non-differentiable ranking objective.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "json.h"
#include "scoring.h"
#include "fit.h"

/* sysconf(_SC_NPROCESSORS_ONLN) is POSIX and does not exist on MinGW, so
 * the default thread count needs a platform split. Everything else in the
 * tool is portable C11 plus pthreads, which MinGW supplies via winpthreads. */
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static int detect_cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}
#else
#  include <unistd.h>
static int detect_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#endif

/* ---------------------------------------------------------------------
 * Data model
 * ------------------------------------------------------------------- */

/* candidate_score and training_session live in fit.h, next to the search
 * that consumes them. */

typedef struct {
    training_session *items;
    size_t count;
    size_t capacity;
} session_list;

/* Returns 0 if the list could not grow; the caller still owns `s`. */
static int session_list_push(session_list *list, training_session s) {
    if (list->count == list->capacity) {
        size_t new_cap = list->capacity ? list->capacity * 2 : 16;
        training_session *grown = realloc(list->items, new_cap * sizeof(training_session));
        if (!grown) return 0;
        list->items = grown;
        list->capacity = new_cap;
    }
    list->items[list->count++] = s;
    return 1;
}

static void free_sessions(session_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].profile);
        free(list->items[i].candidates);
    }
    free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

typedef struct {
    char *name;
    size_t *indices; /* indices into the global session_list */
    size_t count;
    size_t capacity;
} profile_group;

/* Returns NULL if the group table could not grow or the name could not be
 * copied. */
static profile_group *find_or_create_group(profile_group **groups, size_t *count, size_t *cap, const char *name) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*groups)[i].name, name) == 0) return &(*groups)[i];
    }
    if (*count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 8;
        profile_group *grown = realloc(*groups, new_cap * sizeof(profile_group));
        if (!grown) return NULL;
        *groups = grown;
        *cap = new_cap;
    }
    char *name_copy = strdup(name);
    if (!name_copy) return NULL;

    profile_group *g = &(*groups)[(*count)++];
    g->name = name_copy;
    g->indices = NULL;
    g->count = 0;
    g->capacity = 0;
    return g;
}

/* Returns 0 if the index list could not grow. */
static int group_push(profile_group *g, size_t idx) {
    if (g->count == g->capacity) {
        size_t new_cap = g->capacity ? g->capacity * 2 : 8;
        size_t *grown = realloc(g->indices, new_cap * sizeof(size_t));
        if (!grown) return 0;
        g->indices = grown;
        g->capacity = new_cap;
    }
    g->indices[g->count++] = idx;
    return 1;
}

/* ---------------------------------------------------------------------
 * Training-data loading
 * ------------------------------------------------------------------- */

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); errno = ENOMEM; return NULL; }
    size_t got = fread(buf, 1, (size_t)size, f);
    if (ferror(f)) { free(buf); fclose(f); return NULL; }
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Parses one candidate object: {"value": "...", "chosen": true|false}.
 * "field" is accepted as an alias for "value".
 *
 * Returns 1 on success, 0 if the candidate is malformed (caller warns and
 * skips it), or -1 if an allocation failed. The last is kept distinct
 * because silently dropping candidates when memory runs short would fit
 * weights against a quietly truncated training set. */
static int load_candidate(const json_value *cobj, const ustr *query, candidate_score *out) {
    if (!json_is_object(cobj)) return 0;
    const json_value *val = json_object_get(cobj, "value");
    if (!val) val = json_object_get(cobj, "field");
    if (!json_is_string(val)) return 0;

    ustr field = ustr_decode_lower(val->as.string);
    if (!ustr_ok(&field)) return -1;
    fras_scores fs;
    int scored = fras_score_pair(query, &field, &fs);
    ustr_free(&field);
    if (!scored) return -1;

    out->prefix = fs.prefix;
    out->lcs = fs.lcs;
    out->levenshtein = fs.levenshtein;
    out->chosen = json_as_bool(json_object_get(cobj, "chosen"), 0);
    return 1;
}

/* Parses one session object and appends it to `list` if valid. Returns 0
 * if it loaded fine, 1 if it was skipped (the caller counts that as a
 * warning), or -1 if an allocation failed and the run cannot continue. */
static int load_session(const json_value *sobj, session_list *list, size_t line_hint, int verbose) {
    if (!json_is_object(sobj)) {
        fprintf(stderr, "warning: session #%zu is not a JSON object, skipping\n", line_hint);
        return 1;
    }
    const json_value *query_v = json_object_get(sobj, "query");
    if (!json_is_string(query_v)) {
        fprintf(stderr, "warning: session #%zu has no string \"query\", skipping\n", line_hint);
        return 1;
    }
    const json_value *cands_v = json_object_get(sobj, "candidates");
    if (!json_is_array(cands_v) || cands_v->as.array.count < 2) {
        fprintf(stderr, "warning: session #%zu needs at least 2 \"candidates\", skipping\n", line_hint);
        return 1;
    }

    ustr query = ustr_decode_lower(query_v->as.string);
    if (!ustr_ok(&query)) return -1;

    candidate_score *scores = malloc(cands_v->as.array.count * sizeof(candidate_score));
    if (!scores) { ustr_free(&query); return -1; }

    size_t count = 0;
    int any_chosen = 0;
    for (size_t i = 0; i < cands_v->as.array.count; i++) {
        candidate_score cs;
        int rc = load_candidate(cands_v->as.array.items[i], &query, &cs);
        if (rc < 0) { ustr_free(&query); free(scores); return -1; }
        if (rc == 0) {
            fprintf(stderr, "warning: session #%zu candidate #%zu is malformed, skipping candidate\n", line_hint, i);
            continue;
        }
        if (cs.chosen) any_chosen = 1;
        scores[count++] = cs;
    }
    ustr_free(&query);

    if (count < 2 || !any_chosen) {
        fprintf(stderr, "warning: session #%zu has fewer than 2 usable candidates or none marked \"chosen\": true, skipping\n", line_hint);
        free(scores);
        return 1;
    }

    training_session s;
    const json_value *profile_v = json_object_get(sobj, "profile");
    s.profile = NULL;
    if (json_is_string(profile_v)) {
        /* A failed copy here must not fall back to NULL: that would quietly
         * move the session out of its profile group and into "global" only,
         * changing what gets fitted. */
        s.profile = strdup(profile_v->as.string);
        if (!s.profile) { free(scores); return -1; }
    }
    s.candidates = scores;
    s.candidate_count = count;
    if (!session_list_push(list, s)) {
        free(s.profile);
        free(scores);
        return -1;
    }

    if (verbose) {
        fprintf(stderr, "loaded session #%zu: query=\"%s\" candidates=%zu profile=%s\n",
                line_hint, query_v->as.string, count, s.profile ? s.profile : "(none)");
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * Output
 * ------------------------------------------------------------------- */

static void write_json_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", f); break;
            case '\\': fputs("\\\\", f); break;
            case '\n': fputs("\\n", f); break;
            case '\r': fputs("\\r", f); break;
            case '\t': fputs("\\t", f); break;
            default:
                if (*p < 0x20) fprintf(f, "\\u%04x", *p);
                else fputc(*p, f);
        }
    }
    fputc('"', f);
}

/* Below this many sessions, an exhaustive search over ~5000 grid points
 * can fit the training set perfectly no matter what it contains, so the
 * MRR printed alongside the weights says nothing about how they will
 * generalise. There is no held-out split here, so the honest thing is to
 * say so rather than let a confident-looking number stand unqualified. */
#define TRUSTWORTHY_SESSION_COUNT 30

static void warn_if_overfitted(const char *label, size_t n, const fit_result *r) {
    if (n >= TRUSTWORTHY_SESSION_COUNT) return;
    fprintf(stderr,
            "warning: \"%s\" was fitted from only %zu session(s). The grid search has\n"
            "         far more weight combinations than that many sessions can\n"
            "         distinguish, so the MRR of %.4f is measured on the same data it\n"
            "         was fitted to and is an upper bound, not an estimate. Collect at\n"
            "         least %d labelled sessions before trusting these weights.\n",
            label, n, r->mrr, TRUSTWORTHY_SESSION_COUNT);
}

static void write_weight_object(FILE *f, const fit_result *r, const char *indent) {
    fprintf(f, "{\n");
    fprintf(f, "%s  \"prefix\": %.4f,\n", indent, r->prefix);
    fprintf(f, "%s  \"lcs\": %.4f,\n", indent, r->lcs);
    fprintf(f, "%s  \"levenshtein\": %.4f\n", indent, r->levenshtein);
    fprintf(f, "%s}", indent);
}

/* ---------------------------------------------------------------------
 * CLI
 * ------------------------------------------------------------------- */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <training-data.json>\n"
        "\n"
        "Fits FRAS [prefix, lcs, levenshtein] weights from labeled search\n"
        "sessions and writes a JSON file compatible with FRAS's $weightsFile\n"
        "constructor argument.\n"
        "\n"
        "Options:\n"
        "  -o, --output PATH        Output path (default: learned-weights.json)\n"
        "      --step FLOAT         Grid search resolution, e.g. 0.01 = 101x101\n"
        "                           steps per axis (default: 0.01)\n"
        "  -t, --threads N          Worker threads (default: number of CPUs)\n"
        "      --min-examples N     Minimum sessions required to fit a per-profile\n"
        "                           entry; profiles with fewer still contribute to\n"
        "                           \"global\" (default: 3)\n"
        "  -v, --verbose            Print per-session loading detail\n"
        "  -h, --help                Show this help\n"
        "\n"
        "Training data format: see README.md.\n",
        prog);
}

int main(int argc, char **argv) {
    const char *input_path = NULL;
    const char *output_path = "learned-weights.json";
    double step = 0.01;
    int threads = detect_cpu_count();
    int min_examples = 3;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 || strcmp(a, "--output") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: %s requires an argument\n", a); return 1; }
            output_path = argv[i];
        } else if (strcmp(a, "--step") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: --step requires an argument\n"); return 1; }
            step = atof(argv[i]);
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "--threads") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: %s requires an argument\n", a); return 1; }
            threads = atoi(argv[i]);
        } else if (strcmp(a, "--min-examples") == 0) {
            if (++i >= argc) { fprintf(stderr, "error: --min-examples requires an argument\n"); return 1; }
            min_examples = atoi(argv[i]);
        } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (a[0] == '-' && strcmp(a, "-") != 0) {
            fprintf(stderr, "error: unknown option '%s'\n", a);
            print_usage(argv[0]);
            return 1;
        } else {
            input_path = a;
        }
    }

    if (!input_path) {
        print_usage(argv[0]);
        return 1;
    }
    if (step <= 0.0 || step > 1.0) {
        fprintf(stderr, "error: --step must be in (0, 1]\n");
        return 1;
    }

    char *text = read_file(input_path);
    if (!text) {
        fprintf(stderr, "error: could not read '%s': %s\n", input_path, strerror(errno));
        return 1;
    }

    char *parse_err = NULL;
    json_value *root = json_parse(text, &parse_err);
    free(text);
    if (!root) {
        fprintf(stderr, "error: failed to parse '%s': %s\n", input_path,
                parse_err ? parse_err : "out of memory");
        free(parse_err);
        return 1;
    }

    const json_value *sessions_v = json_is_array(root) ? root : json_object_get(root, "sessions");
    if (!json_is_array(sessions_v)) {
        fprintf(stderr, "error: expected a top-level JSON array, or an object with a \"sessions\" array\n");
        json_free(root);
        return 1;
    }

    session_list list = {0};
    size_t skipped = 0;
    for (size_t i = 0; i < sessions_v->as.array.count; i++) {
        int rc = load_session(sessions_v->as.array.items[i], &list, i, verbose);
        if (rc < 0) {
            fprintf(stderr, "error: out of memory loading session #%zu\n", i);
            json_free(root);
            free_sessions(&list);
            return 1;
        }
        skipped += (rc == 1) ? 1 : 0;
    }
    json_free(root);

    fprintf(stderr, "loaded %zu usable session(s), skipped %zu\n", list.count, skipped);
    if (list.count == 0) {
        fprintf(stderr, "error: no usable training sessions, nothing to fit\n");
        free_sessions(&list);
        return 1;
    }

    /* Global group: every valid session. */
    size_t *global_idx = malloc(list.count * sizeof(size_t));
    if (!global_idx) {
        fprintf(stderr, "error: out of memory\n");
        free_sessions(&list);
        return 1;
    }
    for (size_t i = 0; i < list.count; i++) global_idx[i] = i;

    /* Profile groups: every session tagged with a profile also contributes
     * to that profile's own group, in addition to global. */
    profile_group *groups = NULL;
    size_t group_count = 0, group_cap = 0;
    typedef struct { char *name; fit_result fit; } profile_fit;
    profile_fit *profile_fits = NULL;
    size_t profile_fit_count = 0;
    FILE *out = NULL;
    int status = 1; /* every path below cleans up through `fail:` */

    for (size_t i = 0; i < list.count; i++) {
        if (!list.items[i].profile) continue;
        profile_group *g = find_or_create_group(&groups, &group_count, &group_cap, list.items[i].profile);
        if (!g || !group_push(g, i)) {
            fprintf(stderr, "error: out of memory grouping sessions by profile\n");
            goto fail;
        }
    }

    printf("Fitting global weights from %zu session(s)...\n", list.count);
    fit_result global_fit = fras_fit_weights(list.items, global_idx, list.count, step, threads);
    if (!global_fit.ok) {
        fprintf(stderr, "error: out of memory fitting global weights\n");
        goto fail;
    }
    printf("  global: prefix=%.4f lcs=%.4f levenshtein=%.4f  (MRR=%.4f, top-1 accuracy=%.4f)\n",
           global_fit.prefix, global_fit.lcs, global_fit.levenshtein, global_fit.mrr, global_fit.accuracy);
    warn_if_overfitted("global", list.count, &global_fit);

    if (group_count) {
        profile_fits = malloc(group_count * sizeof(profile_fit));
        if (!profile_fits) {
            fprintf(stderr, "error: out of memory\n");
            goto fail;
        }
    }

    for (size_t g = 0; g < group_count; g++) {
        profile_group *pg = &groups[g];
        if ((int)pg->count < min_examples) {
            fprintf(stderr, "note: profile \"%s\" has only %zu session(s) (< --min-examples %d), "
                            "not writing a dedicated entry; these sessions still count toward \"global\"\n",
                    pg->name, pg->count, min_examples);
            continue;
        }
        printf("Fitting profile \"%s\" weights from %zu session(s)...\n", pg->name, pg->count);
        fit_result pf = fras_fit_weights(list.items, pg->indices, pg->count, step, threads);
        if (!pf.ok) {
            fprintf(stderr, "error: out of memory fitting profile \"%s\"\n", pg->name);
            goto fail;
        }
        printf("  %s: prefix=%.4f lcs=%.4f levenshtein=%.4f  (MRR=%.4f, top-1 accuracy=%.4f)\n",
               pg->name, pf.prefix, pf.lcs, pf.levenshtein, pf.mrr, pf.accuracy);
        warn_if_overfitted(pg->name, pg->count, &pf);
        profile_fits[profile_fit_count].name = pg->name;
        profile_fits[profile_fit_count].fit = pf;
        profile_fit_count++;
    }

    out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "error: could not write '%s': %s\n", output_path, strerror(errno));
        goto fail;
    }
    fprintf(out, "{\n");
    fprintf(out, "  \"profiles\": {");
    if (profile_fit_count == 0) {
        fprintf(out, "},\n");
    } else {
        fprintf(out, "\n");
        for (size_t i = 0; i < profile_fit_count; i++) {
            fprintf(out, "    ");
            write_json_string(out, profile_fits[i].name);
            fprintf(out, ": ");
            write_weight_object(out, &profile_fits[i].fit, "    ");
            fprintf(out, i + 1 < profile_fit_count ? ",\n" : "\n");
        }
        fprintf(out, "  },\n");
    }
    fprintf(out, "  \"global\": ");
    write_weight_object(out, &global_fit, "  ");
    fprintf(out, "\n}\n");

    /* The weights file is the entire output of this tool, so a write that
     * failed half way -- a full disk, a full quota -- must not be reported
     * as success. stdio buffers, so most errors only surface at the flush
     * that fclose() performs. */
    if (ferror(out) || fclose(out) != 0) {
        fprintf(stderr, "error: failed writing '%s': %s\n", output_path, strerror(errno));
        out = NULL;
        goto fail;
    }
    out = NULL;

    printf("Wrote %s\n", output_path);
    status = 0;

fail:
    if (out) fclose(out);
    for (size_t g = 0; g < group_count; g++) {
        free(groups[g].name);
        free(groups[g].indices);
    }
    free(groups);
    free(profile_fits);
    free(global_idx);
    free_sessions(&list);

    return status;
}
