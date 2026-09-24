#include "lyrics.h"
#include "net.h"
#include "util.h"

#include "json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* -------------------------------------------------------------------------- */
/* JSON helpers (type checked, see the note in innertube.c)                    */
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

    if (json_is_integer(v))
        return (int)json_integer_value(v);
    return fallback;
}

static int has_text(const char *s)
{
    return s && *s;
}

/* -------------------------------------------------------------------------- */
/* title / artist cleanup                                                     */
/* -------------------------------------------------------------------------- */

/* Keywords that mark a bracketed chunk as noise rather than part of a title. */
static int is_noise_chunk(const char *chunk)
{
    static const char *keywords[] = {
        "official",  "video",   "audio",     "lyrics", "lyric",
        "visualizer", "hd",     "hq",        "4k",     "remaster",
        "remix",     "live",    "acoustic",  "version", "edit",
        "extended",  "radio",   "clean",     "explicit", "color coded",
        "sped up",   "slowed"
    };
    size_t i;

    for (i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (vm_strcasestr(chunk, keywords[i]))
            return 1;
    }
    return 0;
}

/* Removes "(Official Video)" style chunks and "feat." credits. */
static void clean_title(const char *title, char *out, size_t cap)
{
    size_t used = 0;
    const char *p = title ? title : "";

    out[0] = '\0';

    while (*p && used + 1 < cap) {
        if (*p == '(' || *p == '[') {
            char close = (*p == '(') ? ')' : ']';
            const char *end = strchr(p + 1, close);

            if (end) {
                char chunk[160];
                size_t len = (size_t)(end - p - 1);

                if (len >= sizeof(chunk))
                    len = sizeof(chunk) - 1;
                memcpy(chunk, p + 1, len);
                chunk[len] = '\0';

                if (is_noise_chunk(chunk)) {
                    p = end + 1;
                    continue;
                }
            }
        }
        /* CJK lenticular brackets that never carry meaning for lrclib. */
        if ((unsigned char)p[0] == 0xE3 && (unsigned char)p[1] == 0x80 &&
            (unsigned char)p[2] == 0x90) {
            const char *end = strstr(p + 3, "\xE3\x80\x91");

            if (end) {
                p = end + 3;
                continue;
            }
        }
        if (strncasecmp(p, " feat.", 6) == 0 || strncasecmp(p, " ft.", 4) == 0)
            break;

        out[used++] = *p++;
    }
    out[used] = '\0';

    /* A trailing "| something" is almost always a channel credit. */
    {
        char *bar = strchr(out, '|');

        if (bar && bar != out)
            *bar = '\0';
    }

    {
        char *trimmed = vm_trim(out);

        if (trimmed != out)
            memmove(out, trimmed, strlen(trimmed) + 1);
    }
}

/* Keeps only the first credited artist. */
static void clean_artist(const char *artist, char *out, size_t cap)
{
    static const char *separators[] = {
        " & ",  " and ", ", ", " x ",       " X ",  " feat. ",
        " feat ", " ft. ", " ft ", " featuring ", " with ", " / "
    };
    size_t i;
    size_t best = 0;

    vm_strlcpy(out, artist ? artist : "", cap);

    for (i = 0; i < sizeof(separators) / sizeof(separators[0]); i++) {
        const char *found = vm_strcasestr(out, separators[i]);

        if (found) {
            size_t index = (size_t)(found - out);

            if (best == 0 || index < best)
                best = index;
        }
    }

    if (best > 0)
        out[best] = '\0';

    {
        char *trimmed = vm_trim(out);

        if (trimmed != out)
            memmove(out, trimmed, strlen(trimmed) + 1);
    }
}

/* -------------------------------------------------------------------------- */
/* candidate scoring                                                          */
/* -------------------------------------------------------------------------- */

