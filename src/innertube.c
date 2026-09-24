#include "innertube.h"
#include "config.h"
#include "net.h"
#include "util.h"

#include "json.h"

#include <psp2/io/fcntl.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_visitor[768];
static int g_visitor_ready;

/* -------------------------------------------------------------------------- */
/* JSON helpers                                                               */
/*                                                                            */
/* Every accessor used here checks the type first: a malformed or re-shaped    */
/* response must not abort the app.                                           */
/* -------------------------------------------------------------------------- */

static json_t *obj(json_t *o, const char *key)
{
    if (!json_is_object(o))
        return NULL;
    return json_object_get(o, key);
}

static json_t *arr(json_t *o, size_t index)
{
    if (!json_is_array(o))
        return NULL;
    return json_array_get(o, index);
}

static size_t arr_size(json_t *o)
{
    return json_is_array(o) ? json_array_size(o) : 0;
}

static const char *str_of(json_t *o)
{
    return json_is_string(o) ? json_string_value(o) : NULL;
}

static const char *obj_str(json_t *o, const char *key)
{
    return str_of(obj(o, key));
}

static int obj_int(json_t *o, const char *key, int fallback)
{
    json_t *v = obj(o, key);

    return json_is_integer(v) ? (int)json_integer_value(v) : fallback;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    vm_strlcpy(dst, src ? src : "", cap);
}

/* vm_trim() only reports where the trimmed text starts, so shift it down. */
static void trim_in_place(char *s)
{
    char *trimmed = vm_trim(s);

    if (trimmed != s)
        memmove(s, trimmed, strlen(trimmed) + 1);
}

/* -------------------------------------------------------------------------- */
/* request building                                                           */
/* -------------------------------------------------------------------------- */

/* `which`: 0 = ANDROID, 1 = VISIONOS, 2 = WEB_EMBEDDED. The search client
 * (which < 0 handled by the caller) is untouched by this. */
static void append_client_ex(vm_str_t *s, int which)
{
    const char *name = YT_PLAYER_CLIENT_NAME;
    const char *version = YT_PLAYER_CLIENT_VERSION;

    if (which == 2) {
        name = YT_FALLBACK2_CLIENT_NAME;
        version = YT_FALLBACK2_CLIENT_VERSION;
    } else if (which == 1) {
        name = YT_FALLBACK_CLIENT_NAME;
        version = YT_FALLBACK_CLIENT_VERSION;
    } else if (which == -1) {
        name = YTM_WEB_CLIENT_NAME;
        version = YTM_WEB_VERSION;
    }

    vm_str_append(s, "\"client\":{\"clientName\":\"");
    vm_str_append(s, name);
    vm_str_append(s, "\",\"clientVersion\":\"");
    vm_str_append(s, version);
    vm_str_append(s, "\",\"gl\":\"US\",\"hl\":\"en\"");

    if (which == 0) {
        vm_str_append(s,
                        ",\"osName\":\"" YT_PLAYER_OS_NAME "\","
                        "\"osVersion\":\"" YT_PLAYER_OS_VERSION "\","
                        "\"androidSdkVersion\":");
        {
            char sdk[16];

            snprintf(sdk, sizeof(sdk), "%d", YT_PLAYER_SDK);
            vm_str_append(s, sdk);
        }
    } else if (which == 1) {
        vm_str_append(s,
                        ",\"osName\":\"" YT_FALLBACK_OS_NAME "\","
                        "\"osVersion\":\"" YT_FALLBACK_OS_VERSION "\","
                        "\"deviceMake\":\"" YT_FALLBACK_DEVICE_MAKE "\","
                        "\"deviceModel\":\"" YT_FALLBACK_DEVICE_MODEL "\"");
    }

    if (g_visitor[0]) {
        vm_str_append(s, ",\"visitorData\":\"");
        vm_str_append_json(s, g_visitor);
        vm_str_append(s, "\"");
    }

    vm_str_append(s, "}");

    /* WEB_EMBEDDED needs a non-YouTube thirdParty.embedUrl or YouTube treats
     * the request as a regular WEB player and cipher-locks the formats. */
    if (which == 2) {
        vm_str_append(s, ",\"thirdParty\":{\"embedUrl\":\"");
        vm_str_append_json(s, YT_FALLBACK2_EMBED_URL);
        vm_str_append(s, "\"}");
    }
}

