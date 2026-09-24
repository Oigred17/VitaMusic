#include "util.h"
#include "config.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */
/* logging                                                                    */
/* -------------------------------------------------------------------------- */

static SceUID g_log_fd = -1;
static SceKernelLwMutexWork g_log_lock;
static int g_log_ready = 0;

void vm_log_init(void)
{
    if (sceKernelCreateLwMutex(&g_log_lock, "vm_log_lock", 0, 0, NULL) < 0)
        return;

    g_log_fd = sceIoOpen(VM_LOG_FILE,
                         SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,
                         0777);
    g_log_ready = 1;

    if (g_log_fd >= 0) {
        const char banner[] = "\n===== VitaMusic " VM_APP_VERSION " =====\n";
        sceIoWrite(g_log_fd, banner, sizeof(banner) - 1);
    }
}

void vm_log_close(void)
{
    if (!g_log_ready)
        return;
    if (g_log_fd >= 0)
        sceIoClose(g_log_fd);
    g_log_fd = -1;
    g_log_ready = 0;
}

void vm_log(const char *fmt, ...)
{
    char line[1024];
    va_list args;
    int n;

    if (!g_log_ready)
        return;

    va_start(args, fmt);
    n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n < 0)
        return;
    if ((size_t)n > sizeof(line) - 1)
        n = (int)sizeof(line) - 1;

    sceKernelLockLwMutex(&g_log_lock, 1, NULL);
    if (g_log_fd >= 0)
        sceIoWrite(g_log_fd, line, (unsigned)n);
    sceKernelUnlockLwMutex(&g_log_lock, 1);
}

/* -------------------------------------------------------------------------- */
/* strings                                                                    */
/* -------------------------------------------------------------------------- */

char *vm_strdup(const char *s)
{
    size_t len;
    char *copy;

    if (!s)
        return NULL;
    len = strlen(s) + 1;
    copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

void vm_strlcpy(char *dst, const char *src, size_t n)
{
    size_t i;

    if (!dst || n == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < n && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

char *vm_trim(char *s)
{
    char *end;

    if (!s)
        return s;
    while (*s && isspace((unsigned char)*s))
        s++;

    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
    return s;
}

const char *vm_strcasestr(const char *haystack, const char *needle)
{
    size_t needle_len;

    if (!haystack || !needle)
        return NULL;
    needle_len = strlen(needle);
    if (needle_len == 0)
        return haystack;

    for (; *haystack; haystack++) {
        size_t i;
        for (i = 0; i < needle_len; i++) {
            if (!haystack[i])
                return NULL;
            if (tolower((unsigned char)haystack[i]) !=
                tolower((unsigned char)needle[i]))
                break;
        }
        if (i == needle_len)
            return haystack;
    }
    return NULL;
}

/* --- growable string ------------------------------------------------------ */

void vm_str_init(vm_str_t *s)
{
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void vm_str_free(vm_str_t *s)
{
    free(s->data);
    s->data = NULL;
    s->len = 0;
    s->cap = 0;
}

void vm_str_clear(vm_str_t *s)
{
    s->len = 0;
    if (s->data)
        s->data[0] = '\0';
}

static int vm_str_reserve(vm_str_t *s, size_t extra)
{
    size_t need = s->len + extra + 1;
    size_t cap;
    char *grown;

    if (need <= s->cap)
        return 1;

    cap = s->cap ? s->cap : 256;
    while (cap < need)
        cap *= 2;

    grown = realloc(s->data, cap);
    if (!grown)
        return 0;
    s->data = grown;
    s->cap = cap;
    return 1;
}

void vm_str_append(vm_str_t *s, const char *text)
{
    size_t n;

    if (!text)
        return;
    n = strlen(text);
    if (!vm_str_reserve(s, n))
        return;
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void vm_str_append_len(vm_str_t *s, const char *text, size_t n)
{
    if (!text || n == 0)
        return;
    if (!vm_str_reserve(s, n))
        return;
    memcpy(s->data + s->len, text, n);
    s->len += n;
    s->data[s->len] = '\0';
}

void vm_str_appendf(vm_str_t *s, const char *fmt, ...)
{
    char stack_buf[512];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    va_end(args);
    if (n < 0)
        return;

    if ((size_t)n < sizeof(stack_buf)) {
        vm_str_append(s, stack_buf);
        return;
    }

    /* Longer than the scratch buffer: format straight into the target. */
    if (!vm_str_reserve(s, (size_t)n))
        return;
    va_start(args, fmt);
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, args);
    va_end(args);
    s->len += (size_t)n;
}

void vm_str_append_json(vm_str_t *s, const char *text)
{
    static const char hex[] = "0123456789abcdef";

    if (!text)
        return;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        switch (c) {
        case '"':  vm_str_append(s, "\\\""); break;
        case '\\': vm_str_append(s, "\\\\"); break;
        case '\n': vm_str_append(s, "\\n"); break;
        case '\r': vm_str_append(s, "\\r"); break;
        case '\t': vm_str_append(s, "\\t"); break;
        case '\b': vm_str_append(s, "\\b"); break;
        case '\f': vm_str_append(s, "\\f"); break;
        default:
            if (c < 0x20) {
                char esc[7] = { '\\', 'u', '0', '0', 0, 0, 0 };
                esc[4] = hex[(c >> 4) & 0xF];
                esc[5] = hex[c & 0xF];
                vm_str_append(s, esc);
            } else {
                char one[2] = { (char)c, '\0' };
                vm_str_append(s, one);
            }
            break;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* filesystem                                                                 */
/* -------------------------------------------------------------------------- */

int vm_mkdir_p(const char *path)
{
    char work[256];
    char *p;
    int rc = 0;

    vm_strlcpy(work, path, sizeof(work));
    if (work[0] == '\0')
        return -1;

    /* Skip a leading device prefix such as "ux0:"; the device node itself
     * already exists and must not be created. */
    p = strchr(work, ':');
    if (p) {
        p++;
        if (*p == '/')
            p++;
    } else {
        p = work;
    }

    for (; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        rc = sceIoMkdir(work, 0777);
        *p = '/';
    }
    rc = sceIoMkdir(work, 0777);
    /* An already-existing directory is expected on repeat runs and is not a
     * failure, but the directory has to actually be there: SceIoStat is the
     * only reliable way to tell "exists" from "creation refused". */
    if (rc < 0) {
        SceIoStat st;
        if (sceIoGetstat(work, &st) < 0)
            return rc;
    }
    return 0;
}

bool vm_file_exists(const char *path)
{
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

long vm_file_size(const char *path)
{
    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0)
        return -1;
    return (long)st.st_size;
}

bool vm_copy_file(const char *src, const char *dst)
{
    SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
    SceUID out;
    char buf[8192];
    int n;
    bool ok = true;

    if (in < 0)
        return false;
    out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (out < 0) {
        sceIoClose(in);
        return false;
    }

    while ((n = sceIoRead(in, buf, sizeof(buf))) > 0) {
        if (sceIoWrite(out, buf, (unsigned)n) != n) {
            ok = false;
            break;
        }
    }
    if (n < 0)
        ok = false;

    sceIoClose(out);
    sceIoClose(in);
    return ok;
}
