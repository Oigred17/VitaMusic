/*
 * A small JSON reader with the same call surface as the handful of jansson
 * entry points this app uses.
 *
 * The jansson archive shipped by vdpm is built position-independent, so it
 * carries R_ARM_GOT_BREL relocations that vita-elf-create refuses
 * ("Invalid relocation type 25!"). Parsing the Innertube and lrclib replies
 * only needs objects, arrays, strings, integers and booleans, so the reader
 * lives here instead of pulling in a dependency that cannot be linked.
 *
 * Values are allocated from a per-document arena. `json_decref` releases the
 * whole document, so it must only be called on the value returned by
 * `json_loads`; passing a nested value is a no-op, not a free.
 */
#ifndef VM_JSON_H
#define VM_JSON_H

#include <stddef.h>

typedef struct vm_json json_t;

typedef struct {
    int line;
    int column;
    int position;
    char source[80];
    char text[160];
} json_error_t;

/* `flags` is accepted for source compatibility and ignored. */
json_t *json_loads(const char *input, size_t flags, json_error_t *error);

/* Releases the document that `value` came from. NULL is accepted. */
void json_decref(json_t *value);

int json_is_object(const json_t *value);
int json_is_array(const json_t *value);
int json_is_string(const json_t *value);
int json_is_integer(const json_t *value);
int json_is_real(const json_t *value);
int json_is_number(const json_t *value);
int json_is_boolean(const json_t *value);
int json_is_true(const json_t *value);
int json_is_false(const json_t *value);
int json_is_null(const json_t *value);

/* Property lookup; NULL when `value` is not an object or the key is absent. */
json_t *json_object_get(const json_t *value, const char *key);

size_t json_array_size(const json_t *value);
/* Out-of-range indexes and non-arrays yield NULL. */
json_t *json_array_get(const json_t *value, size_t index);

/* Object iteration, for walking an unknown schema (e.g. swarm data). */
size_t json_object_size(const json_t *value);
json_t *json_object_value_at(const json_t *value, size_t index);

/* NUL-terminated UTF-8; NULL when `value` is not a string. */
const char *json_string_value(const json_t *value);
long long json_integer_value(const json_t *value);
double json_real_value(const json_t *value);
int json_boolean_value(const json_t *value);

#endif /* VM_JSON_H */
