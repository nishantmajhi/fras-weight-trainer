#define _POSIX_C_SOURCE 200809L
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Allocation is routed through these so the test suite can force failures
 * and check that every site handles them. They are plain malloc/calloc/
 * realloc unless the translation unit is compiled with overrides. */
#ifndef JSON_MALLOC
#define JSON_MALLOC malloc
#endif
#ifndef JSON_CALLOC
#define JSON_CALLOC calloc
#endif
#ifndef JSON_REALLOC
#define JSON_REALLOC realloc
#endif

/* Depth cap for nested arrays/objects. The parser recurses once per level,
 * so without a cap a file of a few thousand '[' characters exhausts the
 * stack and crashes the process. The training data this reads is only ever
 * two levels deep (sessions -> candidates), so this is generous. */
#define JSON_MAX_DEPTH 200

typedef struct {
    const char *s;
    size_t pos;
    size_t len;
    char errbuf[256];
    int failed;
    int depth;
} parser_state;

static json_value *parse_value(parser_state *p);

static void fail(parser_state *p, const char *msg) {
    if (p->failed) return; /* keep first error */
    p->failed = 1;
    /* Compute a rough line/column for a useful error message. */
    size_t line = 1, col = 1;
    for (size_t i = 0; i < p->pos && i < p->len; i++) {
        if (p->s[i] == '\n') { line++; col = 1; } else { col++; }
    }
    snprintf(p->errbuf, sizeof(p->errbuf), "%s (line %zu, column %zu)", msg, line, col);
}

static int peek(parser_state *p) {
    return p->pos < p->len ? (unsigned char)p->s[p->pos] : -1;
}

