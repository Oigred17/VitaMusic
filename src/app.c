#include "app.h"
#include "audio.h"
#include "net.h"
#include "player.h"
#include "util.h"

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

vm_app_t g_app;

/* -------------------------------------------------------------------------- */
/* background jobs                                                            */
/* -------------------------------------------------------------------------- */

struct work_queue {
    volatile int search_req;
    char search_query[160];

    volatile int lyrics_req;
    vm_track_t lyrics_track;

    volatile int thumb_req;
    vm_track_t thumb_track;

    volatile int visitor_req;

    volatile int exit;
};

static struct work_queue g_work;
static SceKernelLwMutexWork g_lock;
static SceUID g_worker = -1;

/* Staging buffers: workers fill these and publish them in one go, so the UI
 * never sees a half written result. */
static vm_track_t g_results_staging[VM_MAX_RESULTS];
static vm_lyrics_t g_lyrics_staging;

static char g_toast[160];
static int64_t g_toast_until;
static char g_thumb_path_buf[300];
static int g_error_reported;

static void lock(void);
static void unlock(void);

/* -------------------------------------------------------------------------- */
/* small helpers                                                              */
/* -------------------------------------------------------------------------- */

static void lock(void)
{
    sceKernelLockLwMutex(&g_lock, 1, NULL);
}

static void unlock(void)
{
    sceKernelUnlockLwMutex(&g_lock, 1);
}

static void track_path(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, VM_THUMB_DIR "/%s.jpg", id);
}

/* Same shape as parse_track_line, but for playlist lines: id|title|artist.
 * Fields are pipe-separated because track titles themselves are sanitized
 * (tabs/newlines flattened) but may contain anything else. */
static int parse_pl_track_line(char *line, vm_track_t *track)
{
    char *title;
    char *artist;
    size_t n;

    n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    title = strchr(line, '|');
    if (!title)
        return -1;
    *title++ = '\0';
    artist = strchr(title, '|');
    if (artist)
        *artist++ = '\0';

    memset(track, 0, sizeof(*track));
    vm_strlcpy(track->id, line, sizeof(track->id));
    vm_strlcpy(track->title, title, sizeof(track->title));
    if (artist && artist[0])
        vm_strlcpy(track->artist, artist, sizeof(track->artist));

    return track->id[0] ? 0 : -1;
}

/* TSV fields cannot contain tabs or newlines, so they are flattened on write. */
static void sanitize_field(const char *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }

    for (; *src && i + 1 < cap; src++) {
        char c = *src;

        if (c == '\t' || c == '\n' || c == '\r')
            c = ' ';
        dst[i++] = c;
    }
    dst[i] = '\0';
}