static void append_client(vm_str_t *s, int player)
{
    append_client_ex(s, player ? 0 : -1);
}

static int fill_headers_ex(vm_http_header_t *headers, int which)
{
    const char *client_id = YT_PLAYER_CLIENT_ID;
    const char *client_version = YT_PLAYER_CLIENT_VERSION;
    const char *user_agent = YT_PLAYER_USER_AGENT;
    const char *origin = YT_WWW_ORIGIN;
    const char *referer = YT_WWW_ORIGIN "/";
    int n = 0;

    if (which == 2) {
        client_id = YT_FALLBACK2_CLIENT_ID;
        client_version = YT_FALLBACK2_CLIENT_VERSION;
        user_agent = YT_FALLBACK2_USER_AGENT;
        referer = YT_WWW_ORIGIN "/embed/";
    } else if (which == 1) {
        client_id = YT_FALLBACK_CLIENT_ID;
        client_version = YT_FALLBACK_CLIENT_VERSION;
        user_agent = YT_FALLBACK_USER_AGENT;
    } else if (which == -1) {
        client_id = YTM_WEB_CLIENT_ID;
        client_version = YTM_WEB_VERSION;
        user_agent = YTM_WEB_USER_AGENT;
        origin = YTM_ORIGIN;
        referer = YTM_ORIGIN "/";
    } else if (which == 0) {
        /* ANDROID player on www.youtube.com */
    }

    headers[n++] = (vm_http_header_t){ "Content-Type", "application/json" };
    headers[n++] = (vm_http_header_t){ "X-Goog-Api-Format-Version", "1" };
    headers[n++] = (vm_http_header_t){ "X-YouTube-Client-Name", client_id };
    headers[n++] = (vm_http_header_t){ "X-YouTube-Client-Version",
                                         client_version };
    headers[n++] = (vm_http_header_t){ "X-Origin", origin };
    headers[n++] = (vm_http_header_t){ "Origin", origin };
    headers[n++] = (vm_http_header_t){ "Referer", referer };
    headers[n++] = (vm_http_header_t){ "User-Agent", user_agent };
    if (g_visitor[0])
        headers[n++] = (vm_http_header_t){ "X-Goog-Visitor-Id", g_visitor };

    return n;
}

static int fill_headers(vm_http_header_t *headers, int player)
{
    return fill_headers_ex(headers, player ? 0 : -1);
}

/* -------------------------------------------------------------------------- */
/* visitor id                                                                 */
/* -------------------------------------------------------------------------- */

/* Visitor ids, or the visitor portion nested in them, are a run of URL-safe
 * base64-ish bytes that always carries a "Cg"/"Ch" protobuf prefix and that
 * grows every time YouTube merges another token into it (it is 500+ chars
 * today), so only the character set and a generous length window are pinned. */
static int looks_like_visitor(const char *s)
{
    size_t len;
    const char *p;

    if (!s)
        return 0;
    len = strlen(s);
    if (len < 24 || len > 640)
        return 0;
    if (strncmp(s, "Cg", 2) != 0 && strncmp(s, "Ch", 2) != 0)
        return 0;

    for (p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;

        if (isalnum(c) || c == '_' || c == '-' || c == '%' || c == '=' ||
            c == '.' || c == '+' || c == '~')
            continue;
        return 0;
    }
    return 1;
}

