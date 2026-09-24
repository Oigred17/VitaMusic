/*
 * VitaMusic - playback thread.
 *
 * One thread owns the whole chain: resolving a stream URL, pulling the audio
 * down into the cache, decoding it with the hardware codec and feeding the
 * audio port. The UI only ever reads the published state, which is why
 * playback keeps running while searches and artwork downloads happen.
 */
#ifndef VM_PLAYER_H
#define VM_PLAYER_H

#include <stdint.h>

#include "innertube.h"

typedef enum {
    PL_IDLE = 0,
    PL_RESOLVING, /* asking Innertube for a stream URL */
    PL_BUFFERING, /* downloading into the cache */
    PL_PLAYING,
    PL_PAUSED,
    PL_ERROR,
    PL_ENDED /* finished; the app decides what comes next */
} pl_state;

void pl_init(void);
void pl_shutdown(void);

/* Starts `track`. Safe to call while something else is playing. */
void pl_play(const vm_track_t *track);
void pl_toggle_pause(void);
void pl_stop(void);
void pl_seek_ms(uint32_t ms);
/* Downloads `track` into the cache in the background; NULL or an empty track
 * cancels whatever prefetch is in flight. */
void pl_prefetch(const vm_track_t *track);

pl_state pl_get_state(void);
uint32_t pl_position_ms(void);
uint32_t pl_duration_ms(void);
/* Download progress while buffering, 0..100. */
int pl_buffer_percent(void);
/* Milliseconds of decoded audio queued at the speaker: the playback headroom
 * against a stalled transfer. VM_BUF_TARGET_MS is the steady-state level. */
int pl_buffered_ms(void);
/* Steady-state backlog the player aims for, in milliseconds. */
#define PL_BUF_TARGET_MS ((480 * 1000) / 44100)
/* Last status or error text for the UI. */
const char *pl_message(void);
/* Copies the track that is loaded and returns 1, or 0 when there is none. */
int pl_current_track(vm_track_t *out);

#endif /* VM_PLAYER_H */
