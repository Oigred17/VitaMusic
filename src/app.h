/*
 * VitaMusic - application state.
 *
 * The UI thread never talks to the network. A single worker thread runs one
 * background job at a time (search, artwork, lyrics) and publishes finished
 * results behind a job state the UI polls, which keeps the 60 Hz draw loop
 * from ever stalling on curl.
 */
#ifndef VM_APP_H
#define VM_APP_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "innertube.h"
#include "lyrics.h"

typedef enum {
    APP_SCR_HOME = 0,
    APP_SCR_RESULTS,
    APP_SCR_LIBRARY,
    APP_SCR_FAVORITES,
    APP_SCR_PLAYLISTS,
    APP_SCR_PLVIEW,
    APP_SCR_ACCOUNT,
    APP_SCR_RECENT, /* kept for back navigation, not shown by itself */
    APP_SCR_NOW,
    APP_SCR_LYRICS,
    APP_SCR_QUEUE,
    APP_SCR_SETTINGS
} app_screen;

typedef enum {
    JOB_IDLE = 0,
    JOB_RUNNING,
    JOB_DONE,
    JOB_FAILED
} app_job_state;

typedef enum {
    REPEAT_OFF = 0,
    REPEAT_ALL,
    REPEAT_ONE
} app_repeat_mode;

typedef struct {
    /* --- search results --- */
    vm_track_t results[VM_MAX_RESULTS];
    int result_count;
    char query[160];
    app_job_state search_state;
    char search_error[128];

    /* --- library --- */
    vm_track_t favorites[VM_MAX_LIBRARY];
    int favorite_count;
    vm_track_t history[VM_MAX_HISTORY];
    int history_count;

    /* --- playback queue --- */
    vm_track_t queue[VM_MAX_QUEUE];
    int queue_count;
    int queue_index;
    app_repeat_mode repeat_mode;
    int shuffle;

    /* --- lyrics --- */
    vm_lyrics_t lyrics;
    app_job_state lyrics_state;
    int lyrics_scroll;

    /* --- artwork --- */
    app_job_state thumb_state;
    char thumb_id[VM_ID_LEN];

    /* --- settings --- */
    int volume;
    int ui_scale_percent;
    int net_warning; /* the CA bundle is missing, TLS runs unverified */

    /* --- local account (display name; not a Google login) --- */
    char account_name[64];
    int account_logged_in;

    /* --- saved playlists (named copies of the playback queue) --- */
    char playlist_names[VM_MAX_PLAYLISTS][VM_MAX_PLAYLIST_NAME];
    vm_track_t playlist_tracks[VM_MAX_PLAYLISTS][VM_MAX_QUEUE];
    int playlist_counts[VM_MAX_PLAYLISTS];
    int playlist_count;
} vm_app_t;

extern vm_app_t g_app;

void app_init(void);
void app_shutdown(void);
/* Advances queue transitions; call once per frame from the UI loop. */
void app_tick(void);

/* --- search --------------------------------------------------------------- */
void app_start_search(const char *query);
/* Clears the finished result so the next search can start; call once the UI
 * has picked the results up. */
void app_search_consume(void);

/* --- playback ------------------------------------------------------------- */
/* Starts `index` of the list the given screen is showing. */
int app_play_from_results(int index);
int app_play_from_favorites(int index);
int app_play_from_history(int index);
int app_play_from_queue(int index);
/* Saved playlists: load one into the queue (starting at `index`, or without
 * playing when index < 0), store the current queue under a name, drop it. */
int app_play_from_playlist(int playlist, int index);
int app_playlist_save(const char *name);
/* Creates an (optionally one-track) playlist and appends a single track to an
 * existing one; both reject duplicate names/tracks. */
int app_playlist_create(const char *name, const vm_track_t *first);
int app_playlist_add_track(int playlist, const vm_track_t *track);
void app_playlist_remove_track(int playlist, int index);
void app_playlist_delete(int playlist);
void app_play_next(void);
void app_play_prev(void);
void app_toggle_pause(void);
void app_stop(void);
void app_seek_ms(uint32_t ms);
void app_seek_relative(int seconds);

/* --- lyrics --------------------------------------------------------------- */
/* Queues a lookup for the track that is playing (or `track`). */
void app_request_lyrics(void);
void app_lyrics_reset(void);

/* --- artwork -------------------------------------------------------------- */
/*
 * Returns the path of the cached JPEG for `track` and queues the download when
 * it is missing. The pointer refers to a shared buffer and is only valid until
 * the next call.
 */
const char *app_thumb_file(const vm_track_t *track);
/* Drops a cached artwork file that refused to decode so the next draw
 * re-fetches it. Used once per track to avoid re-download loops. */
void app_thumb_invalidate(const vm_track_t *track);

/* --- library -------------------------------------------------------------- */
int app_is_favorite(const char *video_id);
void app_toggle_favorite(const vm_track_t *track);
/* 1 when a usable offline copy of `video_id` is in the stream cache. */
int app_is_cached(const char *video_id);
/* Queues a background download so the track can play offline. */
void app_download_offline(const vm_track_t *track);

/* --- account -------------------------------------------------------------- */
int app_account_logged_in(void);
const char *app_account_name(void);
void app_account_login(const char *name);
void app_account_logout(void);

/* --- settings ------------------------------------------------------------- */
void app_set_volume(int percent);
int app_volume(void);
void app_cycle_repeat(void);
void app_toggle_shuffle(void);
void app_clear_cache(void);

/* --- transient messages --------------------------------------------------- */
void app_toast(const char *fmt, ...);
const char *app_toast_text(void);
int app_toast_active(void);

#endif /* VM_APP_H */