static int load_visitor_from_file(void)
{
    SceUID fd;
    char buf[768];
    int n;

    fd = sceIoOpen(VM_VISITOR_FILE, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;
    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0)
        return -1;

    buf[n] = '\0';
    trim_in_place(buf);
    if (!looks_like_visitor(buf))
        return -1;

    copy_str(g_visitor, sizeof(g_visitor), buf);
    return 0;
}
static void save_visitor_to_file(void)
{
    SceUID fd = sceIoOpen(VM_VISITOR_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);

    if (fd < 0)
        return;
    sceIoWrite(fd, g_visitor, (unsigned int)strlen(g_visitor));
    sceIoClose(fd);
}

/*
 * The player bootstrap script carries a freshly minted visitor id in a JSON
 * blob behind a `)]}'` guard, which is exactly where the Android client reads
 * it from too. Its position wobbles between responses, so the whole document
 * is walked and the longest visitor-shaped string wins: the "identity" token
 * YouTube merges in today dwarfs every other visitor-sized value.
 */
static void scan_for_visitor(json_t *node, size_t *best_len)
{
    size_t i, n;

    if (!node)
        return;

    if (json_is_string(node)) {
        const char *s = json_string_value(node);
        size_t len = s ? strlen(s) : 0;

        if (len > *best_len && looks_like_visitor(s)) {
            copy_str(g_visitor, sizeof(g_visitor), s);
            *best_len = len;
        }
        return;
    }

    if (json_is_array(node)) {
        n = json_array_size(node);
        for (i = 0; i < n; i++)
            scan_for_visitor(json_array_get(node, i), best_len);
        return;
    }

    if (json_is_object(node)) {
        n = json_object_size(node);
        for (i = 0; i < n; i++)
            scan_for_visitor(json_object_value_at(node, i), best_len);
    }
}

static int fetch_visitor(void)
{
    vm_str_t body;
    json_error_t err;
    json_t *root;
    size_t best_len = 0;
    int ret = -1;

    vm_str_init(&body);

    if (vm_net_get(YTM_ORIGIN "/sw.js_data", NULL, 0, &body, NULL) != 0) {
        vm_log("innertube: sw.js_data failed: %s\n", vm_net_last_error());
        vm_str_free(&body);
        return -1;
    }

    {
        const char *text = body.data ? body.data : "";
        size_t skip = 0;

        /* Strip the XSSI guard line, however many characters it uses. */
        if (strncmp(text, ")]}'", 4) == 0) {
            skip = 4;
            while (text[skip] == '\n' || text[skip] == '\r')
                skip++;
        }

        root = json_loads(text + skip, 0, &err);
    }
    vm_str_free(&body);

    if (!root) {
        vm_log("innertube: sw.js_data is not JSON: %s\n", err.text);
        return -1;
    }

    scan_for_visitor(root, &best_len);
    if (best_len > 0)
        ret = 0;

    json_decref(root);

    if (ret != 0)
        vm_log("innertube: no visitor id in sw.js_data\n");
    return ret;
}

int it_init(void)
{
    if (g_visitor_ready)
        return 0;

    if (load_visitor_from_file() == 0) {
        g_visitor_ready = 1;
        vm_log("innertube: visitor id from cache\n");
    }
    /* Missing visitor is fine here; it_refresh_visitor() fetches one. */
    return 0;
}

int it_refresh_visitor(void)
{
    if (g_visitor_ready)
        return 0;

    if (vm_net_init() != 0)
        return -1;

    if (load_visitor_from_file() == 0) {
        g_visitor_ready = 1;
        vm_log("innertube: visitor id from cache\n");
        return 0;
    }

    if (fetch_visitor() != 0) {
        /* Search still works anonymously; playback is more reliable with a
         * visitor id, but missing one is not fatal. */
        vm_log("innertube: continuing without a visitor id\n");
        return 0;
    }

    save_visitor_to_file();
    g_visitor_ready = 1;
    vm_log("innertube: visitor id acquired\n");
    return 0;
}

int it_visitor_ready(void)
{
    return g_visitor_ready;
}

/* -------------------------------------------------------------------------- */
/* search                                                                     */
/* -------------------------------------------------------------------------- */

