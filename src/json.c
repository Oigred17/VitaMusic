/*
 * JSON reader backing json.h. See that header for why this exists.
 *
 * The document is built in an arena: one or two contiguous blocks per parse
 * instead of a malloc per node, which keeps a 2 MB Innertube reply cheap and
 * makes teardown a single free.
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    E_JSON_NULL = 0,
    E_JSON_TRUE,
    E_JSON_FALSE,
    E_JSON_INTEGER,
    E_JSON_REAL,
    E_JSON_STRING,
    E_JSON_ARRAY,
    E_JSON_OBJECT
};

/* Innertube replies nest a few dozen levels at most; the cap only exists so a
 * hostile response cannot run the worker thread out of stack. */
#define E_JSON_MAX_DEPTH 128
#define E_JSON_BLOCK (16 * 1024)

struct ej_block {
    struct ej_block *next;
    size_t used;
    size_t cap;
    size_t pad;
};

_Static_assert(sizeof(struct ej_block) % 8 == 0,
               "arena payload must stay 8-byte aligned");

struct ej_arena {
    struct ej_block *head;
};

struct vm_json {
    unsigned char type;
    unsigned char root;
    struct ej_arena *arena;
    union {
        long long integer;
        double real;
        struct {
            const char *ptr;
            size_t len;
        } string;
        struct {
            struct vm_json **items;
            size_t count;
        } array;
        struct {
            const char **keys;
            struct vm_json **values;
            size_t count;
        } object;
    } u;
};

/* Scratch lists that collect members while the container is still open. Their
 * order is reversed when the final array is filled, so lookups see the
 * document's own order. */
struct ej_item {
    struct ej_item *next;
    struct vm_json *value;
};

struct ej_member {
    struct ej_member *next;
    const char *key;
    struct vm_json *value;
};

struct parser {
    const char *begin;
    const char *p;
    const char *end;
    struct ej_arena *arena;
    json_error_t *err;
    int depth;
};

/* Mutual recursion: containers are parsed before the value dispatcher is
 * defined, and the dispatcher needs the container parsers. */
static struct vm_json *parse_value(struct parser *ps);
static struct vm_json *parse_object(struct parser *ps);
static struct vm_json *parse_array(struct parser *ps);

/* -------------------------------------------------------------------------- */
/* arena                                                                      */
/* -------------------------------------------------------------------------- */

static void *arena_alloc(struct ej_arena *a, size_t size)
{
    struct ej_block *b;
    void *p;

    size = (size + 7u) & ~(size_t)7u;

    b = a->head;
    if (!b || b->cap - b->used < size) {
        size_t cap = size > E_JSON_BLOCK ? size : (size_t)E_JSON_BLOCK;
        struct ej_block *nb = malloc(sizeof(*nb) + cap);

        if (!nb)
            return NULL;
        nb->next = a->head;
        nb->used = 0;
        nb->cap = cap;
        nb->pad = 0;
        a->head = nb;
        b = nb;
    }

    p = (char *)(b + 1) + b->used;
    b->used += size;
    return p;
}

static void arena_free(struct ej_arena *a)
{
    struct ej_block *b = a->head;

    while (b) {
        struct ej_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

/* -------------------------------------------------------------------------- */
/* parser                                                                     */
/* -------------------------------------------------------------------------- */

static void fail(struct parser *ps, const char *msg)
{
    int line = 1;
    int column = 1;
    const char *q;

    if (!ps->err)
        return;

    for (q = ps->begin; q < ps->p; q++) {
        if (*q == '\n') {
            line++;
            column = 1;
        } else {
            column++;
        }
    }

    ps->err->position = (int)(ps->p - ps->begin);
    ps->err->line = line;
    ps->err->column = column;
    snprintf(ps->err->text, sizeof(ps->err->text), "%s at offset %d", msg,
             ps->err->position);
}

static void skip_ws(struct parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            ps->p++;
        else
            break;
    }
}

static int hex4(const char *p, unsigned int *out)
{
    unsigned int value = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        int digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return -1;

        value = (value << 4) | (unsigned int)digit;
    }

    *out = value;
    return 0;
}

