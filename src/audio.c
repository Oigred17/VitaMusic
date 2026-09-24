#include "audio.h"
#include "config.h"
#include "util.h"

#include <psp2/audioout.h>

#include <string.h>

/*
 * Audio pipeline. The decoder pushes PCM frames into a FIFO and the player
 * pulls complete grains out with blocking sceAudioOutOutput calls, which pace
 * the loop against real time. The FIFO is deliberately deep: once the player
 * starts draining it, whatever is inside is the only slack a stalled network
 * transfer has, so the buffer target in the player keeps it close to full.
 */

/* ~11.6 s of 44.1 kHz audio (536 x 1024-frame grains, ~2.2 MiB static). The
 * player keeps a ~10.9 s backlog on top of the ~200 ms already handed to the
 * hardware, deep enough to ride out long Wi-Fi stalls and slow-flash IO
 * hiccups while the transfer thread keeps writing the cache file. */
#define VM_FIFO_FRAMES (VM_OUT_GRAIN * 536)

static struct {
    int port;
    int rate;
    int channels;
    int grain;
    int16_t fifo[VM_FIFO_FRAMES * 2]; /* stereo interleaved */
    int head;
    int count;
    int volume;
} g_audio = { -1, 0, 0, 0, { 0 }, 0, 0, 70 };

int vm_audio_init(void)
{
    return 0;
}

void vm_audio_shutdown(void)
{
    vm_audio_close();
}

int vm_audio_is_open(void)
{
    return g_audio.port >= 0;
}

int vm_audio_grain(void)
{
    return g_audio.grain ? g_audio.grain : VM_OUT_GRAIN;
}

static int audio_rate(void)
{
    return g_audio.rate > 0 ? g_audio.rate : VM_OUT_RATE;
}

static void apply_volume(void)
{
    int vol;

    if (g_audio.port < 0)
        return;
    vol = (VM_OUT_VOLUME_MAX * g_audio.volume) / 100;
    sceAudioOutSetVolume(g_audio.port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         (int[]){ vol, vol });
}

int vm_audio_open(int sample_rate, int channels)
{
    int grain = VM_OUT_GRAIN;
    int port;

    vm_audio_close();

    if (sample_rate <= 0)
        sample_rate = VM_OUT_RATE;
    if (channels <= 0)
        channels = 2;

    /* sceAudioOutOutput requires a granularity between SCE_AUDIO_MIN_LEN and
     * SCE_AUDIO_MAX_LEN; anything else has to be trimmed. */
    if (grain < SCE_AUDIO_MIN_LEN)
        grain = SCE_AUDIO_MIN_LEN;
    if (grain > SCE_AUDIO_MAX_LEN)
        grain = SCE_AUDIO_MAX_LEN;
    grain &= ~63;

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, grain, sample_rate,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        vm_log("audio: open failed 0x%08X (%d Hz)\n", (unsigned int)port,
                 sample_rate);
        return -1;
    }

    g_audio.port = port;
    g_audio.rate = sample_rate;
    g_audio.channels = channels;
    g_audio.grain = grain;
    g_audio.head = 0;
    g_audio.count = 0;
    apply_volume();

    vm_log("audio: BGM port %d at %d Hz, grain %d\n", port, sample_rate, grain);
    return 0;
}

void vm_audio_close(void)
{
    if (g_audio.port < 0)
        return;
    /* Wait for the queued grain so the port does not cut off mid sample. */
    sceAudioOutOutput(g_audio.port, NULL);
    sceAudioOutReleasePort(g_audio.port);
    g_audio.port = -1;
    g_audio.head = 0;
    g_audio.count = 0;
}

void vm_audio_flush(void)
{
    g_audio.head = 0;
    g_audio.count = 0;
}

int vm_audio_queued_frames(void)
{
    return g_audio.count;
}

int vm_audio_buffered_ms(void)
{
    return (int)(((int64_t)g_audio.count * 1000LL) / audio_rate());
}

int vm_audio_queue(const int16_t *frames, int frame_count)
{
    int i;

    if (g_audio.port < 0 || !frames || frame_count <= 0)
        return 0;
    if (frame_count > VM_FIFO_FRAMES - g_audio.count) {
        /* The player stops decoding ahead well before capacity, so this is
         * only a safety net; skip the frames that do not fit (the newest)
         * rather than the ones about to play, which would glitch the audio
         * mid stream. */
        frame_count = VM_FIFO_FRAMES - g_audio.count;
    }

    for (i = 0; i < frame_count; i++) {
        int slot = (g_audio.head + g_audio.count) % VM_FIFO_FRAMES;

        if (g_audio.channels == 1) {
            /* Mono source into a stereo port: the same sample on both
             * channels. */
            int16_t s = frames[i];
            g_audio.fifo[slot * 2] = s;
            g_audio.fifo[slot * 2 + 1] = s;
        } else {
            g_audio.fifo[slot * 2] = frames[i * 2];
            g_audio.fifo[slot * 2 + 1] = frames[i * 2 + 1];
        }
        g_audio.count++;
    }

    return frame_count;
}

int vm_audio_drain(void)
{
    int played = 0;
    int grain = vm_audio_grain();

    while (g_audio.port >= 0 && g_audio.count >= grain) {
        int16_t grain_buf[VM_OUT_GRAIN * 2];
        int i;

        for (i = 0; i < grain; i++) {
            int slot = (g_audio.head + i) % VM_FIFO_FRAMES;

            grain_buf[i * 2] = g_audio.fifo[slot * 2];
            grain_buf[i * 2 + 1] = g_audio.fifo[slot * 2 + 1];
        }

        g_audio.head = (g_audio.head + grain) % VM_FIFO_FRAMES;
        g_audio.count -= grain;

        /* Blocking: paces the decode loop against real time, so the FIFO
         * empties no faster than the listener hears it. */
        sceAudioOutOutput(g_audio.port, grain_buf);
        played += grain;
    }

    return played;
}

/* Hands exactly one grain to the hardware and blocks for its duration: the
 * player's clock during stalls and the track handoff. Returns 0 when there
 * was no complete grain to play. */
int vm_audio_drain_one(void)
{
    int16_t grain_buf[VM_OUT_GRAIN * 2];
    int grain = vm_audio_grain();
    int i;

    if (g_audio.port < 0 || g_audio.count < grain)
        return 0;
    for (i = 0; i < grain; i++) {
        int slot = (g_audio.head + i) % VM_FIFO_FRAMES;

        grain_buf[i * 2] = g_audio.fifo[slot * 2];
        grain_buf[i * 2 + 1] = g_audio.fifo[slot * 2 + 1];
    }
    g_audio.head = (g_audio.head + grain) % VM_FIFO_FRAMES;
    g_audio.count -= grain;
    sceAudioOutOutput(g_audio.port, grain_buf);
    return grain;
}

/* Sample rate the port was opened at (VM_OUT_RATE while it is closed). */
int vm_audio_output_rate(void)
{
    return g_audio.rate > 0 ? g_audio.rate : VM_OUT_RATE;
}

void vm_audio_set_volume(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;
    g_audio.volume = percent;
    apply_volume();
}

int vm_audio_volume(void)
{
    return g_audio.volume;
}
