#include "net.h"
#include "config.h"

#include <psp2/io/fcntl.h>
#include <psp2/net/net.h>
#include <psp2/sysmodule.h>

#include <curl/curl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VM_NET_ERR_LEN 192

static void *g_net_pool;
static int g_net_up;
static int g_ca_ok;
static char g_error[VM_NET_ERR_LEN];

static void set_error(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_error, sizeof(g_error), fmt, args);
    va_end(args);
}

const char *vm_net_last_error(void)
{
    return g_error[0] ? g_error : "no error";
}

bool vm_net_verify_enabled(void)
{
    return g_ca_ok != 0;
}

/* -------------------------------------------------------------------------- */
/* request plumbing                                                           */
/* -------------------------------------------------------------------------- */

/* Where a response goes: either a growable string or a file on ux0:. */
struct sink {
    vm_str_t *str;
    SceUID fd;
    int64_t written;
};

struct req_ctx {
    struct sink sink;
    vm_dl_progress_t *progress;
    volatile int *cancel;
};

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct req_ctx *ctx = (struct req_ctx *)userdata;
    size_t bytes = size * nmemb;

    if (ctx->sink.fd >= 0) {
        int n = sceIoWrite(ctx->sink.fd, ptr, (unsigned int)bytes);
        if (n < 0 || (size_t)n != bytes)
            return 0;
    } else if (ctx->sink.str) {
        /* Refuse to grow past the cap instead of exhausting the heap on a
         * runaway response. */
        if (ctx->sink.str->len + bytes > (size_t)VM_JSON_MAX_BYTES)
            return 0;
        vm_str_append_len(ctx->sink.str, ptr, bytes);
    }

    ctx->sink.written += (int64_t)bytes;
    return bytes;
}

static int progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    struct req_ctx *ctx = (struct req_ctx *)userdata;

    (void)ultotal;
    (void)ulnow;

    if (ctx->progress) {
        ctx->progress->downloaded = (int64_t)dlnow;
        ctx->progress->total = dltotal > 0 ? (int64_t)dltotal : -1;
    }
    if (ctx->cancel && *ctx->cancel)
        return 1; /* aborts with CURLE_ABORTED_BY_CALLBACK */
    return 0;
}

/*
 * Runs one request. `out` collects the body in memory, `out_path` streams it to
 * a file instead - exactly one of the two must be set. `verify` selects whether
 * the CA bundle is enforced; only the CA download itself runs without it.
 */
static int request_run(const char *method, const char *url,
                       const vm_http_header_t *headers, int nheaders,
                       const char *body, vm_str_t *out, const char *out_path,
                       int verify, vm_dl_progress_t *progress,
                       volatile int *cancel, long *status_out, CURLcode *rc_out)
{
    CURL *curl;
    CURLcode rc;
    struct curl_slist *slist = NULL;
    struct curl_slist *grown;
    struct req_ctx ctx;
    long status = 0;
    int i;
    int ret = -1;

    memset(&ctx, 0, sizeof(ctx));
    ctx.sink.fd = -1;
    ctx.sink.str = out;
    ctx.progress = progress;
    ctx.cancel = cancel;

    if (progress) {
        progress->downloaded = 0;
        progress->total = -1;
    }

    if (out_path) {
        ctx.sink.str = NULL;
        ctx.sink.fd = sceIoOpen(out_path,
                                SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (ctx.sink.fd < 0) {
            set_error("cannot write %s", out_path);
            return -1;
        }
    }

    for (i = 0; i < nheaders; i++) {
        vm_str_t line;

        vm_str_init(&line);
        vm_str_appendf(&line, "%s: %s", headers[i].name, headers[i].value);
        grown = curl_slist_append(slist, line.data ? line.data : "");
        vm_str_free(&line);
        if (!grown) {
            set_error("out of memory building headers");
            goto done;
        }
        slist = grown;
    }

    curl = curl_easy_init();
    if (!curl) {
        set_error("curl_easy_init failed");
        goto done;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    /* An empty string asks curl to advertise every encoding it can actually
     * decode, so nothing arrives compressed that we cannot unpack. */
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, VM_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, VM_HTTP_CONNECT_TIMEOUT);

    if (verify) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_CAINFO, VM_CA_FILE);
    } else {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    if (out_path) {
        /* A slow start is normal, a stalled transfer is not. */
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, VM_HTTP_LOW_SPEED);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, VM_HTTP_LOW_SPEED_TIME);
    } else {
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, VM_HTTP_TIMEOUT);
    }

    if (slist)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

    if (body || strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                         (long)(body ? strlen(body) : 0));
    }

    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc_out)
        *rc_out = rc;

    if (rc != CURLE_OK) {
        if (cancel && *cancel)
            set_error("cancelled");
        else
            set_error("%s", curl_easy_strerror(rc));
        goto done;
    }

    if (status < 200 || status >= 300) {
        set_error("server returned HTTP %ld", status);
        goto done;
    }

    ret = 0;