static size_t utf8_encode(unsigned int code, char *out)
{
    if (code < 0x80u) {
        out[0] = (char)code;
        return 1;
    }
    if (code < 0x800u) {
        out[0] = (char)(0xC0u | (code >> 6));
        out[1] = (char)(0x80u | (code & 0x3Fu));
        return 2;
    }
    if (code < 0x10000u) {
        out[0] = (char)(0xE0u | (code >> 12));
        out[1] = (char)(0x80u | ((code >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (code & 0x3Fu));
        return 3;
    }

    out[0] = (char)(0xF0u | (code >> 18));
    out[1] = (char)(0x80u | ((code >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((code >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (code & 0x3Fu));
    return 4;
}

/* `ps->p` must sit on the opening quote. Decoded text is always shorter than
 * its source, so one arena buffer of the raw length plus the terminator is
 * enough even on the escape-heavy path. */
static int parse_string(struct parser *ps, const char **out_ptr,
                        size_t *out_len)
{
    const char *start;
    const char *close;
    size_t raw;

    ps->p++; /* the opening quote */
    start = ps->p;

    for (close = start; close < ps->end; close++) {
        unsigned char c = (unsigned char)*close;

        if (c == '"')
            break;
        if (c == '\\') {
            close++;
            if (close >= ps->end)
                break;
            continue;
        }
        if (c < 0x20u) {
            ps->p = close;
            fail(ps, "control character in string");
            return -1;
        }
    }

    if (close >= ps->end || *close != '"') {
        ps->p = close;
        fail(ps, "unterminated string");
        return -1;
    }

    raw = (size_t)(close - start);

    {
        char *dst = arena_alloc(ps->arena, raw + 1);
        char *w;

        if (!dst) {
            fail(ps, "out of memory");
            return -1;
        }

        w = dst;
        {
            const char *r = start;

            while (r < close) {
                unsigned char c = (unsigned char)*r++;

                if (c != '\\') {
                    *w++ = (char)c;
                    continue;
                }

                if (r >= close)
                    break;

                switch (*r++) {
                case '"':
                    *w++ = '"';
                    break;
                case '\\':
                    *w++ = '\\';
                    break;
                case '/':
                    *w++ = '/';
                    break;
                case 'b':
                    *w++ = '\b';
                    break;
                case 'f':
                    *w++ = '\f';
                    break;
                case 'n':
                    *w++ = '\n';
                    break;
                case 'r':
                    *w++ = '\r';
                    break;
                case 't':
                    *w++ = '\t';
                    break;
                case 'u': {
                    unsigned int code;

                    if (r + 4 > close || hex4(r, &code) != 0) {
                        ps->p = r;
                        fail(ps, "bad \\u escape");
                        return -1;
                    }
                    r += 4;

                    if (code >= 0xD800u && code <= 0xDBFFu) {
                        unsigned int low;

                        if (r + 6 <= close && r[0] == '\\' && r[1] == 'u' &&
                            hex4(r + 2, &low) == 0 && low >= 0xDC00u &&
                            low <= 0xDFFFu) {
                            code = 0x10000u + ((code - 0xD800u) << 10) +
                                   (low - 0xDC00u);
                            r += 6;
                        } else {
                            code = 0xFFFDu; /* unpaired surrogate */
                        }
                    } else if (code >= 0xDC00u && code <= 0xDFFFu) {
                        code = 0xFFFDu;
                    }

                    w += utf8_encode(code, w);
                    break;
                }
                default:
                    ps->p = r - 1;
                    fail(ps, "bad escape sequence");
                    return -1;
                }
            }
        }

        *w = '\0';
        *out_ptr = dst;
        *out_len = (size_t)(w - dst);
    }

    ps->p = close + 1;
    return 0;
}

static struct vm_json *mk_node(struct parser *ps, unsigned char type)
{
    struct vm_json *node = arena_alloc(ps->arena, sizeof(*node));

    if (!node) {
        fail(ps, "out of memory");
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    node->type = type;
    node->arena = ps->arena;
    return node;
}

static struct vm_json *mk_boolean(struct parser *ps, int value)
{
    return mk_node(ps, value ? E_JSON_TRUE : E_JSON_FALSE);
}

static int match_literal(struct parser *ps, const char *word)
{
    size_t len = strlen(word);

    if ((size_t)(ps->end - ps->p) < len || memcmp(ps->p, word, len) != 0) {
        fail(ps, "invalid literal");
        return -1;
    }

    ps->p += len;
    return 0;
}

static struct vm_json *parse_object(struct parser *ps)
{
    struct ej_member *head = NULL;
    struct vm_json *node;
    size_t count = 0;

    ps->p++; /* the opening brace */
    skip_ws(ps);

    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return mk_node(ps, E_JSON_OBJECT);
    }

    for (;;) {
        const char *key;
        size_t key_len;
        struct ej_member *member;
        struct vm_json *value;

        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            fail(ps, "expected object key");
            return NULL;
        }
        if (parse_string(ps, &key, &key_len) != 0)
            return NULL;

        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            fail(ps, "expected ':'");
            return NULL;
        }
        ps->p++;

        value = parse_value(ps);
        if (!value)
            return NULL;

        member = arena_alloc(ps->arena, sizeof(*member));
        if (!member) {
            fail(ps, "out of memory");
            return NULL;
        }
        member->key = key;
        member->value = value;
        member->next = head;
        head = member;
        count++;

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            break;
        }

        fail(ps, "expected ',' or '}'");
        return NULL;
    }

    node = mk_node(ps, E_JSON_OBJECT);
    if (!node)
        return NULL;

    node->u.object.keys = arena_alloc(ps->arena, count * sizeof(*node->u.object.keys));
    node->u.object.values =
        arena_alloc(ps->arena, count * sizeof(*node->u.object.values));
    if (!node->u.object.keys || !node->u.object.values) {
        fail(ps, "out of memory");
        return NULL;
    }
    node->u.object.count = count;

    {
        struct ej_member *m = head;
        size_t slot = count;

        while (m) {
            slot--;
            node->u.object.keys[slot] = m->key;
            node->u.object.values[slot] = m->value;
            m = m->next;
        }
    }

    return node;
}

static struct vm_json *parse_array(struct parser *ps)
{
    struct ej_item *head = NULL;
    struct vm_json *node;
    size_t count = 0;

    ps->p++; /* the opening bracket */
    skip_ws(ps);

    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return mk_node(ps, E_JSON_ARRAY);
    }

    for (;;) {
        struct ej_item *item;
        struct vm_json *value = parse_value(ps);

        if (!value)
            return NULL;

        item = arena_alloc(ps->arena, sizeof(*item));
        if (!item) {
            fail(ps, "out of memory");
            return NULL;
        }
        item->value = value;
        item->next = head;
        head = item;
        count++;

        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            break;
        }

        fail(ps, "expected ',' or ']'");
        return NULL;
    }

    node = mk_node(ps, E_JSON_ARRAY);
    if (!node)
        return NULL;

    node->u.array.items = arena_alloc(ps->arena, count * sizeof(*node->u.array.items));
    if (!node->u.array.items) {
        fail(ps, "out of memory");
        return NULL;
    }
    node->u.array.count = count;

    {
        struct ej_item *item = head;
        size_t slot = count;

        while (item) {
            slot--;
            node->u.array.items[slot] = item->value;
            item = item->next;
        }
    }

    return node;
}

static struct vm_json *parse_number(struct parser *ps)
{
    const char *start = ps->p;
    int is_real = 0;
    size_t int_digits = 0;
    struct vm_json *node;

    if (ps->p < ps->end && *ps->p == '-')
        ps->p++;

    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
        ps->p++;
        int_digits++;
    }

    if (int_digits == 0) {
        fail(ps, "invalid number");
        return NULL;
    }

    if (ps->p < ps->end && *ps->p == '.') {
        size_t frac_digits = 0;

        ps->p++;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
            ps->p++;
            frac_digits++;
        }
        if (frac_digits == 0) {
            fail(ps, "invalid number");
            return NULL;
        }
        is_real = 1;
    }

    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
            ps->p++;

        {
            size_t exp_digits = 0;

            while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
                ps->p++;
                exp_digits++;
            }
            if (exp_digits == 0) {
                fail(ps, "invalid number");
                return NULL;
            }
        }
        is_real = 1;
    }

    /* Long literals are handed to strtod, which saturates rather than
     * wrapping the way a hand-rolled accumulator would. */
    if (int_digits > 18)
        is_real = 1;

    node = mk_node(ps, is_real ? E_JSON_REAL : E_JSON_INTEGER);
    if (!node)
        return NULL;

    {
        char *copy = arena_alloc(ps->arena, (size_t)(ps->p - start) + 1);

        if (!copy) {
            fail(ps, "out of memory");
            return NULL;
        }
        memcpy(copy, start, (size_t)(ps->p - start));
        copy[ps->p - start] = '\0';

        if (is_real)
            node->u.real = strtod(copy, NULL);
        else
            node->u.integer = strtoll(copy, NULL, 10);
    }

    return node;
}