static void skip_ws(parser_state *p) {
    while (p->pos < p->len) {
        char c = p->s[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static json_value *alloc_value(json_type t) {
    json_value *v = JSON_CALLOC(1, sizeof(json_value));
    if (!v) return NULL;
    v->type = t;
    return v;
}

/* Returns 0 if the array could not grow. The caller owns `item` either
 * way, so on failure it must free it -- the array is left untouched. */
static int array_push(json_value *arr, json_value *item) {
    if (arr->as.array.count == arr->as.array.capacity) {
        size_t new_cap = arr->as.array.capacity ? arr->as.array.capacity * 2 : 8;
        /* Assign to a temporary first: realloc returns NULL on failure, and
         * writing that straight back over the field would throw away the
         * only pointer to the existing buffer. */
        json_value **grown = JSON_REALLOC(arr->as.array.items, new_cap * sizeof(json_value *));
        if (!grown) return 0;
        arr->as.array.items = grown;
        arr->as.array.capacity = new_cap;
    }
    arr->as.array.items[arr->as.array.count++] = item;
    return 1;
}

/* Returns 0 if the object could not grow; `key` and `val` stay the
 * caller's to free. The two arrays grow independently, so a failure on the
 * second leaves the first larger than needed -- harmless, since capacity
 * is only ever an upper bound and json_free works off `count`. */
static int object_push(json_value *obj, char *key, json_value *val) {
    if (obj->as.object.count == obj->as.object.capacity) {
        size_t new_cap = obj->as.object.capacity ? obj->as.object.capacity * 2 : 8;
        char **grown_keys = JSON_REALLOC(obj->as.object.keys, new_cap * sizeof(char *));
        if (!grown_keys) return 0;
        obj->as.object.keys = grown_keys;

        json_value **grown_vals = JSON_REALLOC(obj->as.object.values, new_cap * sizeof(json_value *));
        if (!grown_vals) return 0;
        obj->as.object.values = grown_vals;

        obj->as.object.capacity = new_cap;
    }
    obj->as.object.keys[obj->as.object.count] = key;
    obj->as.object.values[obj->as.object.count] = val;
    obj->as.object.count++;
    return 1;
}

/* Encodes a single Unicode codepoint as UTF-8 into buf, returns bytes written. */
static int utf8_encode(unsigned int cp, char *buf) {
    if (cp <= 0x7F) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parses a JSON string literal starting at the opening quote. Returns a
 * freshly allocated, NUL-terminated, unescaped UTF-8 C string, or NULL on
 * failure (with p->errbuf set). */
static char *parse_string_raw(parser_state *p) {
    if (peek(p) != '"') { fail(p, "expected string"); return NULL; }
    p->pos++; /* consume opening quote */

    size_t cap = 32, len = 0;
    char *out = JSON_MALLOC(cap);
    if (!out) { fail(p, "out of memory"); return NULL; }

    for (;;) {
        if (p->pos >= p->len) { fail(p, "unterminated string"); free(out); return NULL; }
        char c = p->s[p->pos];
        if (c == '"') { p->pos++; break; }
        if ((unsigned char)c < 0x20) { fail(p, "control character in string"); free(out); return NULL; }

        char piece[5];
        int piece_len;

        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) { fail(p, "unterminated escape"); free(out); return NULL; }
            char e = p->s[p->pos];
            switch (e) {
                case '"': piece[0] = '"'; piece_len = 1; p->pos++; break;
                case '\\': piece[0] = '\\'; piece_len = 1; p->pos++; break;
                case '/': piece[0] = '/'; piece_len = 1; p->pos++; break;
                case 'b': piece[0] = '\b'; piece_len = 1; p->pos++; break;
                case 'f': piece[0] = '\f'; piece_len = 1; p->pos++; break;
                case 'n': piece[0] = '\n'; piece_len = 1; p->pos++; break;
                case 'r': piece[0] = '\r'; piece_len = 1; p->pos++; break;
                case 't': piece[0] = '\t'; piece_len = 1; p->pos++; break;
                case 'u': {
                    p->pos++;
                    if (p->pos + 4 > p->len) { fail(p, "truncated \\u escape"); free(out); return NULL; }
                    int h0 = hex_val(p->s[p->pos]), h1 = hex_val(p->s[p->pos+1]),
                        h2 = hex_val(p->s[p->pos+2]), h3 = hex_val(p->s[p->pos+3]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) { fail(p, "invalid \\u escape"); free(out); return NULL; }
                    unsigned int cp = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
                    p->pos += 4;
                    /* Handle surrogate pairs. */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p->pos + 6 <= p->len && p->s[p->pos] == '\\' && p->s[p->pos+1] == 'u') {
                            int g0 = hex_val(p->s[p->pos+2]), g1 = hex_val(p->s[p->pos+3]),
                                g2 = hex_val(p->s[p->pos+4]), g3 = hex_val(p->s[p->pos+5]);
                            unsigned int low = (g0 << 12) | (g1 << 8) | (g2 << 4) | g3;
                            if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0 && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                p->pos += 6;
                            }
                        }
                    }
                    piece_len = utf8_encode(cp, piece);
                    break;
                }
                default:
                    fail(p, "invalid escape sequence");
                    free(out);
                    return NULL;
            }
        } else {
            piece[0] = c;
            piece_len = 1;
            p->pos++;
        }

        if (len + (size_t)piece_len + 1 > cap) {
            size_t new_cap = cap;
            while (len + (size_t)piece_len + 1 > new_cap) new_cap *= 2;
            char *grown = JSON_REALLOC(out, new_cap);
            if (!grown) { fail(p, "out of memory"); free(out); return NULL; }
            out = grown;
            cap = new_cap;
        }
        memcpy(out + len, piece, (size_t)piece_len);
        len += (size_t)piece_len;
    }

    out[len] = '\0';
    return out;
}

static json_value *parse_string(parser_state *p) {
    char *s = parse_string_raw(p);
    if (!s) return NULL;
    json_value *v = alloc_value(JSON_STRING);
    if (!v) { fail(p, "out of memory"); free(s); return NULL; }
    v->as.string = s;
    return v;
}