done:
    curl_slist_free_all(slist);
    if (ctx.sink.fd >= 0) {
        sceIoClose(ctx.sink.fd);
        ctx.sink.fd = -1;
        /* Never leave a half written file behind: a truncated m4a or JPEG
         * would be picked up as valid on the next run. */
        if (ret != 0 && out_path)
            sceIoRemove(out_path);
    }
    if (status_out)
        *status_out = status;
    return ret;
}

static int do_request(const char *method, const char *url,
                      const vm_http_header_t *headers, int nheaders,
                      const char *body, vm_str_t *out, const char *out_path,
                      int verify, vm_dl_progress_t *progress,
                      volatile int *cancel, long *status_out)
{
    CURLcode rc = CURLE_OK;
    long status = 0;
    int ret;

    ret = request_run(method, url, headers, nheaders, body, out, out_path,
                      verify, progress, cancel, &status, &rc);

    /* A stored CA bundle that turns out unusable (truncated or grown stale)
     * makes every request die with a TLS error. Fall back to unverified for
     * the session so the app keeps working, and repair the bundle next boot. */
    if (ret != 0 && verify &&
        (rc == CURLE_SSL_CACERT || rc == CURLE_SSL_CACERT_BADFILE ||
         rc == CURLE_PEER_FAILED_VERIFICATION)) {
        vm_log("net: TLS verify failed (%s), retrying unverified\n",
                 curl_easy_strerror(rc));
        g_ca_ok = 0;
        status = 0;
        ret = request_run(method, url, headers, nheaders, body, out, out_path,
                          0, progress, cancel, &status, NULL);
    }

    if (status_out)
        *status_out = status;
    return ret;
}

/* -------------------------------------------------------------------------- */
/* public API                                                                 */
/* -------------------------------------------------------------------------- */

int vm_net_init(void)
{
    SceNetInitParam param;

    if (g_net_up)
        return 0;

    vm_mkdir_p(VM_DATA_DIR);
    vm_mkdir_p(VM_CACHE_DIR);

    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0)
        vm_log("net: SCE_SYSMODULE_NET load failed\n");
    /* curl keeps its own TLS stack, but the SSL/HTTPS modules are loaded
     * defensively: some builds hand the handshake to SceSsl. */
    sceSysmoduleLoadModule(SCE_SYSMODULE_SSL);
    sceSysmoduleLoadModule(SCE_SYSMODULE_HTTPS);

    g_net_pool = malloc(VM_NET_POOL_BYTES);
    if (!g_net_pool) {
        set_error("out of memory for the network pool");
        return -1;
    }

    param.memory = g_net_pool;
    param.size = VM_NET_POOL_BYTES;
    param.flags = 0;
    if (sceNetInit(&param) < 0)
        vm_log("net: sceNetInit failed\n");

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        set_error("curl_global_init failed");
        return -1;
    }

    g_net_up = 1;

    if (vm_file_exists(VM_CA_FILE) &&
        vm_file_size(VM_CA_FILE) >= VM_CA_MIN_BYTES) {
        g_ca_ok = 1;
    } else {
        /* Present but unusable (empty/truncated) has to go: existence alone
         * is not enough for curl to trust the bundle. */
        if (vm_file_exists(VM_CA_FILE)) {
            vm_log("net: stale CA bundle, removing\n");
            sceIoRemove(VM_CA_FILE);
        }
        vm_log("net: downloading the CA bundle\n");
        /* Chicken and egg: the bundle has to be fetched before it can be used,
         * so this single request runs unverified and everything after it is
         * pinned to the bundle it produced. */
        if (do_request("GET", VM_CA_URL, NULL, 0, NULL, NULL, VM_CA_FILE, 0,
                       NULL, NULL, NULL) == 0 &&
            vm_file_exists(VM_CA_FILE) &&
            vm_file_size(VM_CA_FILE) >= VM_CA_MIN_BYTES) {
            g_ca_ok = 1;
            vm_log("net: CA bundle ready\n");
        } else {
            g_ca_ok = 0;
            vm_log("net: CA bundle unavailable (%s), TLS verification off\n",
                     vm_net_last_error());
        }
    }

    return 0;
}