static struct vm_json *parse_value_inner(struct parser *ps)
{
    char c;

    skip_ws(ps);
    if (ps->p >= ps->end) {
        fail(ps, "unexpected end of input");
        return NULL;
    }

    c = *ps->p;

    switch (c) {
    case '{':
        return parse_object(ps);
    case '[':
        return parse_array(ps);
    case '"': {
        const char *ptr;
        size_t len;
        struct vm_json *node;

        if (parse_string(ps, &ptr, &len) != 0)
            return NULL;

        node = mk_node(ps, E_JSON_STRING);
        if (node) {
            node->u.string.ptr = ptr;
            node->u.string.len = len;
        }
        return node;
    }
    case 't':
        return match_literal(ps, "true") == 0 ? mk_boolean(ps, 1) : NULL;
    case 'f':
        return match_literal(ps, "false") == 0 ? mk_boolean(ps, 0) : NULL;
    case 'n':
        return match_literal(ps, "null") == 0 ? mk_node(ps, E_JSON_NULL) : NULL;
    default:
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number(ps);
        fail(ps, "unexpected character");
        return NULL;
    }
}

static struct vm_json *parse_value(struct parser *ps)
{
    struct vm_json *value;

    if (ps->depth >= E_JSON_MAX_DEPTH) {
        fail(ps, "nesting too deep");
        return NULL;
    }