/* 0..100 similarity, close enough to the ratio the Android client uses. */
static int similarity(const char *a, const char *b)
{
    static int prev[129];
    static int cur[129];
    size_t la, lb, i, j, max_len;

    if (!a || !b)
        return 0;
    la = strlen(a);
    lb = strlen(b);
    if (la == 0 || lb == 0)
        return 0;
    /* Keep the DP bounded; these are song titles, not paragraphs. */
    if (la > 128)
        la = 128;
    if (lb > 128)
        lb = 128;

    if (la == lb && strncasecmp(a, b, la) == 0)
        return 100;

    for (j = 0; j <= lb; j++)
        prev[j] = (int)j;

    for (i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (j = 1; j <= lb; j++) {
            int cost = (tolower((unsigned char)a[i - 1]) ==
                        tolower((unsigned char)b[j - 1]))
                           ? 0
                           : 1;
            int del = prev[j] + 1;
            int ins = cur[j - 1] + 1;
            int sub = prev[j - 1] + cost;
            int best = del < ins ? del : ins;

            cur[j] = best < sub ? best : sub;
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }

    max_len = la > lb ? la : lb;
    return (int)(100 - (prev[lb] * 100 / (int)max_len));
}

/*
 * Scores one lrclib result. Synced lyrics are worth more than plain ones, a
 * duration within a few seconds is worth more still, and the title and artist
 * text decide the rest.
 */
static int score_item(json_t *item, int wanted_duration, const char *title,
                      const char *artist, int *out_synced)
{
    const char *synced = obj_str(item, "syncedLyrics");
    const char *plain = obj_str(item, "plainLyrics");
    int duration = obj_int(item, "duration", 0);
    int score;

    *out_synced = has_text(synced);

    if (!has_text(synced) && !has_text(plain))
        return -1;

    score = *out_synced ? 400 : 0;

    if (wanted_duration > 0 && duration > 0) {
        int diff = duration - wanted_duration;

        if (diff < 0)
            diff = -diff;
        score += diff <= 5 ? 400 - diff * 10 : 100 - diff;
    } else {
        score += 200;
    }

    if (title) {
        const char *candidate = obj_str(item, "trackName");

        if (candidate)
            score += similarity(title, candidate) / 4;
    }
    if (artist) {
        const char *candidate = obj_str(item, "artistName");

        if (candidate)
            score += similarity(artist, candidate) / 8;
    }

    return score;
}

/* -------------------------------------------------------------------------- */
/* LRC parsing                                                                */
/* -------------------------------------------------------------------------- */

static void append_line(vm_lyrics_t *out, uint32_t time_ms, const char *text)
{
    vm_lyric_line_t *line;
    size_t used = 0;
    size_t len;
    int i;

    if (out->count >= VM_MAX_LYRIC_LINES)
        return;

    while (*text == ' ' || *text == '\t')
        text++;

    len = strlen(text);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == ' ' ||
                       text[len - 1] == '\t'))
        len--;
    if (len == 0)
        return;
    if (len > 0xFFFF)
        len = 0xFFFF;

    /* Lines are stored in the order they are appended, so the blob cursor is
     * not tracked; the highest end offset is looked up instead. */
    for (i = 0; i < out->count; i++) {
        size_t end = (size_t)out->lines[i].offset + out->lines[i].length;

        if (end > used)
            used = end;
    }
    if (used + len + 1 > sizeof(out->text))
        return;

    memcpy(out->text + used, text, len);
    out->text[used + len] = '\0';

    line = &out->lines[out->count++];
    line->time_ms = time_ms;
    line->offset = (uint16_t)used;
    line->length = (uint16_t)len;
}