void vm_net_term(void)
{
    if (!g_net_up)
        return;
    curl_global_cleanup();
    sceNetTerm();
    free(g_net_pool);
    g_net_pool = NULL;
    g_net_up = 0;
}

int vm_net_post_json(const char *url, const vm_http_header_t *headers,
                       int nheaders, const char *body, vm_str_t *out,
                       long *status_out)
{
    if (!g_net_up && vm_net_init() != 0)
        return -1;
    return do_request("POST", url, headers, nheaders, body, out, NULL,
                      g_ca_ok, NULL, NULL, status_out);
}

int vm_net_get(const char *url, const vm_http_header_t *headers,
                 int nheaders, vm_str_t *out, long *status_out)
{
    if (!g_net_up && vm_net_init() != 0)
        return -1;
    return do_request("GET", url, headers, nheaders, NULL, out, NULL, g_ca_ok,
                      NULL, NULL, status_out);
}

int vm_net_download(const char *url, const char *path,
                      const vm_http_header_t *headers, int nheaders,
                      vm_dl_progress_t *progress, volatile int *cancel)
{
    if (!g_net_up && vm_net_init() != 0)
        return -1;
    return do_request("GET", url, headers, nheaders, NULL, NULL, path, g_ca_ok,
                      progress, cancel, NULL);
}

/* -------------------------------------------------------------------------- */
/* HLS                                                                         */
/* -------------------------------------------------------------------------- */

/* Lines split into heap copies? No: each playlist is a few KB, so we walk a
 * single buffer instead. Everything UTC is done on slices of that buffer. */

static int hls_next_line(char **cursor, char **start, size_t *len)
{
    char *p = *cursor;

    if (!p || !*p)
        return -1;
    *start = p;
    p = strstr(p, "\n");
    if (p) {
        *len = (size_t)(p - *start);
        *cursor = p + 1;
    } else {
        *len = strlen(*start);
        *cursor = *start + *len;
    }
    while (*len > 0 && ((*start)[*len - 1] == '\r'))
        (*len)--;
    return 0;
}

/* True when the reply looks like a master (: has #EXT-X-MEDIA) rather than an
 * audio playlist (a list of #EXTINF segments). */
static int hls_is_master(const char *pl)
{
    return pl && strstr(pl, "#EXT-X-MEDIA") != NULL;
}

/* Pulls the `URI="..."` out of an #EXT-X-MEDIA tag. */
static int hls_media_uri(const char *line, size_t len, char *out,
                         size_t out_cap)
{
    const char *uri;

    if (strncmp(line, "#EXT-X-MEDIA:", 13) != 0)
        return -1;
    uri = strstr(line, "TYPE=AUDIO");
    if (!uri || uri > line + len)
        return -1;
    uri = strstr(line, "URI=\"");
    if (!uri || (size_t)(uri - line) >= len)
        return -1;
    uri += 5;
    {
        size_t n = strcspn(uri, "\"");
        if (n == 0 || n >= out_cap)
            return -1;
        memcpy(out, uri, n);
        out[n] = '\0';
        return 0;
    }
}

static int hls_prefer_media(const char *a, const char *b)
{
    if (strstr(a, "/itag/234/"))
        return strstr(b, "/itag/234/") ? 0 : 1;
    if (strstr(b, "/itag/234/"))
        return -1;
    if (strstr(a, "/itag/233/"))
        return strstr(b, "/itag/233/") ? 0 : 1;
    if (strstr(b, "/itag/233/"))
        return -1;
    return 0;
}

static int hls_grab_list(const char *url, const vm_http_header_t *headers,
                         int nheaders, vm_str_t *out)
{
    long status = 0;

    if (!url)
        return -1;
    if (vm_net_get(url, headers, nheaders, out, &status) != 0) {
        /* vm_net_last_error() already holds the reason. */
        return -1;
    }
    if (!out->data || !*out->data)
        return -1;
    return 0;
}

