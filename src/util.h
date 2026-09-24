/*
 * VitaMusic - small shared helpers (logging, strings, filesystem).
 */
#ifndef VM_UTIL_H
#define VM_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/* --- logging -------------------------------------------------------------- */
/* Writes to VM_LOG_FILE and to the debug screen once it is available. */
void vm_log_init(void);
void vm_log_close(void);
void vm_log(const char *fmt, ...);

/* --- strings -------------------------------------------------------------- */
char *vm_strdup(const char *s);
void vm_strlcpy(char *dst, const char *src, size_t n);
/* Trims leading and trailing whitespace in place; returns s. */
char *vm_trim(char *s);
/* Case-insensitive substring search; returns a pointer into haystack or NULL. */
const char *vm_strcasestr(const char *haystack, const char *needle);

/* Growable string buffer used for URL and JSON assembly. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} vm_str_t;

void vm_str_init(vm_str_t *s);
void vm_str_free(vm_str_t *s);
void vm_str_clear(vm_str_t *s);
void vm_str_append(vm_str_t *s, const char *text);
/* Appends exactly `n` bytes; the payload may contain NUL characters, which is
 * what HTTP response bodies arriving from curl look like. */
void vm_str_append_len(vm_str_t *s, const char *text, size_t n);
void vm_str_appendf(vm_str_t *s, const char *fmt, ...);
/* Appends `text` with the characters JSON requires escaped ("\ and control). */
void vm_str_append_json(vm_str_t *s, const char *text);

/* --- filesystem ----------------------------------------------------------- */
/* Creates every missing component of `path` (ux0: style prefixes allowed). */
int vm_mkdir_p(const char *path);
bool vm_file_exists(const char *path);
long vm_file_size(const char *path);
bool vm_copy_file(const char *src, const char *dst);

#endif /* VM_UTIL_H */