/* "[mm:ss.xx]" or "[mm:ss]" -> milliseconds, or -1 when it is metadata. */
static int parse_timestamp(const char *tag, size_t len)
{
    size_t i = 0;
    int minutes = 0;
    int seconds = 0;
    int millis = 0;
    int digits;

    if (len == 0 || !isdigit((unsigned char)tag[0]))
        return -1; /* named tags such as [ar:...] or [offset:...] */

    digits = 0;
    while (i < len && isdigit((unsigned char)tag[i]) && digits < 4) {
        minutes = minutes * 10 + (tag[i] - '0');
        i++;
        digits++;
    }
    if (i >= len || tag[i] != ':')
        return -1;
    i++;

    digits = 0;
    while (i < len && isdigit((unsigned char)tag[i]) && digits < 2) {
        seconds = seconds * 10 + (tag[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0)
        return -1;

    if (i < len && (tag[i] == '.' || tag[i] == ':')) {
        int scale = 100;

        i++;
        digits = 0;
        while (i < len && isdigit((unsigned char)tag[i]) && digits < 3) {
            millis += (tag[i] - '0') * scale;
            scale /= 10;
            i++;
            digits++;
        }
    }

    return minutes * 60000 + seconds * 1000 + millis;
}

static void parse_lyrics_body(vm_lyrics_t *out, const char *text)
{
    const char *p = text;
    int plain_mode = 1;

    /* [offset:...] is metadata and is deliberately not applied: its sign
     * convention is not consistent across the files lrclib serves. */
    {
        const char *probe = text;

        while (*probe) {
            if (*probe == '[') {
                const char *close = strchr(probe, ']');

                if (close &&
                    parse_timestamp(probe + 1,
                                    (size_t)(close - probe - 1)) >= 0) {
                    plain_mode = 0;
                    break;
                }
            }
            probe++;
        }
    }

    if (plain_mode) {
        while (*p) {
            char line[1024];
            size_t len = 0;

            while (*p && *p != '\n' && len + 1 < sizeof(line))
                line[len++] = *p++;
            line[len] = '\0';
            while (*p && *p != '\n')
                p++;
            if (*p == '\n')
                p++;

            append_line(out, 0, line);
        }
        return;
    }

    while (*p) {
        char line[1024];
        uint32_t times[16];
        int time_count = 0;
        const char *cursor;
        size_t len = 0;

        while (*p && *p != '\n' && len + 1 < sizeof(line))
            line[len++] = *p++;
        line[len] = '\0';
        while (*p && *p != '\n')
            p++;
        if (*p == '\n')
            p++;

        /* One line may carry several timestamps for the same text. */
        cursor = line;
        while (*cursor == '[') {
            const char *close = strchr(cursor, ']');
            int ms;

            if (!close)
                break;
            ms = parse_timestamp(cursor + 1, (size_t)(close - cursor - 1));
            cursor = close + 1;
            if (ms < 0)
                continue; /* metadata tag */
            if (time_count < 16)
                times[time_count++] = (uint32_t)ms;
        }

        if (time_count == 0)
            continue; /* metadata-only line, or a credit */

        {
            int i;

            for (i = 0; i < time_count; i++)
                append_line(out, times[i], cursor);
        }
    }

    /* Timed files are not required to be in order. */
    {
        int i, j;

        for (i = 1; i < out->count; i++) {
            vm_lyric_line_t key = out->lines[i];

            for (j = i - 1; j >= 0 && out->lines[j].time_ms > key.time_ms; j--)
                out->lines[j + 1] = out->lines[j];
            out->lines[j + 1] = key;
        }
    }

    out->synced = 1;
}

/* -------------------------------------------------------------------------- */
/* lookup                                                                     */
/* -------------------------------------------------------------------------- */

static int query_api(const char *track_name, const char *artist_name,
                     const char *album_name, const char *query,
                     vm_str_t *response)
{
    vm_http_header_t headers[2];
    vm_str_t url;
    int ret;

    vm_str_init(&url);
    vm_str_append(&url, "https://lrclib.net/api/search?");

    if (query) {
        vm_str_append(&url, "q=");
        vm_net_url_encode(&url, query);
    } else {
        int first = 1;

        if (track_name) {
            vm_str_append(&url, "track_name=");
            vm_net_url_encode(&url, track_name);
            first = 0;
        }
        if (artist_name) {
            if (!first)
                vm_str_append(&url, "&");
            vm_str_append(&url, "artist_name=");
            vm_net_url_encode(&url, artist_name);
            first = 0;
        }
        if (album_name && *album_name) {
            if (!first)
                vm_str_append(&url, "&");
            vm_str_append(&url, "album_name=");
            vm_net_url_encode(&url, album_name);
        }
    }

    headers[0] = (vm_http_header_t){ "User-Agent", VM_USER_AGENT };
    headers[1] = (vm_http_header_t){ "Accept", "application/json" };

    vm_str_init(response);
    ret = vm_net_get(url.data, headers, 2, response, NULL);
    vm_str_free(&url);

    if (ret != 0) {
        vm_log("lyrics: query failed: %s\n", vm_net_last_error());
        vm_str_free(response);
        return -1;
    }
    return 0;
}

/* Runs one rung of the strategy ladder. Returns 0 when a reply was received. */
static int run_strategy(int step, const char *title, const char *artist,
                        const char *album, const char *raw_title,
                        const char *raw_artist, vm_str_t *response)
{
    vm_str_t combined;
    int ret;

    switch (step) {
    case 0:
        return query_api(title, artist, album, NULL, response);
    case 1:
        return query_api(title, NULL, NULL, NULL, response);
    case 2:
        vm_str_init(&combined);
        vm_str_append(&combined, artist);
        vm_str_append(&combined, " ");
        vm_str_append(&combined, title);
        ret = query_api(NULL, NULL, NULL, combined.data, response);
        vm_str_free(&combined);
        return ret;
    case 3:
        return query_api(NULL, NULL, NULL, title, response);
    default:
        /* The cleaned title may have thrown away the only thing lrclib
         * matches on, so the raw pair gets one last try. */
        if (!raw_title || (strcmp(raw_title, title) == 0 &&
                           (!raw_artist || strcmp(raw_artist, artist) == 0)))
            return -1;
        return query_api(raw_title, raw_artist, NULL, NULL, response);
    }
}

/*
 * Mirrors the ladder used by the Android client: ask precisely first, then
 * relax until something comes back. The returned text is a malloc'd copy that
 * the caller owns, because the JSON tree it came from does not outlive the
 * request.
 */
static char *find_best_lyrics(const char *title, const char *artist,
                              const char *album, int duration,
                              const char *raw_title, const char *raw_artist,
                              int *out_synced)
{
    int step;

    *out_synced = 0;

    for (step = 0; step < 5; step++) {
        vm_str_t response;
        json_t *root;
        json_error_t err;
        size_t n, k;
        int best_score = -1;
        char *best_copy = NULL;
        int best_synced = 0;

        if (run_strategy(step, title, artist, album, raw_title, raw_artist,
                         &response) != 0)
            continue;

        root = json_loads(response.data ? response.data : "", 0, &err);
        vm_str_free(&response);
        if (!root) {
            vm_log("lyrics: reply is not JSON: %s\n", err.text);
            continue;
        }

        n = arr_size(root);
        for (k = 0; k < n; k++) {
            json_t *item = arr(root, k);
            int synced = 0;
            int score = score_item(item, duration, title, artist, &synced);
            const char *synced_text;
            const char *plain_text;
            const char *text;

            if (score <= best_score)
                continue;

            synced_text = obj_str(item, "syncedLyrics");
            plain_text = obj_str(item, "plainLyrics");
            text = has_text(synced_text) ? synced_text : plain_text;
            if (!has_text(text))
                continue;

            free(best_copy);
            best_copy = vm_strdup(text);
            if (!best_copy)
                break;
            best_score = score;
            best_synced = synced;
        }

        json_decref(root);

        if (best_copy) {
            *out_synced = best_synced;
            vm_log("lyrics: matched on strategy %d\n", step);
            return best_copy;
        }
    }

    return NULL;
}

int lyrics_fetch(const vm_track_t *track, vm_lyrics_t *out)
{
    char clean_t[VM_TITLE_LEN];
    char clean_a[VM_ARTIST_LEN];
    char *text;
    int synced = 0;

    if (!track || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    if (vm_net_init() != 0)
        return -1;

    clean_title(track->title, clean_t, sizeof(clean_t));
    clean_artist(track->artist, clean_a, sizeof(clean_a));
    if (!clean_t[0])
        vm_strlcpy(clean_t, track->title, sizeof(clean_t));

    text = find_best_lyrics(clean_t, clean_a, track->album, track->duration_s,
                            track->title, track->artist, &synced);
    if (!text)
        return -1;

    parse_lyrics_body(out, text);
    free(text);

    if (out->count == 0)
        return -1;

    if (synced)
        out->synced = 1;
    out->valid = 1;

    vm_log("lyrics: %d lines for \"%s\" (%s)\n", out->count, track->title,
             out->synced ? "synced" : "plain");
    return 0;
}

int lyrics_line_at(const vm_lyrics_t *lyrics, uint32_t position_ms)
{
    int low = 0;
    int high;
    int best = -1;

    if (!lyrics || lyrics->count == 0 || !lyrics->synced)
        return -1;

    high = lyrics->count - 1;
    while (low <= high) {
        int mid = (low + high) / 2;

        if (lyrics->lines[mid].time_ms <= position_ms) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return best;
}