static json_value *parse_number(parser_state *p) {
    size_t start = p->pos;
    if (peek(p) == '-') p->pos++;
    if (p->pos >= p->len || !isdigit((unsigned char)p->s[p->pos])) { fail(p, "invalid number"); return NULL; }
    /* JSON forbids leading zeros, and so does PHP's json_decode. Without
     * this check "01" is read as the number 1 rather than rejected, so a
     * typo in a training file changes a value instead of reporting it. */
    if (p->s[p->pos] == '0' && p->pos + 1 < p->len && isdigit((unsigned char)p->s[p->pos + 1])) {
        fail(p, "number has a leading zero");
        return NULL;
    }
    while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    if (peek(p) == '.') {
        p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->s[p->pos])) { fail(p, "invalid number"); return NULL; }
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    if (peek(p) == 'e' || peek(p) == 'E') {
        p->pos++;
        if (peek(p) == '+' || peek(p) == '-') p->pos++;
        if (p->pos >= p->len || !isdigit((unsigned char)p->s[p->pos])) { fail(p, "invalid number"); return NULL; }
        while (p->pos < p->len && isdigit((unsigned char)p->s[p->pos])) p->pos++;
    }
    char *tmp = JSON_MALLOC(p->pos - start + 1);
    if (!tmp) { fail(p, "out of memory"); return NULL; }
    memcpy(tmp, p->s + start, p->pos - start);
    tmp[p->pos - start] = '\0';
    json_value *v = alloc_value(JSON_NUMBER);
    if (!v) { fail(p, "out of memory"); free(tmp); return NULL; }
    v->as.number = strtod(tmp, NULL);
    free(tmp);
    return v;
}

static json_value *parse_literal(parser_state *p, const char *lit, json_value *made) {
    if (!made) { fail(p, "out of memory"); return NULL; }
    size_t l = strlen(lit);
    if (p->pos + l > p->len || strncmp(p->s + p->pos, lit, l) != 0) {
        fail(p, "invalid literal");
        free(made);
        return NULL;
    }
    p->pos += l;
    return made;
}

static json_value *parse_array(parser_state *p) {
    p->pos++; /* consume '[' */
    json_value *arr = alloc_value(JSON_ARRAY);
    if (!arr) { fail(p, "out of memory"); return NULL; }
    skip_ws(p);
    if (peek(p) == ']') { p->pos++; return arr; }
    for (;;) {
        skip_ws(p);
        json_value *item = parse_value(p);
        if (!item) { json_free(arr); return NULL; }
        if (!array_push(arr, item)) {
            fail(p, "out of memory");
            json_free(item);
            json_free(arr);
            return NULL;
        }
        skip_ws(p);
        int c = peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == ']') { p->pos++; break; }
        fail(p, "expected ',' or ']' in array");
        json_free(arr);
        return NULL;
    }
    return arr;
}

static json_value *parse_object(parser_state *p) {
    p->pos++; /* consume '{' */
    json_value *obj = alloc_value(JSON_OBJECT);
    if (!obj) { fail(p, "out of memory"); return NULL; }
    skip_ws(p);
    if (peek(p) == '}') { p->pos++; return obj; }
    for (;;) {
        skip_ws(p);
        if (peek(p) != '"') { fail(p, "expected string key in object"); json_free(obj); return NULL; }
        char *key = parse_string_raw(p);
        if (!key) { json_free(obj); return NULL; }
        skip_ws(p);
        if (peek(p) != ':') { fail(p, "expected ':' after object key"); free(key); json_free(obj); return NULL; }
        p->pos++;
        skip_ws(p);
        json_value *val = parse_value(p);
        if (!val) { free(key); json_free(obj); return NULL; }
        if (!object_push(obj, key, val)) {
            fail(p, "out of memory");
            free(key);
            json_free(val);
            json_free(obj);
            return NULL;
        }
        skip_ws(p);
        int c = peek(p);
        if (c == ',') { p->pos++; continue; }
        if (c == '}') { p->pos++; break; }
        fail(p, "expected ',' or '}' in object");
        json_free(obj);
        return NULL;
    }
    return obj;
}

