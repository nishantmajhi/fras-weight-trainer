/*
 * json.h - a small, dependency-free JSON reader used by fras-weight-trainer.
 *
 * This is intentionally NOT a general-purpose JSON library. It implements
 * just enough of the spec (objects, arrays, strings with escapes, numbers,
 * true/false/null) to load the training-data format documented in
 * README.md. No external dependencies means the whole tool builds with a
 * single `cc` invocation.
 */
#ifndef FRAS_JSON_H
#define FRAS_JSON_H

#include <stddef.h>

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type;

typedef struct json_value json_value;

struct json_value {
    json_type type;
    union {
        int boolean;
        double number;
        char *string;
        struct {
            json_value **items;
            size_t count;
            size_t capacity;
        } array;
        struct {
            char **keys;
            json_value **values;
            size_t count;
            size_t capacity;
        } object;
    } as;
};

/* Parses `text` (NUL-terminated). Returns a heap-owned tree on success,
 * which must be freed with json_free().
 *
 * On failure returns NULL and, if `error_out` is non-NULL, stores a
 * heap-allocated message there that the CALLER MUST FREE. *error_out is
 * always written: it is set to NULL first, and stays NULL if even the
 * error message could not be allocated, so callers must tolerate that. */
json_value *json_parse(const char *text, char **error_out);

void json_free(json_value *v);

/* Convenience accessors. Return NULL / 0 / default on type mismatch or
 * missing key rather than crashing -- training files are user-authored and
 * we want clear errors, not segfaults. */
json_value *json_object_get(const json_value *obj, const char *key);
int json_is_array(const json_value *v);
int json_is_object(const json_value *v);
int json_is_string(const json_value *v);
int json_is_number(const json_value *v);
int json_is_bool(const json_value *v);
double json_as_number(const json_value *v, double fallback);
const char *json_as_string(const json_value *v, const char *fallback);
int json_as_bool(const json_value *v, int fallback);

#endif
