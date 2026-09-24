/*
 * VitaMusic - just enough MP4 (and ADTS) to play a YouTube audio
 * stream.
 *
 * The non-fragmented DASH audio tracks carry a plain MP4 with the moov box in
 * front of a single mdat, so the sample table can be parsed up front and every
 * AAC frame read back by index. The audio playlist the HLS client serves
 * concatenates raw ADTS frames instead, which mp4_open detects and maps onto
 * the very same sample table (is_adts marks that mode). Only the boxes needed
 * for that are understood; anything else is skipped.
 */
#ifndef VM_MP4_H
#define VM_MP4_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int fd;
    uint64_t file_size;

    /* Sample table: one entry per AAC access unit. */
    uint32_t count;
    uint32_t *sizes;
    uint64_t *offsets;
    /* Samples per chunk, expanded from stsc so the offset table can be built
     * in a single pass. */
    uint32_t *samples_in_chunk;
    uint64_t stsc_start;
    uint64_t stsc_end;

    /* Stream parameters, taken from the esds audio specific config. */
    int channels;
    int sample_rate;
    int object_type;      /* audioObjectType, 2 = AAC-LC */
    int sbr;              /* explicit SBR / PS present */
    uint32_t frame_samples; /* PCM frames per access unit (1024, or 2048 with SBR) */

    /* 1 when the file is a raw ADTS stream: then every access unit is a
     * complete ADTS frame (isAdts=1 for the decoder) instead of the raw
     * payload an MP4 sample table yields. */
    int is_adts;
    /* ADTS only: 1 when the frame walk validated every header in chain (each
     * frame's declared length lands exactly on the next sync word). A 0 means
     * the table contains probable false syncs inside AAC payloads, which the
     * decoder would reproduce as bursts of noise mid song. */
    int adts_strict;

    uint32_t duration_ms; /* from mdhd, 0 when unavailable */
} vm_mp4_t;

/* Opens `path` and parses its audio track. Returns 0 on success. */
int vm_mp4_open(vm_mp4_t *mp4, const char *path);
void vm_mp4_close(vm_mp4_t *mp4);

/* Reads access unit `index` into `buf`. Returns the byte count or -1. */
int vm_mp4_read_sample(vm_mp4_t *mp4, uint32_t index, uint8_t *buf,
                         uint32_t cap);

/* Playback position of access unit `index`, in milliseconds. */
uint32_t vm_mp4_sample_time_ms(const vm_mp4_t *mp4, uint32_t index);
/* Access unit index whose position is closest to (but not past) `ms`. */
uint32_t vm_mp4_time_to_sample(const vm_mp4_t *mp4, uint32_t ms);

#endif /* VM_MP4_H */