int vm_net_hls_download(const char *playlist_url, const char *path,
                          const vm_http_header_t *headers, int nheaders,
                          vm_dl_progress_t *progress, volatile int *cancel)
{
    vm_str_t pl;
    vm_str_t best;
    const char *audio_uri = NULL;
    char *cursor;
    char *line;
    size_t len;
    char uri_buf[4096];
    SceUID fd = -1;
    int seg_index = 0;
    int total_segments = 0;
    int rc = 0;

    if (!g_net_up && vm_net_init() != 0)
        return -1;

    vm_str_init(&pl);
    if (hls_grab_list(playlist_url, headers, nheaders, &pl) != 0) {
        vm_str_free(&pl);
        return -1;
    }

    /* A master playlist references the audio groups; pick the AAC-LC one
     * (itag 234) over the HE-AAC (itag 233) and follow it. */
    if (hls_is_master(pl.data)) {
        vm_str_init(&best);
        cursor = pl.data;
        while (hls_next_line(&cursor, &line, &len) == 0) {
            if (hls_media_uri(line, len, uri_buf, sizeof(uri_buf)) == 0) {
                if (!audio_uri) {
                    vm_str_append(&best, uri_buf);
                    audio_uri = best.data;
                } else if (hls_prefer_media(uri_buf, audio_uri) > 0) {
                    vm_str_clear(&best);
                    vm_str_append(&best, uri_buf);
                    audio_uri = best.data;
                }
            }
        }
        if (!audio_uri) {
            set_error("hls: master has no AUDIO media group");
            vm_str_free(&best);
            vm_str_free(&pl);
            return -1;
        }
        vm_str_free(&pl);
        vm_str_init(&pl);
        rc = hls_grab_list(audio_uri, headers, nheaders, &pl);
        vm_str_free(&best);
        if (rc != 0) {
            vm_str_free(&pl);
            return -1;
        }
    }

    /* Count the segments: every non-empty, non-tag line is a segment URI. */
    cursor = pl.data;
    while (hls_next_line(&cursor, &line, &len) == 0)
        if (*line && *line != '#')
            total_segments++;
    if (total_segments == 0) {
        set_error("hls: playlist has no segments");
        vm_str_free(&pl);
        return -1;
    }
    vm_log("hls: %d segments\n", total_segments);

    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        set_error("hls: cannot open %s", path);
        vm_str_free(&pl);
        return -1;
    }

    if (progress) {
        progress->downloaded = 0;
        progress->total = total_segments;
    }

    cursor = pl.data;
    {
        vm_str_t seg;

        vm_str_init(&seg);
        while (hls_next_line(&cursor, &line, &len) == 0) {
            if (!*line || *line == '#')
                continue;
            line[len] = '\0'; /* terminate the URI for curl */
            vm_str_clear(&seg);
            if (vm_net_get(line, headers, nheaders, &seg, NULL) != 0) {
                set_error("hls: segment %d: %s", seg_index,
                          vm_net_last_error());
                rc = -1;
                break;
            }
            if (seg.len > 0 &&
                sceIoWrite(fd, seg.data, (SceSize)seg.len) != (int)seg.len) {
                set_error("hls: write %s failed", path);
                rc = -1;
                break;
            }
            seg_index++;
            if (progress)
                progress->downloaded = seg_index;
            if (cancel && *cancel) {
                rc = 1;
                break;
            }
        }
        vm_str_free(&seg);
    }

    sceIoClose(fd);
    vm_str_free(&pl);

    if (rc == 1) {
        set_error("hls download cancelled");
        sceIoRemove(path);
        rc = -1;
    } else if (rc != 0) {
        sceIoRemove(path);
    } else {
        vm_log("hls: stored %d segments in %s\n", seg_index, path);
        if (progress)
            progress->downloaded = progress->total;
    }
    return rc;
}

void vm_net_url_encode(vm_str_t *out, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!in)
        return;

    for (; *in; in++) {
        unsigned char c = (unsigned char)*in;

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            char one[2] = { (char)c, '\0' };
            vm_str_append(out, one);
        } else {
            char esc[4] = { '%', hex[(c >> 4) & 0xF], hex[c & 0xF], '\0' };
            vm_str_append(out, esc);
        }
    }
}