static json_value *parse_value(parser_state *p) {
    skip_ws(p);
    int c = peek(p);
    if (c < 0) { fail(p, "unexpected end of input"); return NULL; }

    if (c == '{' || c == '[') {
        if (p->depth >= JSON_MAX_DEPTH) {
            fail(p, "maximum nesting depth exceeded");
            return NULL;
        }
        p->depth++;
        json_value *v = (c == '{') ? parse_object(p) : parse_array(p);
        p->depth--;
        return v;
    }

    switch (c) {
        case '"': return parse_string(p);
        case 't': {
            json_value *v = alloc_value(JSON_BOOL);
            if (!v) { fail(p, "out of memory"); return NULL; }
            v->as.boolean = 1;
            return parse_literal(p, "true", v);
        }
        case 'f': {
            json_value *v = alloc_value(JSON_BOOL);
            if (!v) { fail(p, "out of memory"); return NULL; }
            v->as.boolean = 0;
            return parse_literal(p, "false", v);
        }
        case 'n':
            return parse_literal(p, "null", alloc_value(JSON_NULL));
        default:
            if (c == '-' || isdigit(c)) return parse_number(p);
            fail(p, "unexpected character");
            return NULL;
    }
}

json_value *json_parse(const char *text, char **error_out) {
    parser_state p;
    p.s = text;
    p.pos = 0;
    p.len = strlen(text);
    p.failed = 0;
    p.depth = 0;
    p.errbuf[0] = '\0';

    if (error_out) *error_out = NULL;

    /* Skip a UTF-8 byte order mark. Plenty of Windows tools write one when
     * asked for "UTF-8", and without this the file fails with "unexpected
     * character" pointing at column 1, where there is nothing visible. */
    if (p.len >= 3 && (unsigned char)p.s[0] == 0xEF
                   && (unsigned char)p.s[1] == 0xBB
                   && (unsigned char)p.s[2] == 0xBF) {
        p.pos = 3;
    }

    json_value *v = parse_value(&p);
    if (v) {
        skip_ws(&p);
        if (p.pos != p.len) {
            fail(&p, "trailing data after JSON value");
            json_free(v);
            v = NULL;
        }
    }
    if (!v && error_out) {
        *error_out = strdup(p.errbuf[0] ? p.errbuf : "unknown JSON parse error");
    }
    return v;
}

void json_free(json_value *v) {
    if (!v) return;
    switch (v->type) {
        case JSON_STRING:
            free(v->as.string);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < v->as.array.count; i++) json_free(v->as.array.items[i]);
            free(v->as.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < v->as.object.count; i++) {
                free(v->as.object.keys[i]);
                json_free(v->as.object.values[i]);
            }
            free(v->as.object.keys);
            free(v->as.object.values);
            break;
        default:
            break;
    }
    free(v);
}

json_value *json_object_get(const json_value *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->as.object.count; i++) {
        if (strcmp(obj->as.object.keys[i], key) == 0) return obj->as.object.values[i];
    }
    return NULL;
}

int json_is_array(const json_value *v) { return v && v->type == JSON_ARRAY; }
int json_is_object(const json_value *v) { return v && v->type == JSON_OBJECT; }
int json_is_string(const json_value *v) { return v && v->type == JSON_STRING; }
int json_is_number(const json_value *v) { return v && v->type == JSON_NUMBER; }
int json_is_bool(const json_value *v) { return v && v->type == JSON_BOOL; }

double json_as_number(const json_value *v, double fallback) {
    return json_is_number(v) ? v->as.number : fallback;
}
const char *json_as_string(const json_value *v, const char *fallback) {
    return json_is_string(v) ? v->as.string : fallback;
}
int json_as_bool(const json_value *v, int fallback) {
    return json_is_bool(v) ? v->as.boolean : fallback;
}