static int parse_time_text(const char *text)
{
    int parts[3] = { 0, 0, 0 };
    int n = 0;
    const char *p = text;

    if (!text)
        return 0;

    while (*p && n < 3) {
        int value = 0;
        int digits = 0;

        while (*p && !isdigit((unsigned char)*p))
            p++;
        while (*p && isdigit((unsigned char)*p)) {
            value = value * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 6)
                break;
        }
        if (digits == 0)
            break;
        parts[n++] = value;
    }

    if (n == 2)
        return parts[0] * 60 + parts[1];
    if (n == 3)
        return parts[0] * 3600 + parts[1] * 60 + parts[2];
    return 0;
}

/* Concatenates every run of a flex column into one plain string. */
static void join_runs(json_t *runs, char *dst, size_t cap)
{
    size_t i, n = arr_size(runs);
    size_t used = 0;

    dst[0] = '\0';
    for (i = 0; i < n; i++) {
        const char *text = obj_str(arr(runs, i), "text");
        size_t len;

        if (!text)
            continue;
        len = strlen(text);
        if (used + len + 1 >= cap)
            break;
        memcpy(dst + used, text, len);
        used += len;
        dst[used] = '\0';
    }
}

static json_t *flex_runs(json_t *renderer, int index)
{
    json_t *column = arr(obj(renderer, "flexColumns"), (size_t)index);
    json_t *text = obj(obj(column, "musicResponsiveListItemFlexColumnRenderer"),
                       "text");

    return obj(text, "runs");
}

static const char *renderer_video_id(json_t *renderer)
{
    const char *id;

    id = obj_str(obj(renderer, "playlistItemData"), "videoId");
    if (id && *id)
        return id;

    id = obj_str(obj(obj(renderer, "navigationEndpoint"), "watchEndpoint"),
                 "videoId");
    if (id && *id)
        return id;

    id = obj_str(obj(obj(obj(obj(obj(renderer, "overlay"),
                                   "musicItemThumbnailOverlayRenderer"),
                             "content"),
                       "musicPlayButtonRenderer"),
                 "playNavigationEndpoint"),
                 "watchEndpoint");
    if (id && *id)
        return id;

    /* Last resort: the title run itself links to the track. */
    id = obj_str(obj(obj(arr(flex_runs(renderer, 0), 0), "navigationEndpoint"),
                     "watchEndpoint"),
                 "videoId");
    return (id && *id) ? id : NULL;
}

static const char *renderer_thumb_url(json_t *renderer)
{
    json_t *thumbs = obj(obj(obj(renderer, "thumbnail"), "musicThumbnailRenderer"),
                         "thumbnail");
    json_t *list = obj(thumbs, "thumbnails");
    size_t n = arr_size(list);
    const char *url;

    if (n == 0)
        return NULL;
    /* The list is ordered smallest first, so the last entry is the biggest. */
    url = obj_str(arr(list, n - 1), "url");
    return (url && *url) ? url : NULL;
}

static int parse_item(json_t *renderer, vm_track_t *out)
{
    char secondary[512];
    const char *id;
    const char *title;
    const char *thumb;

    id = renderer_video_id(renderer);
    if (!id)
        return -1;

    {
        json_t *runs = flex_runs(renderer, 0);
        const char *first = obj_str(arr(runs, 0), "text");
        if (!first)
            return -1;
        title = first;
    }

    memset(out, 0, sizeof(*out));
    copy_str(out->id, sizeof(out->id), id);
    copy_str(out->title, sizeof(out->title), title);

    thumb = renderer_thumb_url(renderer);
    if (thumb)
        copy_str(out->thumb_url, sizeof(out->thumb_url), thumb);

    join_runs(flex_runs(renderer, 1), secondary, sizeof(secondary));
    trim_in_place(secondary);

    /* The secondary line reads "Artist • Album • 3:45" (album and duration are
     * dropped for some shelves, and separators vary by locale). */
    {
        char *segments[6];
        int count = 0;
        char *cursor = secondary;

        while (cursor && count < 6) {
            char *sep = strstr(cursor, "\xE2\x80\xA2"); /* U+2022 bullet */

            if (sep) {
                *sep = '\0';
                sep += 3;
            }
            segments[count++] = vm_trim(cursor);
            cursor = sep;
        }

        if (count >= 1)
            copy_str(out->artist, sizeof(out->artist), segments[0]);
        if (count >= 3)
            copy_str(out->album, sizeof(out->album), segments[1]);

        /* The duration is always the final segment. */
        if (count >= 2) {
            int seconds = parse_time_text(segments[count - 1]);

            if (seconds > 0)
                out->duration_s = seconds;
        }
    }

    return out->id[0] ? 0 : -1;
}

