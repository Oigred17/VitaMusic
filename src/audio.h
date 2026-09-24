/*
 * VitaMusic - audio output thread-side helpers.
 *
 * The BGM port is used rather than MAIN: the MAIN port is locked to 48 kHz
 * while the BGM port accepts every rate AAC can produce, which keeps almost
 * every track free of resampling. Decoded frames are pushed into a small ring
 * buffer and pulled out in fixed grains because sceAudioOutOutput requires an
 * exact sample count per call.
 */
#ifndef VM_AUDIO_H
#define VM_AUDIO_H

#include <stdint.h>

int vm_audio_init(void);
void vm_audio_shutdown(void);

/* Opens the output port. `channels` is 1 or 2; mono is upmixed. */
int vm_audio_open(int sample_rate, int channels);
void vm_audio_close(void);
int vm_audio_is_open(void);
int vm_audio_grain(void);

/* Appends interleaved frames (as many channels as the port was opened with).
 * Returns the number of frames accepted. */
int vm_audio_queue(const int16_t *frames, int frame_count);

/* Plays out every complete grain currently buffered, blocking as needed.
 * Returns the number of frames handed to the hardware. Because the calls are
 * blocking, the FIFO empties at (at most) real-time pace and whatever is
 * queued when the caller stops draining is genuine playback headroom. */
int vm_audio_drain(void);

/* Hands exactly one grain to the hardware and blocks for its duration (~23 ms
 * at the default grain). The player's clock while a transfer is stalled and
 * when handing the speaker over between tracks. Returns the number of frames
 * played, or 0 when no complete grain was buffered. */
int vm_audio_drain_one(void);

/* Sample rate the output port was opened at (falls back to VM_OUT_RATE). */
int vm_audio_output_rate(void);

int vm_audio_queued_frames(void);
/* Milliseconds of decoded audio sitting in the FIFO right now: the real
 * playback headroom against a stalled transfer. */
int vm_audio_buffered_ms(void);
/* Drops everything that has not been played yet (used when seeking). */
void vm_audio_flush(void);

/* 0..100, applied to the output port. */
void vm_audio_set_volume(int percent);
int vm_audio_volume(void);

#endif /* VM_AUDIO_H */