    ps->depth++;
    value = parse_value_inner(ps);
    ps->depth--;
    return value;
}

/* -------------------------------------------------------------------------- */
/* public surface                                                             */
/* -------------------------------------------------------------------------- */

json_t *json_loads(const char *input, size_t flags, json_error_t *error)
{
    struct parser ps;
    struct ej_arena *arena;
    struct vm_json *root;

    (void)flags;

    if (error) {
        memset(error, 0, sizeof(*error));
        error->line = 1;
        error->column = 1;
    }

    if (!input)
        return NULL;

    arena = malloc(sizeof(*arena));
    if (!arena)
        return NULL;
    arena->head = NULL;

    /* A byte order mark sometimes survives a proxy; it is not part of the
     * document and would otherwise be reported as an unexpected character. */
    if ((unsigned char)input[0] == 0xEFu && (unsigned char)input[1] == 0xBBu &&
        (unsigned char)input[2] == 0xBFu)
        input += 3;

    ps.begin = input;
    ps.p = input;
    ps.end = input + strlen(input);
    ps.arena = arena;
    ps.err = error;
    ps.depth = 0;

    root = parse_value(&ps);
    if (root) {
        skip_ws(&ps);
        if (ps.p != ps.end) {
            fail(&ps, "trailing characters after the document");
            root = NULL;
        }
    }

    if (!root) {
        arena_free(arena);
        return NULL;
    }

    root->root = 1;
    return root;
}