static void parse_shelf(json_t *shelf, vm_track_t *out, int max, int *count)
{
    json_t *contents = obj(shelf, "contents");
    size_t i, n = arr_size(contents);

    for (i = 0; i < n && *count < max; i++) {
        json_t *renderer =
            obj(arr(contents, i), "musicResponsiveListItemRenderer");

        if (!renderer)
            continue;
        if (parse_item(renderer, &out[*count]) == 0)
            (*count)++;
    }
}

int it_search(const char *query, vm_track_t *out, int max_results,
              int *out_count)
{
    vm_str_t body;
    vm_http_header_t headers[10];
    vm_str_t response;
    json_t *root;
    json_error_t err;
    int nheaders;
    int count = 0;
    int ret = -1;
    size_t t, nt, s, ns;

    if (!query || !*query || !out || !out_count)
        return -1;
    *out_count = 0;
    max_results = max_results > 0 ? max_results : VM_MAX_RESULTS;

    if (it_init() != 0)
        return -1;

    vm_str_init(&body);
    vm_str_append(&body, "{\"context\":{");
    append_client(&body, 0);
    vm_str_append(&body, "},\"query\":\"");
    vm_str_append_json(&body, query);
    vm_str_append(&body, "\",\"params\":\"" YTM_PARAM_SONGS "\"}");

    nheaders = fill_headers(headers, 0);

    vm_str_init(&response);
    if (vm_net_post_json(YTM_API_URL "search?prettyPrint=false", headers,
                           nheaders, body.data, &response, NULL) != 0) {
        vm_log("innertube: search failed: %s\n", vm_net_last_error());
        goto done;
    }

    root = json_loads(response.data ? response.data : "", 0, &err);
    if (!root) {
        vm_log("innertube: search reply is not JSON: %s\n", err.text);
        goto done;
    }

    {
        json_t *tabs = obj(obj(obj(root, "contents"),
                               "tabbedSearchResultsRenderer"),
                           "tabs");

        nt = arr_size(tabs);
        for (t = 0; t < nt && count < max_results; t++) {
            json_t *content = obj(obj(arr(tabs, t), "tabRenderer"), "content");
            json_t *sections =
                obj(obj(content, "sectionListRenderer"), "contents");

            ns = arr_size(sections);
            for (s = 0; s < ns && count < max_results; s++) {
                json_t *shelf =
                    obj(arr(sections, s), "musicShelfRenderer");

                if (shelf)
                    parse_shelf(shelf, out, max_results, &count);
            }
        }

        /* Continuation replies use a different envelope. */
        {
            json_t *cont = obj(obj(root, "continuationContents"),
                               "musicShelfContinuation");

            if (cont && count < max_results)
                parse_shelf(cont, out, max_results, &count);
        }
    }

    json_decref(root);
    *out_count = count;
    ret = count > 0 ? 0 : -1;
    if (ret != 0)
        vm_log("innertube: search returned no songs\n");

done:
    vm_str_free(&body);
    vm_str_free(&response);
    return ret;
}

/* -------------------------------------------------------------------------- */
/* playback stream                                                            */
/* -------------------------------------------------------------------------- */

/* True when mime is AAC we can decode: pure audio/mp4, or a muxed video/mp4
 * that carries mp4a (itag 18 from WEB_EMBEDDED). Opus/WebM is rejected. */