static int split_tabs(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;

    if (max > 0)
        fields[n++] = p;

    for (; *p && n < max; p++) {
        if (*p == '\t') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

static int read_line(SceUID fd, char *buf, size_t cap)
{
    size_t i = 0;
    char c;
    int n;

    for (;;) {
        n = sceIoRead(fd, &c, 1);
        if (n <= 0)
            break;
        if (c == '\n')
            break;
        if (i + 1 < cap)
            buf[i++] = c;
    }
    buf[i] = '\0';
    return (int)i;
}

/* -------------------------------------------------------------------------- */
/* persistence                                                                */
/* -------------------------------------------------------------------------- */

static void write_track_line(SceUID fd, const vm_track_t *track)
{
    char title[VM_TITLE_LEN];
    char artist[VM_ARTIST_LEN];
    char album[VM_ALBUM_LEN];
    char thumb[VM_THUMB_URL_LEN];
    char line[1400];
    int n;

    sanitize_field(track->title, title, sizeof(title));
    sanitize_field(track->artist, artist, sizeof(artist));
    sanitize_field(track->album, album, sizeof(album));
    sanitize_field(track->thumb_url, thumb, sizeof(thumb));

    n = snprintf(line, sizeof(line), "%s\t%s\t%s\t%s\t%d\t%s\n", track->id,
                 title, artist, album, track->duration_s, thumb);
    if (n > 0 && n < (int)sizeof(line))
        sceIoWrite(fd, line, (unsigned)n);
}

static int parse_track_line(char *line, vm_track_t *track)
{
    char *fields[6];
    int n;
    char *last;

    /* A line without a trailing newline reaches us intact; strip CR/LF. */
    last = line + strlen(line);
    while (last > line && (last[-1] == '\r' || last[-1] == '\n'))
        *--last = '\0';

    n = split_tabs(line, fields, 6);
    if (n < 2 || fields[0][0] == '\0')
        return -1;

    memset(track, 0, sizeof(*track));
    vm_strlcpy(track->id, fields[0], sizeof(track->id));
    vm_strlcpy(track->title, fields[1], sizeof(track->title));
    if (n > 2)
        vm_strlcpy(track->artist, fields[2], sizeof(track->artist));
    if (n > 3)
        vm_strlcpy(track->album, fields[3], sizeof(track->album));
    if (n > 4)
        track->duration_s = atoi(fields[4]);
    if (n > 5)
        vm_strlcpy(track->thumb_url, fields[5], sizeof(track->thumb_url));

    return 0;
}

static void load_tracks(const char *path, vm_track_t *out, int *count, int max)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    char line[1400];

    *count = 0;
    if (fd < 0)
        return;

    while (*count < max && read_line(fd, line, sizeof(line)) > 0) {
        if (parse_track_line(line, &out[*count]) == 0)
            (*count)++;
    }
    sceIoClose(fd);
    vm_log("app: loaded %d tracks from %s\n", *count, path);
}

static void save_tracks(const char *path, const vm_track_t *list, int count)
{
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int i;

    if (fd < 0) {
        vm_log("app: cannot write %s\n", path);
        return;
    }
    for (i = 0; i < count; i++)
        write_track_line(fd, &list[i]);
    sceIoClose(fd);
}

static void save_settings(void)
{
    SceUID fd = sceIoOpen(VM_CFG_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    char line[160];
    int n;

    if (fd < 0)
        return;

    n = snprintf(line, sizeof(line), "volume=%d\nrepeat=%d\nshuffle=%d\nui=%d\n",
                 g_app.volume, (int)g_app.repeat_mode, g_app.shuffle,
                 g_app.ui_scale_percent);
    if (n > 0)
        sceIoWrite(fd, line, (unsigned)n);
    sceIoClose(fd);
}

static void load_settings(void)
{
    SceUID fd = sceIoOpen(VM_CFG_FILE, SCE_O_RDONLY, 0);
    char line[160];

    g_app.volume = 70;
    g_app.repeat_mode = REPEAT_OFF;
    g_app.shuffle = 0;
    g_app.ui_scale_percent = 100;

    if (fd < 0)
        return;

    while (read_line(fd, line, sizeof(line)) > 0) {
        char *eq = strchr(line, '=');

        if (!eq)
            continue;
        *eq = '\0';
        if (strcmp(line, "volume") == 0)
            g_app.volume = atoi(eq + 1);
        else if (strcmp(line, "repeat") == 0)
            g_app.repeat_mode = (app_repeat_mode)atoi(eq + 1);
        else if (strcmp(line, "shuffle") == 0)
            g_app.shuffle = atoi(eq + 1) ? 1 : 0;
        else if (strcmp(line, "ui") == 0)
            g_app.ui_scale_percent = atoi(eq + 1);
    }
    sceIoClose(fd);

    if (g_app.volume < 0 || g_app.volume > 100)
        g_app.volume = 70;
}

/* -------------------------------------------------------------------------- */
/* playback plumbing                                                          */
/* -------------------------------------------------------------------------- */

/* Forward declarations for the saved-playlist block below, which loads into
 * the queue before the queue helpers are defined. */
static void queue_from(const vm_track_t *list, int count, int index);
static int start_queue_index(int index);

/* -------------------------------------------------------------------------- */
/* saved playlists                                                            */
/* -------------------------------------------------------------------------- */

static void save_playlists_locked(void)
{
    SceUID fd = sceIoOpen(VM_PLAYLISTS_FILE,
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    int i;

    if (fd < 0) {
        vm_log("app: cannot write %s\n", VM_PLAYLISTS_FILE);
        return;
    }
    for (i = 0; i < g_app.playlist_count; i++) {
        int j;
        char line[VM_TITLE_LEN + VM_ARTIST_LEN + VM_MAX_PLAYLIST_NAME + 48];
        int n;

        n = snprintf(line, sizeof(line), "P|%s|%d\n", g_app.playlist_names[i],
                     g_app.playlist_counts[i]);
        if (n > 0 && n < (int)sizeof(line))
            sceIoWrite(fd, line, (unsigned)n);
        for (j = 0; j < g_app.playlist_counts[i]; j++) {
            const vm_track_t *t = &g_app.playlist_tracks[i][j];

            n = snprintf(line, sizeof(line), "%s|%s|%s\n", t->id, t->title,
                         t->artist);
            if (n > 0 && n < (int)sizeof(line))
                sceIoWrite(fd, line, (unsigned)n);
        }
    }
    sceIoClose(fd);
}

static void load_playlists(void)
{
    SceUID fd = sceIoOpen(VM_PLAYLISTS_FILE, SCE_O_RDONLY, 0);
    char line[1400];
    int cur = -1;

    g_app.playlist_count = 0;
    if (fd < 0)
        return;
    while (read_line(fd, line, sizeof(line)) > 0) {
        if (line[0] == 'P' && line[1] == '|') {
            char *name = line + 2;
            char *cnt = strchr(name, '|');

            if (cur >= VM_MAX_PLAYLISTS - 1)
                break;
            if (!cnt)
                continue;
            *cnt++ = '\0';
            cur = g_app.playlist_count;
            vm_strlcpy(g_app.playlist_names[cur], name,
                       sizeof(g_app.playlist_names[cur]));
            g_app.playlist_counts[cur] = 0;
            g_app.playlist_tracks[cur][0].id[0] = '\0';
            g_app.playlist_count++;
            (void)cnt;
        } else if (cur >= 0 && cur < g_app.playlist_count &&
                   g_app.playlist_counts[cur] < VM_MAX_QUEUE) {
            vm_track_t t;

            if (parse_pl_track_line(line, &t) == 0)
                g_app.playlist_tracks[cur][g_app.playlist_counts[cur]++] = t;
        }
    }
    sceIoClose(fd);
    vm_log("app: loaded %d playlists\n", g_app.playlist_count);
}

int app_playlist_save(const char *name)
{
    int i;

    if (!name || !name[0] || g_app.queue_count <= 0)
        return -1;
    for (i = 0; i < g_app.playlist_count; i++)
        if (strcmp(g_app.playlist_names[i], name) == 0)
            return -2; /* duplicate */
    if (g_app.playlist_count >= VM_MAX_PLAYLISTS)
        return -3; /* full */

    vm_strlcpy(g_app.playlist_names[g_app.playlist_count], name,
               sizeof(g_app.playlist_names[0]));
    memcpy(g_app.playlist_tracks[g_app.playlist_count], g_app.queue,
           sizeof(vm_track_t) * (size_t)g_app.queue_count);
    g_app.playlist_counts[g_app.playlist_count] = g_app.queue_count;
    g_app.playlist_count++;
    save_playlists_locked();
    return 0;
}

int app_playlist_create(const char *name, const vm_track_t *first)
{
    int i;

    if (!name || !name[0])
        return -1;
    for (i = 0; i < g_app.playlist_count; i++)
        if (strcmp(g_app.playlist_names[i], name) == 0)
            return -2; /* duplicate */
    if (g_app.playlist_count >= VM_MAX_PLAYLISTS)
        return -3; /* full */

    vm_strlcpy(g_app.playlist_names[g_app.playlist_count], name,
               sizeof(g_app.playlist_names[0]));
    g_app.playlist_counts[g_app.playlist_count] = 0;
    if (first && first->id[0] && VM_MAX_QUEUE > 0)
        g_app.playlist_tracks[g_app.playlist_count]
            [g_app.playlist_counts[g_app.playlist_count]++] = *first;
    g_app.playlist_count++;
    save_playlists_locked();
    return 0;
}

int app_playlist_add_track(int playlist, const vm_track_t *track)
{
    int i;

    if (playlist < 0 || playlist >= g_app.playlist_count)
        return -1;
    if (!track || !track->id[0])
        return -1;
    for (i = 0; i < g_app.playlist_counts[playlist]; i++)
        if (strcmp(g_app.playlist_tracks[playlist][i].id, track->id) == 0)
            return -2; /* already there */
    if (g_app.playlist_counts[playlist] >= VM_MAX_QUEUE)
        return -3; /* full */

    g_app.playlist_tracks[playlist][g_app.playlist_counts[playlist]++] =
        *track;
    save_playlists_locked();
    return 0;
}

void app_playlist_remove_track(int playlist, int index)
{
    int count;

    if (playlist < 0 || playlist >= g_app.playlist_count)
        return;
    count = g_app.playlist_counts[playlist];
    if (index < 0 || index >= count)
        return;
    if (index + 1 < count)
        memmove(&g_app.playlist_tracks[playlist][index],
                &g_app.playlist_tracks[playlist][index + 1],
                sizeof(vm_track_t) * (size_t)(count - index - 1));
    g_app.playlist_counts[playlist]--;
    save_playlists_locked();
}

void app_playlist_delete(int playlist)
{
    if (playlist < 0 || playlist >= g_app.playlist_count)
        return;
    if (playlist + 1 < g_app.playlist_count) {
        memmove(&g_app.playlist_names[playlist], &g_app.playlist_names[playlist + 1],
                sizeof(g_app.playlist_names[0]) *
                    (size_t)(g_app.playlist_count - playlist - 1));
        memmove(&g_app.playlist_tracks[playlist],
                &g_app.playlist_tracks[playlist + 1],
                sizeof(g_app.playlist_tracks[0]) *
                    (size_t)(g_app.playlist_count - playlist - 1));
        memmove(&g_app.playlist_counts[playlist],
                &g_app.playlist_counts[playlist + 1],
                sizeof(int) * (size_t)(g_app.playlist_count - playlist - 1));
    }
    g_app.playlist_count--;
    save_playlists_locked();
}

int app_play_from_playlist(int playlist, int index)
{
    if (playlist < 0 || playlist >= g_app.playlist_count)
        return -1;
    if (index < 0) {
        queue_from(g_app.playlist_tracks[playlist],
                   g_app.playlist_counts[playlist], 0);
        g_app.queue_index = -1;
        return 0;
    }
    if (index >= g_app.playlist_counts[playlist])
        return -1;
    queue_from(g_app.playlist_tracks[playlist],
               g_app.playlist_counts[playlist], index);
    return start_queue_index(index);
}

/* -------------------------------------------------------------------------- */
/* history / favourites                                                       */
/* -------------------------------------------------------------------------- */

static void history_add(const vm_track_t *track)
{
    int i;

    if (!track || !track->id[0])
        return;

    for (i = 0; i < g_app.history_count; i++) {
        if (strcmp(g_app.history[i].id, track->id) == 0) {
            /* Move to front. */
            vm_track_t moved = g_app.history[i];

            memmove(&g_app.history[1], &g_app.history[0],
                    sizeof(vm_track_t) * (size_t)i);
            g_app.history[0] = moved;
            save_tracks(VM_HIST_FILE, g_app.history, g_app.history_count);
            return;
        }
    }

    if (g_app.history_count < VM_MAX_HISTORY)
        g_app.history_count++;
    memmove(&g_app.history[1], &g_app.history[0],
            sizeof(vm_track_t) * (size_t)(g_app.history_count - 1));
    g_app.history[0] = *track;
    save_tracks(VM_HIST_FILE, g_app.history, g_app.history_count);
}

int app_is_favorite(const char *video_id)
{
    int i;

    for (i = 0; i < g_app.favorite_count; i++) {
        if (strcmp(g_app.favorites[i].id, video_id) == 0)
            return 1;
    }
    return 0;
}

void app_toggle_favorite(const vm_track_t *track)
{
    int i;

    if (!track || !track->id[0])
        return;

    for (i = 0; i < g_app.favorite_count; i++) {
        if (strcmp(g_app.favorites[i].id, track->id) == 0) {
            memmove(&g_app.favorites[i], &g_app.favorites[i + 1],
                    sizeof(vm_track_t) *
                        (size_t)(g_app.favorite_count - i - 1));
            g_app.favorite_count--;
            save_tracks(VM_FAV_FILE, g_app.favorites, g_app.favorite_count);
            app_toast("Quitado de favoritos");
            return;
        }
    }

    if (g_app.favorite_count >= VM_MAX_LIBRARY) {
        app_toast("Favoritos llenos");
        return;
    }

    memmove(&g_app.favorites[1], &g_app.favorites[0],
            sizeof(vm_track_t) * (size_t)g_app.favorite_count);
    g_app.favorites[0] = *track;
    g_app.favorite_count++;
    save_tracks(VM_FAV_FILE, g_app.favorites, g_app.favorite_count);
    app_toast("A\xc3\xb1" "adido a favoritos");
}

int app_is_cached(const char *video_id)
{
    char path[300];

    if (!video_id || !video_id[0])
        return 0;
    snprintf(path, sizeof(path), VM_CACHE_DIR "/%s.m4a", video_id);
    return vm_file_exists(path) && vm_file_size(path) >= VM_STREAM_MIN_BYTES;
}

void app_download_offline(const vm_track_t *track)
{
    if (!track || !track->id[0])
        return;
    if (app_is_cached(track->id)) {
        app_toast("Ya est\xc3\xa1 descargada");
        return;
    }
    pl_prefetch(track);
    app_toast("Descargando para offline...");
}

int app_account_logged_in(void)
{
    return g_app.account_logged_in && g_app.account_name[0];
}

const char *app_account_name(void)
{
    return g_app.account_name;
}

void app_account_login(const char *name)
{
    SceUID fd;

    if (!name || !name[0]) {
        app_toast("Escribe un nombre");
        return;
    }
    vm_strlcpy(g_app.account_name, name, sizeof(g_app.account_name));
    vm_trim(g_app.account_name);
    if (!g_app.account_name[0]) {
        app_toast("Escribe un nombre");
        return;
    }
    g_app.account_logged_in = 1;
    fd = sceIoOpen(VM_ACCOUNT_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                   0666);
    if (fd >= 0) {
        sceIoWrite(fd, g_app.account_name, strlen(g_app.account_name));
        sceIoClose(fd);
    }
    app_toast("Sesi\xc3\xb3n iniciada");
}

void app_account_logout(void)
{
    g_app.account_name[0] = '\0';
    g_app.account_logged_in = 0;
    sceIoRemove(VM_ACCOUNT_FILE);
    app_toast("Sesi\xc3\xb3n cerrada");
}

/* -------------------------------------------------------------------------- */
/* worker thread                                                              */
/* -------------------------------------------------------------------------- */

static void run_search_job(const char *query)
{
    int count = 0;
    int rc = it_search(query, g_results_staging, VM_MAX_RESULTS, &count);

    lock();
    if (rc == 0) {
        memcpy(g_app.results, g_results_staging,
               sizeof(vm_track_t) * (size_t)count);
        g_app.result_count = count;
        g_app.search_state = JOB_DONE;
        g_app.search_error[0] = '\0';
    } else {
        g_app.result_count = 0;
        g_app.search_state = JOB_FAILED;
        vm_strlcpy(g_app.search_error, vm_net_last_error(),
                     sizeof(g_app.search_error));
    }
    unlock();
}

static void run_thumb_job(const vm_track_t *track)
{
    char url[VM_THUMB_URL_LEN];
    char path[300];
    char tmp[320];
    int ok = 0;

    track_path(path, sizeof(path), track->id);
    vm_mkdir_p(VM_THUMB_DIR);

    if (vm_file_exists(path)) {
        ok = 1;
    } else if (track->thumb_url[0]) {
        it_thumbnail_url(track->thumb_url, VM_THUMB_PX, url, sizeof(url));
        /* Download to a temp name and publish only once the file is complete,
         * so the UI never sees a half-written JPEG and reports it as undecodable. */
        snprintf(tmp, sizeof(tmp), "%s.part", path);
        if (vm_net_download(url, tmp, NULL, 0, NULL, NULL) == 0) {
            if (vm_copy_file(tmp, path))
                ok = 1;
            sceIoRemove(tmp);
        } else {
            sceIoRemove(tmp);
        }
    }

    lock();
    if (ok) {
        g_app.thumb_state = JOB_DONE;
    } else {
        g_app.thumb_state = JOB_FAILED;
        vm_log("app: artwork for %s failed: %s\n", track->id,
                 vm_net_last_error());
    }
    unlock();
}

static void run_lyrics_job(const vm_track_t *track)
{
    int rc = lyrics_fetch(track, &g_lyrics_staging);

    lock();
    if (rc == 0) {
        memcpy(&g_app.lyrics, &g_lyrics_staging, sizeof(g_app.lyrics));
        g_app.lyrics_state = JOB_DONE;
    } else {
        memset(&g_app.lyrics, 0, sizeof(g_app.lyrics));
        g_app.lyrics_state = JOB_FAILED;
    }
    g_app.lyrics_scroll = 0;
    unlock();
}

static int worker_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    while (!g_work.exit) {
        char query[160];
        vm_track_t track;
        int kind = 0;

        lock();
        if (g_work.search_req) {
            g_work.search_req = 0;
            vm_strlcpy(query, g_work.search_query, sizeof(query));
            kind = 1;
        } else if (g_work.thumb_req) {
            g_work.thumb_req = 0;
            track = g_work.thumb_track;
            kind = 2;
        } else if (g_work.lyrics_req) {
            g_work.lyrics_req = 0;
            track = g_work.lyrics_track;
            kind = 3;
        } else if (g_work.visitor_req) {
            g_work.visitor_req = 0;
            kind = 4;
        }
        unlock();

        if (kind == 0) {
            sceKernelDelayThread(20000);
            continue;
        }

        if (kind == 1)
            run_search_job(query);
        else if (kind == 2)
            run_thumb_job(&track);
        else if (kind == 3)
            run_lyrics_job(&track);
        else if (kind == 4) {
            /* Fetches the visitor id on the worker so a slow or dead network
             * can never hold the UI back at startup. */
            it_refresh_visitor();
            vm_log("app: net ready, verify %s\n",
                     vm_net_verify_enabled() ? "on" : "off");
        }
    }

    return 0;
}

static void start_worker(void)
{
    g_worker = sceKernelCreateThread("vm_worker", worker_main, 0x10000100,
                                     0x20000, 0,
                                     SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                                     NULL);
    if (g_worker < 0) {
        vm_log("app: worker thread failed 0x%08X\n", (unsigned int)g_worker);
        return;
    }
    sceKernelStartThread(g_worker, 0, NULL);
}

/* -------------------------------------------------------------------------- */
/* playback plumbing                                                          */
/* -------------------------------------------------------------------------- */

/* Index of the track the queue will land on next, or -1 when the queue would
 * end (no repeat-all). Mirrors the selection in app_play_next(). */
static int next_queue_index(void)
{
    int count = g_app.queue_count;
    int index = g_app.queue_index;
    int next;

    if (count <= 0 || index < 0 || index >= count)
        return -1;

    if (g_app.shuffle && count > 1) {
        do {
            next = rand() % count;
        } while (next == index);
        return next;
    }

    next = index + 1;
    if (next >= count) {
        if (g_app.repeat_mode == REPEAT_ALL)
            next = 0;
        else
            return -1;
    }
    return next;
}

/* Warms the cache for the next queued track once the current one is fully
 * on disk, so the download of the song that is playing is never starved. */
static char g_prefetch_id[VM_ID_LEN];

static void prefetch_next(void)
{
    int next = next_queue_index();

    if (next < 0 || next == g_app.queue_index) {
        g_prefetch_id[0] = '\0';
        pl_prefetch(NULL);
        return;
    }
    if (strcmp(g_prefetch_id, g_app.queue[next].id) == 0)
        return;
    vm_strlcpy(g_prefetch_id, g_app.queue[next].id, sizeof(g_prefetch_id));
    pl_prefetch(&g_app.queue[next]);
}

static void maybe_prefetch_next(void)
{
    if (g_app.queue_count <= 0 || g_app.queue_index < 0 ||
        g_app.queue_index >= g_app.queue_count)
        return;
    /* Only steal bandwidth once the current song's transfer has finished and
     * the decoded backlog of what is playing has been consumed, so prefetch
     * traffic can never cause a stutter in the live stream. */
    if (pl_buffered_ms() > 500)
        return;
    prefetch_next();
}

static void queue_from(const vm_track_t *list, int count, int index)
{
    if (count > VM_MAX_QUEUE)
        count = VM_MAX_QUEUE;
    if (count <= 0)
        return;

    memcpy(g_app.queue, list, sizeof(vm_track_t) * (size_t)count);
    g_app.queue_count = count;
    g_app.queue_index = index;
}

static int start_queue_index(int index)
{
    if (index < 0 || index >= g_app.queue_count)
        return -1;

    g_app.queue_index = index;
    g_error_reported = 0;
    g_prefetch_id[0] = '\0';
    pl_play(&g_app.queue[index]);
    history_add(&g_app.queue[index]);

    lock();
    g_work.lyrics_track = g_app.queue[index];
    g_work.lyrics_req = 1;
    g_app.lyrics_state = JOB_RUNNING;
    unlock();

    /* Prefetch the successor only when this track is already fully cached;
     * otherwise app_tick starts it once the download finishes. */
    maybe_prefetch_next();

    return 0;
}

int app_play_from_results(int index)
{
    if (index < 0 || index >= g_app.result_count)
        return -1;
    queue_from(g_app.results, g_app.result_count, index);
    return start_queue_index(index);
}

int app_play_from_favorites(int index)
{
    if (index < 0 || index >= g_app.favorite_count)
        return -1;
    queue_from(g_app.favorites, g_app.favorite_count, index);
    return start_queue_index(index);
}

int app_play_from_history(int index)
{
    if (index < 0 || index >= g_app.history_count)
        return -1;
    queue_from(g_app.history, g_app.history_count, index);
    return start_queue_index(index);
}

int app_play_from_queue(int index)
{
    return start_queue_index(index);
}

void app_play_next(void)
{
    int next;

    if (g_app.queue_count == 0)
        return;

    if (g_app.shuffle && g_app.queue_count > 1) {
        int index = g_app.queue_index;

        while (index == g_app.queue_index)
            index = rand() % g_app.queue_count;
        start_queue_index(index);
        return;
    }

    next = g_app.queue_index + 1;
    if (next >= g_app.queue_count) {
        if (g_app.repeat_mode == REPEAT_ALL)
            next = 0;
        else {
            app_stop();
            return;
        }
    }
    start_queue_index(next);
}

void app_play_prev(void)
{
    if (g_app.queue_count == 0)
        return;

    if (pl_position_ms() > 3000) {
        app_seek_ms(0);
        return;
    }

    if (g_app.queue_index > 0)
        start_queue_index(g_app.queue_index - 1);
    else
        app_seek_ms(0);
}

void app_toggle_pause(void)
{
    pl_toggle_pause();
}

void app_stop(void)
{
    pl_stop();
    pl_prefetch(NULL);
}

void app_seek_ms(uint32_t ms)
{
    pl_seek_ms(ms);
}

void app_seek_relative(int seconds)
{
    int64_t target = (int64_t)pl_position_ms() + (int64_t)seconds * 1000;

    if (target < 0)
        target = 0;
    if (g_app.queue_count && g_app.queue_index < g_app.queue_count) {
        int duration = g_app.queue[g_app.queue_index].duration_s;

        if (duration > 0 && target > (int64_t)duration * 1000 - 1000)
            target = (int64_t)duration * 1000 - 1000;
    }
    if (target < 0)
        target = 0;
    pl_seek_ms((uint32_t)target);
}

/* -------------------------------------------------------------------------- */
/* public API                                                                 */
/* -------------------------------------------------------------------------- */

void app_init(void)
{
    memset(&g_app, 0, sizeof(g_app));
    memset(&g_work, 0, sizeof(g_work));

    if (sceKernelCreateLwMutex(&g_lock, "vm_state_lock", 2, 0, NULL) < 0)
        vm_log("app: state lock failed\n");

    vm_mkdir_p(VM_DATA_DIR);
    vm_mkdir_p(VM_CACHE_DIR);
    vm_mkdir_p(VM_THUMB_DIR);

    load_settings();
    load_tracks(VM_FAV_FILE, g_app.favorites, &g_app.favorite_count,
                VM_MAX_LIBRARY);    load_tracks(VM_HIST_FILE, g_app.history, &g_app.history_count,
                VM_MAX_HISTORY);
    load_playlists();

    g_app.account_name[0] = '\0';
    g_app.account_logged_in = 0;
    if (vm_file_exists(VM_ACCOUNT_FILE)) {
        SceUID fd = sceIoOpen(VM_ACCOUNT_FILE, SCE_O_RDONLY, 0);

        if (fd >= 0) {
            int n = sceIoRead(fd, g_app.account_name,
                              sizeof(g_app.account_name) - 1);

            sceIoClose(fd);
            if (n > 0) {
                g_app.account_name[n] = '\0';
                vm_trim(g_app.account_name);
                if (g_app.account_name[0])
                    g_app.account_logged_in = 1;
            }
        }
    }

    g_app.search_state = JOB_IDLE;
    g_app.lyrics_state = JOB_FAILED;
    g_app.thumb_state = JOB_IDLE;

    /* Only the cached visitor id is loaded here: fetching a fresh one (and the
     * whole network stack) runs on the worker thread so a slow or dead network
     * can never block the UI from starting. */
    it_init();

    vm_audio_set_volume(g_app.volume);
    srand((unsigned int)(sceKernelGetSystemTimeWide() & 0xFFFFFFFF));

    pl_init();
    start_worker();

    lock();
    g_work.visitor_req = 1;
    unlock();

    vm_log("app: ready (volume %d)\n", g_app.volume);
}

void app_shutdown(void)
{
    g_work.exit = 1;
    if (g_worker >= 0) {
        sceKernelWaitThreadEnd(g_worker, NULL, NULL);
        sceKernelDeleteThread(g_worker);
        g_worker = -1;
    }
    pl_shutdown();
    save_settings();
    vm_log_close();
}

void app_start_search(const char *query)
{
    if (!query || !*query) {
        app_toast("Escribe algo para buscar");
        return;
    }

    lock();
    vm_strlcpy(g_app.query, query, sizeof(g_app.query));
    vm_strlcpy(g_work.search_query, query, sizeof(g_work.search_query));
    g_work.search_req = 1;
    g_app.search_state = JOB_RUNNING;
    g_app.result_count = 0;
    unlock();
}

void app_search_consume(void)
{
    lock();
    if (g_app.search_state == JOB_DONE || g_app.search_state == JOB_FAILED)
        g_app.search_state = JOB_IDLE;
    unlock();
}

void app_request_lyrics(void)
{
    vm_track_t track;

    memset(&track, 0, sizeof(track));
    if (!pl_current_track(&track)) {
        if (g_app.queue_count > 0 && g_app.queue_index < g_app.queue_count)
            track = g_app.queue[g_app.queue_index];
    }
    if (!track.id[0]) {
        app_toast("Nada en reproducci\xc3\xb3n");
        return;
    }

    lock();
    g_work.lyrics_track = track;
    g_work.lyrics_req = 1;
    g_app.lyrics_state = JOB_RUNNING;
    g_app.lyrics_scroll = 0;
    unlock();
}

void app_lyrics_reset(void)
{
    lock();
    g_work.lyrics_req = 0;
    memset(&g_app.lyrics, 0, sizeof(g_app.lyrics));
    g_app.lyrics_state = JOB_FAILED;
    g_app.lyrics_scroll = 0;
    unlock();
}

const char *app_thumb_file(const vm_track_t *track)
{
    if (!track || !track->id[0] || !track->thumb_url[0])
        return NULL;

    track_path(g_thumb_path_buf, sizeof(g_thumb_path_buf), track->id);
    if (vm_file_exists(g_thumb_path_buf))
        return g_thumb_path_buf;

    lock();
    /* Queue the download unless this exact id is already in flight or has
     * already failed, so a dead URL is not retried every frame. */
    if (strcmp(g_app.thumb_id, track->id) != 0) {
        vm_strlcpy(g_app.thumb_id, track->id, sizeof(g_app.thumb_id));
        g_work.thumb_track = *track;
        g_work.thumb_req = 1;
        g_app.thumb_state = JOB_RUNNING;
    }
    unlock();

    return NULL;
}

void app_thumb_invalidate(const vm_track_t *track)
{
    char path[300];

    if (!track || !track->id[0])
        return;

    track_path(path, sizeof(path), track->id);
    if (vm_file_exists(path))
        sceIoRemove(path);

    lock();
    /* Clear the in-flight marker so the worker queues a fresh download. */
    if (strcmp(g_app.thumb_id, track->id) == 0)
        g_app.thumb_id[0] = '\0';
    unlock();
}

void app_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    g_app.volume = percent;
    vm_audio_set_volume(percent);
    save_settings();
}

int app_volume(void)
{
    return g_app.volume;
}

void app_cycle_repeat(void)
{
    g_app.repeat_mode = (app_repeat_mode)(((int)g_app.repeat_mode + 1) % 3);
    save_settings();

    switch (g_app.repeat_mode) {
    case REPEAT_ALL:
        app_toast("Repetir: todas");
        break;
    case REPEAT_ONE:
        app_toast("Repetir: una");
        break;
    default:
        app_toast("Repetir: off");
        break;
    }
}

void app_toggle_shuffle(void)
{
    g_app.shuffle = !g_app.shuffle;
    save_settings();
    app_toast("%s", g_app.shuffle ? "Aleatorio: on" : "Aleatorio: off");
}

void app_clear_cache(void)
{
    SceUID dir;
    SceIoDirent entry;
    char path[300];
    int removed = 0;

    /* Streams are cached flat, artwork lives one level down. */
    dir = sceIoDopen(VM_CACHE_DIR);
    if (dir >= 0) {
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0) {
            if (entry.d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), VM_CACHE_DIR "/%s", entry.d_name);
            /* Directories are not expected here; sceIoRemove simply refuses
             * to touch one, so no type check is needed. */
            if (sceIoRemove(path) == 0)
                removed++;
        }
        sceIoDclose(dir);
    }

    dir = sceIoDopen(VM_THUMB_DIR);
    if (dir >= 0) {
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0) {
            if (entry.d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), VM_THUMB_DIR "/%s", entry.d_name);
            if (sceIoRemove(path) == 0)
                removed++;
        }
        sceIoDclose(dir);
    }

    app_toast("Cach\xc3\xa9 limpiada (%d archivos)", removed);
    vm_log("app: cache cleared, %d files removed\n", removed);
}

void app_tick(void)
{
    pl_state state = pl_get_state();

    if (state == PL_ENDED) {
        if (g_app.queue_count == 0)
            return;
        if (g_app.repeat_mode == REPEAT_ONE)
            start_queue_index(g_app.queue_index);
        else
            app_play_next();
        return;
    }

    if (state == PL_ERROR && !g_error_reported) {
        g_error_reported = 1;
        app_toast("%s", pl_message());
    }

    if (state == PL_PLAYING || state == PL_PAUSED)
        maybe_prefetch_next();
}

void app_toast(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(g_toast, sizeof(g_toast), fmt, args);
    va_end(args);

    g_toast_until = sceKernelGetSystemTimeWide() + 3000000; /* three seconds */
    vm_log("app: %s\n", g_toast);
}

const char *app_toast_text(void)
{
    if (!g_toast[0])
        return NULL;
    if (sceKernelGetSystemTimeWide() >= g_toast_until) {
        g_toast[0] = '\0';
        return NULL;
    }
    return g_toast;
}

int app_toast_active(void)
{
    return app_toast_text() != NULL;
}