void json_decref(json_t *value)
{
    /* Only a document root owns the arena; nested values are borrowed. */
    if (!value || !value->root)
        return;

    arena_free(value->arena);
}

int json_is_object(const json_t *value)
{
    return value && value->type == E_JSON_OBJECT;
}

int json_is_array(const json_t *value)
{
    return value && value->type == E_JSON_ARRAY;
}

int json_is_string(const json_t *value)
{
    return value && value->type == E_JSON_STRING;
}

int json_is_integer(const json_t *value)
{
    return value && value->type == E_JSON_INTEGER;
}

int json_is_real(const json_t *value)
{
    return value && value->type == E_JSON_REAL;
}

int json_is_number(const json_t *value)
{
    return value && (value->type == E_JSON_INTEGER || value->type == E_JSON_REAL);
}

int json_is_boolean(const json_t *value)
{
    return value && (value->type == E_JSON_TRUE || value->type == E_JSON_FALSE);
}

int json_is_true(const json_t *value)
{
    return value && value->type == E_JSON_TRUE;
}

int json_is_false(const json_t *value)
{
    return value && value->type == E_JSON_FALSE;
}

int json_is_null(const json_t *value)
{
    return value && value->type == E_JSON_NULL;
}

json_t *json_object_get(const json_t *value, const char *key)
{
    size_t len;
    size_t i;

    if (!value || value->type != E_JSON_OBJECT || !key)
        return NULL;

    len = strlen(key);
    for (i = 0; i < value->u.object.count; i++) {
        const char *candidate = value->u.object.keys[i];

        /* The stored key length is implicit in the terminator; comparing the
         * first byte and the length cheaply rejects most misses. */
        if (candidate[0] != key[0] || strlen(candidate) != len)
            continue;
        if (memcmp(candidate, key, len) == 0)
            return value->u.object.values[i];
    }

    return NULL;
}

size_t json_array_size(const json_t *value)
{
    if (!value || value->type != E_JSON_ARRAY)
        return 0;
    return value->u.array.count;
}

json_t *json_array_get(const json_t *value, size_t index)
{
    if (!value || value->type != E_JSON_ARRAY || index >= value->u.array.count)
        return NULL;
    return value->u.array.items[index];
}

size_t json_object_size(const json_t *value)
{
    if (!value || value->type != E_JSON_OBJECT)
        return 0;
    return value->u.object.count;
}

json_t *json_object_value_at(const json_t *value, size_t index)
{
    if (!value || value->type != E_JSON_OBJECT || index >= value->u.object.count)
        return NULL;
    return value->u.object.values[index];
}

const char *json_string_value(const json_t *value)
{
    if (!value || value->type != E_JSON_STRING)
        return NULL;
    return value->u.string.ptr;
}

long long json_integer_value(const json_t *value)
{
    if (!value || value->type != E_JSON_INTEGER)
        return 0;
    return value->u.integer;
}

double json_real_value(const json_t *value)
{
    if (!value)
        return 0.0;
    if (value->type == E_JSON_REAL)
        return value->u.real;
    if (value->type == E_JSON_INTEGER)
        return (double)value->u.integer;
    return 0.0;
}

int json_boolean_value(const json_t *value)
{
    return value && value->type == E_JSON_TRUE;
}