static int mime_is_playable_aac(const char *mime)
{
    if (!mime)
        return 0;
    if (strncmp(mime, "audio/mp4", 9) == 0)
        return 1;
    if (strncmp(mime, "video/mp4", 9) == 0 && strstr(mime, "mp4a"))
        return 1;
    return 0;
}

static int mime_is_opus(const char *mime)
{
    return mime && strncmp(mime, "audio/webm", 10) == 0;
}

/* Scans one formats array (adaptive or progressive). Updates *best_* and the
 * seen_* flags. `from_adaptive` only affects logging. */
static void scan_formats(json_t *formats, int from_adaptive,
                         json_t **best_fmt, int *best_bitrate,
                         int *seen_aac, int *seen_ciphered, int *seen_opus,
                         int *seen_dubbed, int *seen_too_long)
{
    size_t i, n = arr_size(formats);

    for (i = 0; i < n; i++) {
        json_t *fmt = arr(formats, i);
        const char *mime = obj_str(fmt, "mimeType");
        const char *url = obj_str(fmt, "url");
        const char *cipher = obj_str(fmt, "signatureCipher");
        json_t *track = obj(fmt, "audioTrack");
        int is_auto_dubbed = 0;

        if (!cipher)
            cipher = obj_str(fmt, "cipher");

        if (json_is_boolean(obj(track, "isAutoDubbed")))
            is_auto_dubbed = json_is_true(obj(track, "isAutoDubbed"));

        vm_log("innertube: fmt[%s%u] itag=%d mime=%s url=%s cipher=%s "
                 "dubbed=%d\n",
                 from_adaptive ? "a" : "p", (unsigned)i,
                 obj_int(fmt, "itag", -1), mime ? mime : "-",
                 url ? "yes" : "no", cipher ? "yes" : "no", is_auto_dubbed);

        if (!mime)
            continue;
        if (!mime_is_playable_aac(mime)) {
            if (mime_is_opus(mime))
                *seen_opus = 1;
            continue;
        }
        *seen_aac = 1;
        if (!url || !*url) {
            *seen_ciphered = 1; /* ciphered or missing URL ⇒ treated as locked */
            continue;
        }
        if (is_auto_dubbed) {
            *seen_dubbed = 1;
            continue;
        }
        if (strlen(url) >= VM_STREAM_URL_LEN) {
            *seen_too_long = 1;
            continue;
        }

        {
            int value = obj_int(fmt, "bitrate", 0);
            const char *best_mime =
                *best_fmt ? obj_str(*best_fmt, "mimeType") : NULL;

            /* Prefer pure audio/mp4 over muxed video/mp4 at equal bitrate. */
            if (!*best_fmt || value > *best_bitrate ||
                (value == *best_bitrate &&
                 strncmp(mime, "audio/mp4", 9) == 0 &&
                 (!best_mime || strncmp(best_mime, "audio/mp4", 9) != 0))) {
                *best_fmt = fmt;
                *best_bitrate = value;
            }
        }
    }
}

/* Picks the best un-ciphered AAC format. Opus (WebM) is not decoded by the
 * hardware codec, so an Opus-only track is reported as unplayable instead of
 * playing silence. Also accepts progressive muxed MP4 (itag 18) when that is
 * all a client offers. */
