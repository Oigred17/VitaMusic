/*
 * VitaMusic - AAC decoding through the Vita's hardware decoder.
 *
 * mpg123 would be the obvious choice on the Vita, but YouTube Music only
 * serves AAC (itag 140) and Opus, so the port drives SceAudiodec instead. It
 * decodes raw AAC access units (the esds config comes from the MP4 sample
 * entry), which is the shape the MP4 sample table hands us.
 */
#ifndef VM_DECODER_H
#define VM_DECODER_H

#include <stdint.h>

typedef struct vm_dec vm_dec;

/* Loads the codec module and the AAC library. Returns 0 on success. */
int vm_dec_init(void);
void vm_dec_shutdown(void);

/* One decoder per playback stream. `sbr` marks an explicit HE-AAC config;
 * `adts` feeds complete ADTS frames (the HLS audio playlist) instead of raw
 * AAC access units. */
vm_dec *vm_dec_create(int channels, int sample_rate, int sbr, int adts);
void vm_dec_destroy(vm_dec *dec);
/* Drops decoder state after a seek. */
void vm_dec_reset(vm_dec *dec);

/*
 * Decodes one access unit. On success `*pcm` points at the decoder's internal
 * PCM buffer (valid until the next call or until the decoder is destroyed) and
 * `*pcm_bytes` is how much of it is filled.
 */
int vm_dec_decode(vm_dec *dec, const uint8_t *es, uint32_t es_size,
                    const int16_t **pcm, uint32_t *pcm_bytes);

/* Stream parameters as reported by the decoder, valid after the first frame. */
int vm_dec_channels(const vm_dec *dec);
int vm_dec_sample_rate(const vm_dec *dec);

#endif /* VM_DECODER_H */
