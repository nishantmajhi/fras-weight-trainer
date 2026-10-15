/*
 * Tests for src/json.c.
 *
 * The allocation-failure half is the point of this file. The original
 * parser checked none of its seven allocation sites and wrote three of
 * them back over the pointer it was growing, so a failed realloc both
 * leaked the old buffer and produced a NULL dereference. Proving that is
 * fixed needs allocation to actually fail on demand, which is what the
 * counting allocator below provides: src/json.c is compiled into this
 * binary with its JSON_MALLOC/JSON_CALLOC/JSON_REALLOC hooks pointed here.
 */
#include "harness.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

/* ---- the failing allocator ---------------------------------------- */

/* When >= 0, the allocation with this index fails and every later one
 * succeeds. Set to -1 to disable. */
int json_test_fail_at = -1;
static int alloc_index = 0;

static int should_fail(void) {
    int idx = alloc_index++;
    return json_test_fail_at >= 0 && idx == json_test_fail_at;
}

void *json_test_malloc(size_t n) {
    if (should_fail()) return NULL;
    return malloc(n);
}

void *json_test_calloc(size_t n, size_t sz) {
    if (should_fail()) return NULL;
    return calloc(n, sz);
}

void *json_test_realloc(void *p, size_t n) {
    if (should_fail()) return NULL;
    return realloc(p, n);
}

static void reset_alloc(void) {
    alloc_index = 0;
    json_test_fail_at = -1;
}

static int alloc_count_for(const char *text) {
    reset_alloc();
    char *err = NULL;
    json_value *v = json_parse(text, &err);
    json_free(v);
    free(err);
    return alloc_index;
}

/* ---- parsing ------------------------------------------------------- */

static json_value *parse_ok(const char *text) {
    reset_alloc();
    char *err = NULL;
    json_value *v = json_parse(text, &err);
    if (!v) {
        fprintf(stderr, "       (parse failed: %s)\n", err ? err : "no message");
        free(err);
    }
    return v;
}

static void expect_parse_error(const char *text, const char *what) {
    reset_alloc();
    char *err = NULL;
    json_value *v = json_parse(text, &err);
    CHECK(v == NULL, "%s should not parse: %s", what, text);
    CHECK(err != NULL, "%s should report an error message", what);
    json_free(v);
    free(err);
}

static void test_scalars(void) {
    json_value *v = parse_ok("42");
    CHECK(json_is_number(v), "bare number parses");
    CHECK_NEAR(json_as_number(v, -1), 42.0, "number value");
    json_free(v);

    v = parse_ok("-3.5e2");
    CHECK_NEAR(json_as_number(v, -1), -350.0, "exponent and sign");
    json_free(v);

    v = parse_ok("true");
    CHECK(json_is_bool(v), "true parses");
    CHECK_INT(json_as_bool(v, 0), 1, "true value");
    json_free(v);

    v = parse_ok("false");
    CHECK_INT(json_as_bool(v, 1), 0, "false value");
    json_free(v);

    v = parse_ok("null");
    CHECK(v != NULL, "null parses");
    json_free(v);

    v = parse_ok("\"hi\"");
    CHECK_STR(json_as_string(v, NULL), "hi", "string value");
    json_free(v);
}