static int pick_format(json_t *root, vm_stream_t *out)
{
    json_t *streaming = obj(root, "streamingData");
    json_t *adaptive = obj(streaming, "adaptiveFormats");
    json_t *progressive = obj(streaming, "formats");
    json_t *best_fmt = NULL;
    int best_bitrate = 0;
    int seen_aac = 0;
    int seen_ciphered = 0;
    int seen_opus = 0;
    int seen_dubbed = 0;
    int seen_too_long = 0;

    scan_formats(adaptive, 1, &best_fmt, &best_bitrate, &seen_aac,
                 &seen_ciphered, &seen_opus, &seen_dubbed, &seen_too_long);
    scan_formats(progressive, 0, &best_fmt, &best_bitrate, &seen_aac,
                 &seen_ciphered, &seen_opus, &seen_dubbed, &seen_too_long);

    if (!best_fmt) {
        const char *why = "Solo hay audio incompatible";

        if (!seen_aac && seen_opus)
            why = "Solo hay audio Opus (no soportado)";
        else if (!seen_aac)
            why = "No hay formatos de audio reproducibles";
        else if (seen_too_long)
            why = "La URL del stream es demasiado larga";
        else if (seen_ciphered)
            why = "YouTube bloque\xc3\xb3 el stream (cifrado)";
        else if (seen_dubbed)
            why = "Solo hay audio doblado autom\xc3\xa1ticamente";

        vm_log("innertube: pick_format failed: %s\n", why);
        copy_str(out->mime, sizeof(out->mime), why);
        return -1;
    }

    {
        const char *mime = obj_str(best_fmt, "mimeType");
        const char *url = obj_str(best_fmt, "url");
        const char *approx = obj_str(best_fmt, "approxDurationMs");

        copy_str(out->url, sizeof(out->url), url);
        copy_str(out->mime, sizeof(out->mime), mime);
        out->itag = obj_int(best_fmt, "itag", 0);
        out->bitrate = obj_int(best_fmt, "bitrate", 0);
        out->expires_in = obj_int(streaming, "expiresInSeconds", 0);
        out->duration_s = approx && *approx ? atoi(approx) / 1000 : 0;
    }

    return 0;
}

static const char *client_label(int which)
{
    if (which == 1)
        return "viso";
    if (which == 2)
        return "embed";
    return "and";
}

int it_resolve_stream(const char *video_id, vm_stream_t *out)
{
    vm_str_t body;
    vm_http_header_t headers[10];
    vm_str_t response;
    json_t *root = NULL;
    json_error_t err;
    int nheaders;
    int ret = -1;
    int which;

    if (!video_id || !*video_id || !out)
        return -1;

    memset(out, 0, sizeof(*out));
    if (it_init() != 0)
        return -1;
    /* Ensure a visitor id exists before the player requests; without it YouTube
     * frequently answers LOGIN_REQUIRED. Idempotent once cached. */
    it_refresh_visitor();

    /* Prefer clients that still hand out plain URLs / HLS without a PO token
     * or a JS decipher: VISIONOS → ANDROID → WEB_EMBEDDED. */
    for (int i = 0; i < 3 && ret != 0; i++) {
        static const int kTry[] = { 1, 0, 2 };
        which = kTry[i];
        vm_str_init(&body);
        vm_str_init(&response);

        vm_str_append(&body, "{\"context\":{");
        append_client_ex(&body, which);
        vm_str_append(&body, "},\"videoId\":\"");
        vm_str_append_json(&body, video_id);
        vm_str_append(&body,
                        "\",\"contentCheckOk\":true,\"racyCheckOk\":true}");

        nheaders = fill_headers_ex(headers, which);

        if (vm_net_post_json(YT_WWW_API_URL "player?prettyPrint=false",
                               headers, nheaders, body.data, &response,
                               NULL) != 0) {
            vm_log("innertube: player[%s] failed: %s\n", client_label(which),
                     vm_net_last_error());
            vm_str_free(&body);
            vm_str_free(&response);
            continue;
        }

        root = json_loads(response.data ? response.data : "", 0, &err);
        if (!root) {
            vm_log("innertube: player[%s] reply is not JSON: %s\n",
                     client_label(which), err.text);
            vm_str_free(&body);
            vm_str_free(&response);
            continue;
        }

        {
            const char *status =
                obj_str(obj(root, "playabilityStatus"), "status");
            const char *reason =
                obj_str(obj(root, "playabilityStatus"), "reason");

            if (!status || strcmp(status, "OK") != 0) {
                vm_log("innertube: %s not playable via client %d (%s)\n",
                         video_id, which, status ? status : "no status");
                copy_str(out->mime, sizeof(out->mime),
                         reason ? reason : "No se puede reproducir");
                json_decref(root);
                root = NULL;
                vm_str_free(&body);
                vm_str_free(&response);
                continue;
            }
        }

        {
            /* Prefer HLS when present: ANDROID/VISIONOS HLS is not gated by
             * a PO token the way their DASH URLs are. */
            json_t *streaming = obj(root, "streamingData");
            const char *hls_url = obj_str(streaming, "hlsManifestUrl");

            if (hls_url && *hls_url &&
                strlen(hls_url) < VM_STREAM_URL_LEN) {
                out->hls = 1;
                copy_str(out->hls_url, sizeof(out->hls_url), hls_url);
                /* Metadata only; wipe the error text if formats are ciphered. */
                if (pick_format(root, out) != 0) {
                    copy_str(out->mime, sizeof(out->mime), "audio/mp4");
                    out->url[0] = '\0';
                    out->itag = 0;
                    out->bitrate = 0;
                }
                vm_log("innertube: %s using HLS via client %d\n", video_id,
                         which);
            } else if (pick_format(root, out) != 0) {
                vm_log("innertube: %s offers no playable stream via client "
                         "%d\n",
                         video_id, which);
                /* Keep the reason from pick_format in out->mime. */
                out->hls = 0;
                out->url[0] = '\0';
                json_decref(root);
                root = NULL;
                vm_str_free(&body);
                vm_str_free(&response);
                continue;
            }
        }

        {
            json_t *details = obj(root, "videoDetails");
            const char *length = obj_str(details, "lengthSeconds");
            int seconds = length ? atoi(length) : 0;

            if (seconds > 0)
                out->duration_s = seconds;
        }

        json_decref(root);
        root = NULL;
        out->client = which;
        vm_log("innertube: %s -> itag %d, %d kbps (client %d%s)\n", video_id,
                 out->itag, out->bitrate / 1000, which,
                 out->hls ? ", hls" : "");
        ret = 0;

        vm_str_free(&body);
        vm_str_free(&response);
    }

    return ret;
}

