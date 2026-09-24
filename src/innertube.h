/*
 * VitaMusic - YouTube Music (Innertube) client.
 *
 * Search runs as WEB_REMIX on music.youtube.com. Playback resolves through
 * VISIONOS → ANDROID → WEB_EMBEDDED on www.youtube.com, preferring HLS or a
 * direct (unciphered) AAC / muxed-MP4 URL. There is no on-device JS decipher.
 */
#ifndef VM_INNERTUBE_H
#define VM_INNERTUBE_H

#include <stddef.h>
#include <stdint.h>

#define VM_ID_LEN        16
#define VM_TITLE_LEN     192
#define VM_ARTIST_LEN    160
#define VM_ALBUM_LEN     160
#define VM_THUMB_URL_LEN 512
#define VM_STREAM_URL_LEN 4096

typedef struct {
    char id[VM_ID_LEN]; /* YouTube video id */
    char title[VM_TITLE_LEN];
    char artist[VM_ARTIST_LEN];
    char album[VM_ALBUM_LEN];
    char thumb_url[VM_THUMB_URL_LEN];
    int duration_s; /* 0 when the shelf did not say */
} vm_track_t;

typedef struct {
    char url[VM_STREAM_URL_LEN];
    char mime[80];
    int itag;
    int bitrate;
    int duration_s;
    int expires_in;
    int client; /* which Innertube client produced the URL: 0=ANDROID,
                 * 1=VISIONOS, 2=WEB_EMBEDDED; the downloader must reuse its UA */
    int hls;    /* play from the HLS audio playlist instead of `url` */
    char hls_url[VM_STREAM_URL_LEN];
} vm_stream_t;

/* Loads the cached visitor id or fetches one. Returns 0 on success. */
int it_init(void);
/* Kicks the innertube visitor id at the caller's convenience; it may block on
 * the network, so call it from a worker thread and let the UI paint first. */
int it_refresh_visitor(void);
/* 1 once a visitor id (cached or freshly fetched) is usable. */
int it_visitor_ready(void);

/* Fills up to `max_results` tracks. Returns 0 on success. */
int it_search(const char *query, vm_track_t *out, int max_results,
              int *out_count);

/* Resolves the playable AAC stream for `video_id`. */
int it_resolve_stream(const char *video_id, vm_stream_t *out);

/* Rewrites a thumbnail URL into a JPEG of about `px` pixels per side. */
void it_thumbnail_url(const char *url, int px, char *out, size_t out_size);

#endif /* VM_INNERTUBE_H */