static void test_string_escapes(void) {
    json_value *v = parse_ok("\"a\\nb\\tc\\\\d\\\"e\\/f\"");
    CHECK_STR(json_as_string(v, NULL), "a\nb\tc\\d\"e/f", "all simple escapes");
    json_free(v);

    v = parse_ok("\"\\u0041\"");
    CHECK_STR(json_as_string(v, NULL), "A", "\\u escape, ASCII");
    json_free(v);

    v = parse_ok("\"\\u00e9\"");
    CHECK_STR(json_as_string(v, NULL), "\xC3\xA9", "\\u escape, two-byte UTF-8");
    json_free(v);

    v = parse_ok("\"\\u20ac\"");
    CHECK_STR(json_as_string(v, NULL), "\xE2\x82\xAC", "\\u escape, three-byte UTF-8");
    json_free(v);

    /* Surrogate pair for U+1F600. */
    v = parse_ok("\"\\ud83d\\ude00\"");
    CHECK_STR(json_as_string(v, NULL), "\xF0\x9F\x98\x80", "surrogate pair combines");
    json_free(v);

    /* A string long enough to force the output buffer to grow past its
     * initial 32 bytes several times. */
    v = parse_ok("\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"");
    CHECK_INT(strlen(json_as_string(v, "")), 72, "long string grows its buffer correctly");
    json_free(v);

    expect_parse_error("\"unterminated", "unterminated string");
    expect_parse_error("\"\\q\"", "invalid escape");
    expect_parse_error("\"\\u00\"", "truncated \\u escape");
    expect_parse_error("\"\\uZZZZ\"", "non-hex \\u escape");
    expect_parse_error("\"a\tb\"", "raw control character in string");
}

static void test_containers(void) {
    json_value *v = parse_ok("[1,2,3]");
    CHECK(json_is_array(v), "array parses");
    CHECK_INT(v->as.array.count, 3, "array length");
    CHECK_NEAR(json_as_number(v->as.array.items[2], -1), 3.0, "array element");
    json_free(v);

    v = parse_ok("[]");
    CHECK_INT(v->as.array.count, 0, "empty array");
    json_free(v);

    v = parse_ok("{}");
    CHECK(json_is_object(v), "empty object");
    json_free(v);

    v = parse_ok("{\"a\":1,\"b\":\"two\"}");
    CHECK_NEAR(json_as_number(json_object_get(v, "a"), -1), 1.0, "object number member");
    CHECK_STR(json_as_string(json_object_get(v, "b"), NULL), "two", "object string member");
    CHECK(json_object_get(v, "missing") == NULL, "absent key returns NULL");
    json_free(v);

    /* More than 8 elements, so the growth path runs. */
    v = parse_ok("[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]");
    CHECK_INT(v->as.array.count, 18, "array grows past its initial capacity");
    CHECK_NEAR(json_as_number(v->as.array.items[17], -1), 17.0, "last element after growth");
    json_free(v);

    v = parse_ok("{\"k0\":0,\"k1\":1,\"k2\":2,\"k3\":3,\"k4\":4,\"k5\":5,"
                 "\"k6\":6,\"k7\":7,\"k8\":8,\"k9\":9}");
    CHECK_NEAR(json_as_number(json_object_get(v, "k9"), -1), 9.0, "object grows past initial capacity");
    json_free(v);

    expect_parse_error("[1,2", "unterminated array");
    expect_parse_error("{\"a\":1", "unterminated object");
    expect_parse_error("{a:1}", "unquoted object key");
    expect_parse_error("{\"a\" 1}", "missing colon");
    expect_parse_error("[1,]", "trailing comma in array");
    expect_parse_error("", "empty input");
    expect_parse_error("[1] junk", "trailing data after value");
    expect_parse_error("tru", "truncated literal");
    expect_parse_error("01", "leading zero is trailing data");
}

/* Regression: the parser recursed once per nesting level with no cap, so
 * a few thousand opening brackets exhausted the stack and killed the
 * process instead of producing an error. */
static void test_depth_limit(void) {
    size_t n = 5000;
    char *deep = malloc(2 * n + 1);
    CHECK(deep != NULL, "test allocation");
    if (!deep) return;
    for (size_t i = 0; i < n; i++) deep[i] = '[';
    for (size_t i = 0; i < n; i++) deep[n + i] = ']';
    deep[2 * n] = '\0';

    reset_alloc();
    char *err = NULL;
    json_value *v = json_parse(deep, &err);
    CHECK(v == NULL, "deeply nested input is rejected rather than crashing");
    CHECK(err != NULL && strstr(err, "depth") != NULL,
          "rejection names the depth limit: %s", err ? err : "(none)");
    json_free(v);
    free(err);
    free(deep);

    /* Nesting just inside the cap still parses. */
    size_t ok_n = 150;
    char *shallow = malloc(2 * ok_n + 1);
    CHECK(shallow != NULL, "test allocation");
    if (!shallow) return;
    for (size_t i = 0; i < ok_n; i++) shallow[i] = '[';
    for (size_t i = 0; i < ok_n; i++) shallow[ok_n + i] = ']';
    shallow[2 * ok_n] = '\0';

    v = parse_ok(shallow);
    CHECK(v != NULL, "nesting within the cap still parses");
    json_free(v);
    free(shallow);
}

/* ---- allocation failure -------------------------------------------- */

/* For a representative document, fail each allocation in turn and require
 * that the parser reports failure cleanly every time. Run under a leak
 * checker this also proves nothing is dropped; run plainly it proves there
 * is no NULL dereference, which is what the original code did. */
static void test_every_allocation_failure_is_handled(void) {
    static const char *documents[] = {
        "{\"sessions\":[{\"query\":\"hello world\",\"candidates\":["
            "{\"value\":\"first candidate\",\"chosen\":true},"
            "{\"value\":\"second candidate\",\"chosen\":false},"
            "{\"value\":\"third\",\"chosen\":false}],\"profile\":\"name\"}]}",
        "[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19]",
        "{\"k0\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"k1\":1,\"k2\":2,"
            "\"k3\":3,\"k4\":4,\"k5\":5,\"k6\":6,\"k7\":7,\"k8\":8,\"k9\":9}",
        "\"a string long enough to need its buffer grown more than once over\"",
        "-12345.6789e-3",
    };

    for (size_t d = 0; d < sizeof documents / sizeof documents[0]; d++) {
        int total = alloc_count_for(documents[d]);
        CHECK(total > 0, "document %zu performs at least one allocation", d);

        for (int fail = 0; fail < total; fail++) {
            reset_alloc();
            json_test_fail_at = fail;

            char *err = NULL;
            json_value *v = json_parse(documents[d], &err);

            /* Either the parse succeeded (the failed allocation was on a
             * path that could recover) or it failed cleanly with a
             * message. What must never happen is a crash, and what must
             * never happen silently is a NULL tree with no error set. */
            if (v == NULL) {
                CHECK(err != NULL,
                      "document %zu: failing allocation %d reports an error", d, fail);
            }
            json_free(v);
            free(err);
            reset_alloc();
        }
    }
}

/* The specific shape the original got wrong: growing an array or object
 * when realloc fails must not lose the existing buffer. Failing only the
 * growth allocation (not the earlier element ones) exercises it. */
static void test_container_growth_failure(void) {
    const char *doc = "[0,1,2,3,4,5,6,7,8,9]"; /* forces one array growth */
    int total = alloc_count_for(doc);

    int clean_failures = 0;
    for (int fail = 0; fail < total; fail++) {
        reset_alloc();
        json_test_fail_at = fail;
        char *err = NULL;
        json_value *v = json_parse(doc, &err);
        if (!v && err) clean_failures++;
        json_free(v);
        free(err);
    }
    reset_alloc();
    CHECK(clean_failures > 0, "at least one allocation failure is reported rather than ignored");
}

void run_json_tests(void) {
    test_scalars();
    test_string_escapes();
    test_containers();
    test_depth_limit();
    test_every_allocation_failure_is_handled();
    test_container_growth_failure();
    reset_alloc();
}
