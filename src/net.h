/*
 * VitaMusic - HTTP layer built on libcurl.
 *
 * One easy handle per request, so the UI worker thread and the playback thread
 * can both talk to the network without sharing state. Responses are either
 * collected in memory (API calls) or streamed straight to a file (audio,
 * album art, the CA bundle).
 */
#ifndef VM_NET_H
#define VM_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h"

typedef struct {
    const char *name;
    const char *value;
} vm_http_header_t;

typedef struct {
    int64_t downloaded;
    int64_t total; /* -1 while the server has not told us the length */
} vm_dl_progress_t;

/* Brings up SceNet, libcurl and the CA bundle. Safe to call twice. */
int vm_net_init(void);
void vm_net_term(void);

/* True when the CA bundle is missing and TLS verification had to be dropped.
 * The UI surfaces this instead of failing silently. */
bool vm_net_verify_enabled(void);

/* Human readable reason for the most recent failure. */
const char *vm_net_last_error(void);

/* POSTs `body` (already serialised) to `url` and reads the reply into `out`. */
int vm_net_post_json(const char *url, const vm_http_header_t *headers,
                       int nheaders, const char *body, vm_str_t *out,
                       long *status_out);

/* GETs `url` and reads the reply into `out`. */
int vm_net_get(const char *url, const vm_http_header_t *headers,
                 int nheaders, vm_str_t *out, long *status_out);

/* GETs `url` into `path`. `progress` and `cancel` may be NULL; a non-zero
 * `*cancel` aborts the transfer and removes the partial file. */
int vm_net_download(const char *url, const char *path,
                      const vm_http_header_t *headers, int nheaders,
                      vm_dl_progress_t *progress, volatile int *cancel);

/* Follows an HLS playlist (master or audio, plain or CRLF) and streams every
 * segment in order into `path`. The caller keeps `path`; a partial file is
 * removed on error and on cancel. */
int vm_net_hls_download(const char *playlist_url, const char *path,
                          const vm_http_header_t *headers, int nheaders,
                          vm_dl_progress_t *progress, volatile int *cancel);

/* RFC 3986 percent-encoding of `in`, appended to `out`. */
void vm_net_url_encode(vm_str_t *out, const char *in);

#endif /* VM_NET_H */