/* -------------------------------------------------------------------------- */
/* thumbnails                                                                 */
/* -------------------------------------------------------------------------- */

void it_thumbnail_url(const char *url, int px, char *out, size_t out_size)
{
    if (!url || !*url) {
        if (out_size)
            out[0] = '\0';
        return;
    }
    if (px <= 0)
        px = VM_THUMB_PX;

    /* i.ytimg.com serves fixed files, so the whole basename has to be swapped
     * out rather than a size parameter. */
    if (strstr(url, "i.ytimg.com")) {
        const char *quality = px >= 1200 ? "maxresdefault.jpg" : "hqdefault.jpg";
        const char *slash = strrchr(url, '/');

        if (slash) {
            size_t prefix = (size_t)(slash - url) + 1;

            if (prefix + strlen(quality) + 1 <= out_size) {
                memcpy(out, url, prefix);
                memcpy(out + prefix, quality, strlen(quality) + 1);
                return;
            }
        }
    }

    /* googleusercontent and ggpht take a size suffix: everything up to the
     * first '=' is the base, and "rj" asks for JPEG. */
    if (strstr(url, "googleusercontent.com") || strstr(url, "ggpht.com")) {
        const char *eq = strchr(url, '=');
        size_t base = eq ? (size_t)(eq - url) : strlen(url);

        /* Channel avatars carry an inline "-s120" that has to go too. */
        {
            size_t cut = base;

            while (cut > 0 && isdigit((unsigned char)url[cut - 1]))
                cut--;
            if (cut > 1 && cut < base && url[cut - 1] == 's' &&
                url[cut - 2] == '-')
                base = cut - 2;
        }

        if (base + 32 <= out_size) {
            memcpy(out, url, base);
            snprintf(out + base, out_size - base, "=w%d-h%d-l90-rj", px, px);
            return;
        }
    }

    vm_strlcpy(out, url, out_size);
}
