#include "player.h"
#include "audio.h"
#include "config.h"
#include "decoder.h"
#include "mp4.h"
#include "net.h"
#include "util.h"

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* libcurl runs a TLS handshake on this stack, so it is generously sized. */
#define VM_PLAYER_STACK 0x40000
#define VM_PLAYER_PRIO 0x10000100

#define VM_ES_BUF 2048

struct resampler {
    int in_rate;
    int out_rate;
    int channels;
    int16_t prev[2];
    double phase;
};

struct player {
    SceUID thread;
    SceKernelLwMutexWork lock;
    volatile int running;
    /* Speaker-clock thread: pulls grains from the FIFO at real-time pace so
     * IO stalls in the decode loop can never starve the port. */
    SceUID out_thread;
    volatile int out_go;

    /* --- requests --- */
    volatile int play_req;
    vm_track_t play_track;
    volatile int stop_req;
    volatile int want_pause;
    volatile int seek_pending;
    volatile uint32_t seek_ms;

    /* --- published state --- */
    volatile pl_state state;
    volatile int buffer_pct;
    volatile uint32_t position_ms;
    volatile uint32_t duration_ms;
    volatile int has_track;
    vm_track_t current;
    char message[192];

    /* --- transfer --- */
    volatile int cancel;
    vm_dl_progress_t progress;

    /* Streaming download: the transfer runs on its own thread and the decoder
     * reads the same growing file, so playback starts as soon as the first
     * usable data has landed while the rest keeps filling the cache. */
    SceUID dl_thread;
    volatile int dl_active;
    int dl_ok;
    struct {
        char path[300];
        unsigned hls;
        int client;
        char url[VM_STREAM_URL_LEN];
        char hls_url[VM_STREAM_URL_LEN];
    } dl;
};

/* Background download of the next queued track into the cache, so the player
 * rarely has to wait on a cold file. Cancel is only ever consulted inside the
 * transfer; it is re-armed before each new request is processed. */
static struct {
    SceUID thread;
    volatile int req;
    volatile int cancel;
    vm_track_t track;
} g_pf;

static struct player g_pl;
/* Set when the audio port could not be opened: the cached file is fine, so
 * play_track must NOT delete it and loop into a refetch (it used to wipe good
 * cache entries on a transient port error). */
static volatile int g_audio_failed;

static void lock(void)
{
    sceKernelLockLwMutex(&g_pl.lock, 1, NULL);
}

static void unlock(void)
{
    sceKernelUnlockLwMutex(&g_pl.lock, 1);
}

static void set_state(pl_state state)
{
    lock();
    g_pl.state = state;
    unlock();
}

static void set_message(const char *fmt, ...)
{
    va_list args;

    lock();
    va_start(args, fmt);
    vsnprintf(g_pl.message, sizeof(g_pl.message), fmt, args);
    va_end(args);
    unlock();
}

static void fail(const char *fmt, ...)
{
    va_list args;

    lock();
    va_start(args, fmt);
    vsnprintf(g_pl.message, sizeof(g_pl.message), fmt, args);
    va_end(args);
    g_pl.state = PL_ERROR;
    unlock();
}

/* -------------------------------------------------------------------------- */
/* linear resampling (only for rates sceAudioOut cannot take)                 */
/* -------------------------------------------------------------------------- */

static int supported_rate(int rate)
{
    switch (rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        return 1;
    default:
        return 0;
    }
}

static void resampler_init(struct resampler *rs, int in_rate, int channels)
{
    memset(rs, 0, sizeof(*rs));
    rs->in_rate = in_rate;
    rs->out_rate = VM_OUT_RATE;
    rs->channels = channels > 1 ? 2 : 1;
}

/*
 * Linear interpolation from `in_frames` interleaved frames at in_rate to
 * out_rate. The phase and the last input frame carry over between blocks so
 * the output stays continuous.
 */
static int resample_block(struct resampler *rs, const int16_t *in, int in_frames,
                          int16_t *out, int out_cap)
{
    double ratio = (double)rs->in_rate / (double)rs->out_rate;
    int ch = rs->channels;
    int i = 0;
    int produced = 0;
    double pos = rs->phase;

    while (i < in_frames && produced < out_cap) {
        const int16_t *a = (i == 0) ? rs->prev : &in[(i - 1) * ch];
        const int16_t *b = &in[i * ch];
        int c;

        for (c = 0; c < ch; c++) {
            double value = a[c] + (b[c] - a[c]) * pos;

            if (value > 32767.0)
                value = 32767.0;
            if (value < -32768.0)
                value = -32768.0;
            out[produced * ch + c] = (int16_t)value;
        }
        produced++;

        pos += ratio;
        while (pos >= 1.0 && i < in_frames) {
            pos -= 1.0;
            i++;
        }
    }

    while (pos >= 1.0)
        pos -= 1.0;
    rs->phase = pos;

    if (in_frames > 0) {
        int c;

        for (c = 0; c < ch; c++)
            rs->prev[c] = in[(in_frames - 1) * ch + c];
    }

    return produced;
}

/* -------------------------------------------------------------------------- */
/* playback                                                                   */
/* -------------------------------------------------------------------------- */

static void stream_path(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, VM_CACHE_DIR "/%s.m4a", id);
}

static void stream_part_path(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, VM_CACHE_DIR "/%s.m4a.part", id);
}

/* Promotes a finished download into the durable cache name. Incomplete
 * transfers stay on `.part` so app_is_cached never treats them as ready. */
static int cache_commit(const char *part, const char *final_path)
{
    if (!vm_file_exists(part) || vm_file_size(part) < VM_STREAM_MIN_BYTES) {
        sceIoRemove(part);
        return -1;
    }
    sceIoRemove(final_path);
    if (sceIoRename(part, final_path) >= 0)
        return 0;
    if (vm_copy_file(part, final_path)) {
        sceIoRemove(part);
        return 0;
    }
    return -1;
}

static int cache_ready(const char *id)
{
    char path[300];

    stream_path(path, sizeof(path), id);
    return vm_file_exists(path) && vm_file_size(path) >= VM_STREAM_MIN_BYTES;
}

/*
 * Decodes and plays `path`. While a transfer is still writing to the file
 * (g_pl.dl_active) the loop opens the stream as soon as it can be parsed and
 * gates every sample on its bytes having landed, so audio reaches the speaker
 * while the rest of the track keeps downloading into the same file and ends
 * up cached. Once the file is complete this behaves like a plain file player.
 * Returns 0 when the track ran to the end, -1 when it was cut short (stop, a
 * different track, an unreadable file or a failed transfer).
 */
/* Handoff target: 256 grains (~5.8 s) of decoded audio before the first
 * blocking drain, so the hardware queue can never run dry right after the
 * track starts while the transfer is still ramping up (and while the screen
 * off power profile throttles the decode/download threads). */
#define VM_START_FRAMES (VM_OUT_GRAIN * 256)
/* Playback keeps the decoded FIFO near this level: whatever is queued is the
 * only slack a stalled transfer has, so more is simply fewer stutters. */
#define VM_BUF_TARGET_FRAMES (VM_OUT_GRAIN * 480)
/* Same target in milliseconds, for the "buffer X s" badge. */
#define VM_BUF_TARGET_MS ((VM_BUF_TARGET_FRAMES * 1000) / VM_OUT_RATE)

static int play_growing(const vm_track_t *track, const char *path)
{
    vm_mp4_t mp4;
    vm_dec *dec = NULL;
    static uint8_t es_buf[VM_ES_BUF];
    struct resampler rs;
    int port_open = 0;
    int resampling = 0;
    int opened = 0;
    int playback_started = 0;
    int i = 0;
    int completed = 0;
    uint32_t start_ms = 0;

    memset(&mp4, 0, sizeof(mp4));

    /* Drop whatever the previous track still had queued: waiting through a
     * backlog on skip feels broken, and leftover grains bleeding into the new
     * track is exactly the "jumps to the next song" artefact. */
    vm_audio_flush();

    for (;;) {
        if (g_pl.stop_req || g_pl.play_req)
            break;

        if (g_pl.seek_pending) {
            lock();
            start_ms = g_pl.seek_ms;
            g_pl.seek_pending = 0;
            unlock();
            /* A seek before the stream opened is applied when it does. */
            if (opened) {
                i = (int)vm_mp4_time_to_sample(&mp4, start_ms);
                vm_dec_reset(dec);
                vm_audio_flush();
                playback_started = 0;
                g_pl.out_go = 0;
                lock();
                g_pl.position_ms = start_ms;
                unlock();
            }
            continue;
        }

        /* Decode-ahead backpressure: once the backlog reaches target, pause
         * decoding; the output thread drains it back below the line and
         * decoding resumes. The FIFO stays near full without the decode loop
         * ever blocking on the speaker. */
        if (vm_audio_queued_frames() >= VM_BUF_TARGET_FRAMES) {
            sceKernelDelayThread(20000);
            continue;
        }

        if (!opened) {
            long avail = vm_file_size(path);

            /* Wait for a small head of the file before spending CPU on a
             * full ADTS/MP4 parse — keeps the UI responsive and starts
             * audio as soon as the first frames land. */
            if (avail >= 0 && avail < 16384 && g_pl.dl_active) {
                sceKernelDelayThread(50000);
                continue;
            }
            if (vm_mp4_open(&mp4, path) == 0) {
                opened = 1;
                if (!dec) {
                    dec = vm_dec_create(mp4.channels, mp4.sample_rate,
                                        mp4.sbr, mp4.is_adts);
                    if (!dec) {
                        vm_mp4_close(&mp4);
                        return -1;
                    }
                    set_state(PL_PLAYING);
                    set_message("Reproduciendo %s", track->title);
                }
                /* Never shrink duration from a partial ADTS scan. */
                if (mp4.duration_ms > 0) {
                    lock();
                    if (mp4.duration_ms > g_pl.duration_ms)
                        g_pl.duration_ms = mp4.duration_ms;
                    unlock();
                }
                if (start_ms > 0)
                    i = (int)vm_mp4_time_to_sample(&mp4, start_ms);
                continue;
            }
            /* Either the moov box is not there yet, the file has not gathered
             * enough ADTS frames, or the transfer died on us. */
            if (g_pl.stop_req || g_pl.play_req)
                break;
            if (!g_pl.dl_active)
                return -1; /* transfer over, nothing parses */
            sceKernelDelayThread(80000);
            continue;
        }

        if (i >= (int)mp4.count) {
            if (!mp4.is_adts) {
                /* An MP4 moov lists every sample and each one was gated on
                 * being present, so reaching the end means it played out. */
                completed = 1;
                break;
            }
            {
                long avail = vm_file_size(path);

                /* ADTS tables only cover what had landed at open time: rescan
                 * whenever the file grew (or the transfer still runs). Without
                 * this final rescan the song jumped to the next one the
                 * moment the download completed, cutting its tail. */
                if (g_pl.dl_active ||
                    (avail > 0 && (uint64_t)avail > mp4.file_size)) {
                    vm_mp4_close(&mp4);
                    opened = 0;
                    continue;
                }
                completed = 1;
                break;
            }
        }

        if (g_pl.want_pause) {
            set_state(PL_PAUSED);
            /* The output thread stops draining while want_pause is set: the
             * FIFO stays put, so resuming is instant and nothing is lost. */

            while (g_pl.want_pause && !g_pl.stop_req && !g_pl.play_req &&
                   g_pl.running) {
                if (g_pl.seek_pending) {
                    lock();
                    start_ms = g_pl.seek_ms;
                    g_pl.seek_pending = 0;
                    unlock();
                    i = (int)vm_mp4_time_to_sample(&mp4, start_ms);
                    vm_dec_reset(dec);
                    vm_audio_flush();
                    playback_started = 0;
                    lock();
                    g_pl.position_ms = start_ms;
                    unlock();
                }
                sceKernelDelayThread(30000);
            }

            if (g_pl.stop_req || g_pl.play_req)
                break;
            set_state(PL_PLAYING);
        }

        /* Do not read a sample whose bytes have not been written yet. Keep
         * draining the FIFO while we wait so the port never underruns. */
        if (g_pl.dl_active) {
            long avail = vm_file_size(path);
            uint64_t need = mp4.offsets[i] + mp4.sizes[i];

            if (avail < 0 || (uint64_t)avail < need) {
                /* Transfer stalled: the output thread keeps the speaker fed
                 * from the backlog, so just wait for the bytes here. */
                sceKernelDelayThread(10000);
                continue;
            }
        }

        {
            int len = vm_mp4_read_sample(&mp4, (uint32_t)i, es_buf,
                                           sizeof(es_buf));

            if (len > 0) {
                const int16_t *pcm = NULL;
                uint32_t pcm_bytes = 0;

                if (vm_dec_decode(dec, es_buf, (uint32_t)len, &pcm,
                                    &pcm_bytes) == 0 &&
                    pcm_bytes > 0) {
                    int channels = vm_dec_channels(dec);
                    int rate = vm_dec_sample_rate(dec);
                    int frames = (int)(pcm_bytes / (uint32_t)(2 * channels));

                    if (!port_open) {
                        /* The decoder knows the real output rate only now (an
                         * SBR stream reports twice its core rate). */
                        resampling = !supported_rate(rate);
                        if (resampling)
                            resampler_init(&rs, rate, channels);

                        if (vm_audio_open(resampling ? VM_OUT_RATE : rate,
                                            channels) != 0) {
                            /* A port may still be draining from the previous
                             * track; one short retry absorbs it. */
                            sceKernelDelayThread(120000);
                            if (vm_audio_open(resampling ? VM_OUT_RATE : rate,
                                                channels) != 0) {
                                fail("Audio output unavailable at %d Hz",
                                     rate);
                                g_audio_failed = 1;
                                vm_dec_destroy(dec);
                                vm_mp4_close(&mp4);
                                return -1;
                            }
                        }
                        port_open = 1;
                    }

                    if (resampling) {
                        int16_t converted[VM_OUT_GRAIN * 2];
                        int produced = resample_block(&rs, pcm, frames,
                                                      converted,
                                                      VM_OUT_GRAIN * 2 /
                                                          (channels > 1 ? 2
                                                                        : 1));

                        if (produced > 0)
                            vm_audio_queue(converted, produced);
                    } else {
                        vm_audio_queue(pcm, frames);
                    }

                    if (!playback_started) {
                        if (vm_audio_queued_frames() >= VM_START_FRAMES ||
                            !g_pl.dl_active) {
                            playback_started = 1;
                            g_pl.out_go = 1;
                        }
                    }

                    /* Position is advanced by the output thread as grains
                     * actually reach the speaker; nothing to do here. */
                }
            } else if (len < 0 && !g_pl.dl_active) {
                /* A read past the end of a finished file means the transfer
                 * was corrupt, or was cancelled and removed under us. */
                break;
            } else if (len < 0) {
                /* Sample not on disk yet: the output thread owns the speaker;
                 * just wait for the bytes here. */
                sceKernelDelayThread(10000);
                continue;
            }
        }

        i++;
    }

    /* Let the output thread play the remaining backlog before the player
     * reports the end; otherwise the last seconds get cut and the queue
     * advances early. A stop or a track switch exits immediately. */
    if (port_open) {
        while (!g_pl.stop_req && !g_pl.play_req &&
               vm_audio_queued_frames() > 0)
            sceKernelDelayThread(20000);
    }

    /* Park the output thread before touching the port: releasing it while
     * the clock thread is mid-Output leaks the port (0x80260005 on the next
     * open) and takes playback down until the app restarts. */
    g_pl.out_go = 0;
    sceKernelDelayThread(60000);

    vm_dec_destroy(dec);
    if (opened)
        vm_mp4_close(&mp4);
    if (port_open)
        vm_audio_close();

    return completed ? 0 : -1;
}

/*
 * Pulls the stream into the cache on its own thread, so the player can start
 * decoding the growing file before the whole track has arrived. The HLS
 * manifest and the DASH URL come out of the same player response, so a dead
 * manifest or stale segment can still fall back to the direct URL the client
 * also exposed.
 */
static int dl_main(SceSize args, void *argp)
{
    vm_http_header_t headers[2];
    const char *ua =
        g_pl.dl.client == 2 ? YT_FALLBACK2_USER_AGENT :
        g_pl.dl.client == 1 ? YT_FALLBACK_USER_AGENT : YT_PLAYER_USER_AGENT;

    (void)args;
    (void)argp;

    /* googlevideo ties the stream URL to the client identity used during
     * resolution; sending the matching user agent avoids 403s on the
     * download itself. */
    headers[0] = (vm_http_header_t){ "Accept", "*/*" };
    headers[1] = (vm_http_header_t){ "User-Agent", ua };

    if (g_pl.dl.hls) {
        if (vm_net_hls_download(g_pl.dl.hls_url, g_pl.dl.path, headers, 1,
                                &g_pl.progress, &g_pl.cancel) == 0) {
            g_pl.dl_ok = 0;
        } else if (!g_pl.cancel && g_pl.dl.url[0]) {
            vm_log("player: HLS failed (%s), using direct URL\n",
                     vm_net_last_error());
            sceIoRemove(g_pl.dl.path);
            lock();
            g_pl.progress.downloaded = 0;
            g_pl.progress.total = -1;
            unlock();
            g_pl.dl_ok =
                vm_net_download(g_pl.dl.url, g_pl.dl.path, headers, 1,
                                &g_pl.progress, &g_pl.cancel) == 0 ? 0 : -1;
        } else {
            g_pl.dl_ok = -1;
        }
    } else {
        g_pl.dl_ok =
            vm_net_download(g_pl.dl.url, g_pl.dl.path, headers, 1,
                            &g_pl.progress, &g_pl.cancel) == 0 ? 0 : -1;
    }

    g_pl.dl_active = 0;
    return 0;
}

static void play_track(const vm_track_t *track)
{
    char path[300];
    char part[300];
    vm_stream_t stream;
    int retried = 0;

    memset(&stream, 0, sizeof(stream));
    stream_path(path, sizeof(path), track->id);
    stream_part_path(part, sizeof(part), track->id);

    lock();
    g_pl.position_ms = 0;
    g_pl.buffer_pct = 0;
    g_pl.progress.downloaded = 0;
    g_pl.progress.total = -1;
    g_pl.duration_ms = track->duration_s > 0
                           ? (uint32_t)track->duration_s * 1000
                           : 0;
    unlock();

    /* Play from a finished cache entry without touching the network. This is
     * what makes a second play of the same song feel instant. */
    if (cache_ready(track->id)) {
        set_state(PL_PLAYING);
        set_message("Cache: %s", track->title);
        g_audio_failed = 0;
        if (play_growing(track, path) == 0) {
            if (g_pl.stop_req || g_pl.play_req)
                return;
            lock();
            g_pl.position_ms = g_pl.duration_ms;
            g_pl.state = PL_ENDED;
            vm_strlcpy(g_pl.message, "Terminado", sizeof(g_pl.message));
            unlock();
            return;
        }
        if (g_pl.stop_req || g_pl.play_req)
            return;
        if (g_audio_failed) {
            /* The file decoded fine up to the port failure; keep the cache
             * and surface the error instead of looping into a refetch. */
            vm_log("player: audio port failed; keeping cache\n");
            return;
        }
        vm_log("player: cached stream is unusable, refetching\n");
        sceIoRemove(path);
        retried = 1;
    }

    set_state(PL_RESOLVING);
    set_message("Preparando %s", track->title);

    if (it_resolve_stream(track->id, &stream) != 0) {
        fail("%s", stream.mime[0] ? stream.mime : vm_net_last_error());
        return;
    }

    if (stream.duration_s > 0) {
        lock();
        g_pl.duration_ms = (uint32_t)stream.duration_s * 1000;
        unlock();
    }

download:
    if (g_pl.stop_req || g_pl.play_req)
        return;

    {
        int rc;

        set_state(PL_BUFFERING);
        set_message("Descargando %s", track->title);

        sceIoRemove(part);
        memset(&g_pl.dl, 0, sizeof(g_pl.dl));
        snprintf(g_pl.dl.path, sizeof(g_pl.dl.path), "%s", part);
        g_pl.dl.hls = stream.hls;
        g_pl.dl.client = stream.client;
        vm_strlcpy(g_pl.dl.url, stream.url, sizeof(g_pl.dl.url));
        vm_strlcpy(g_pl.dl.hls_url, stream.hls_url, sizeof(g_pl.dl.hls_url));
        g_pl.dl_ok = -1;
        g_pl.dl_active = 1;
        g_pl.dl_thread = sceKernelCreateThread(
            "vm_dl", dl_main, VM_PLAYER_PRIO, VM_PLAYER_STACK, 0,
            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT, NULL);
        if (g_pl.dl_thread < 0) {
            g_pl.dl_active = 0;
            fail("Download thread failed");
            return;
        }
        sceKernelStartThread(g_pl.dl_thread, 0, NULL);

        /* Play the growing `.part` file; promote to `.m4a` only when the
         * transfer finishes cleanly so the next play can skip resolve. */
        rc = play_growing(track, part);

        sceKernelWaitThreadEnd(g_pl.dl_thread, NULL, NULL);
        sceKernelDeleteThread(g_pl.dl_thread);
        g_pl.dl_thread = -1;

        if (g_pl.dl_ok == 0)
            cache_commit(part, path);
        else
            sceIoRemove(part);

        if (g_pl.stop_req || g_pl.play_req)
            return;

        if (rc != 0 && g_pl.dl_ok != 0) {
            if (g_pl.cancel) {
                set_state(PL_IDLE);
                return;
            }
            fail("Download failed: %s", vm_net_last_error());
            return;
        }
        if (rc != 0) {
            if (!retried && vm_file_exists(path)) {
                retried = 1;
                vm_log("player: fresh cache unusable, refetching\n");
                sceIoRemove(path);
                goto download;
            }
            if (g_pl.state != PL_ERROR)
                fail("This track could not be decoded");
            return;
        }
    }

    if (g_pl.stop_req || g_pl.play_req)
        return;

    lock();
    g_pl.position_ms = g_pl.duration_ms;
    g_pl.state = PL_ENDED;
    vm_strlcpy(g_pl.message, "Terminado", sizeof(g_pl.message));
    unlock();
}

/* -------------------------------------------------------------------------- */
/* prefetch: warm the cache for the next queued track                         */
/* -------------------------------------------------------------------------- */

static void prefetch_path(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, VM_CACHE_DIR "/%s.m4a", id);
}

static void prefetch_part_path(char *out, size_t cap, const char *id)
{
    snprintf(out, cap, VM_CACHE_DIR "/%s.m4a.part", id);
}

static int prefetch_one(const vm_track_t *track)
{
    char path[300];
    char part[300];
    vm_stream_t stream;
    vm_http_header_t headers[2];
    const char *ua;
    vm_track_t current;
    int rc;

    /* The player writes this very file when the track starts; never race it. */
    if (pl_current_track(&current) && strcmp(current.id, track->id) == 0)
        return 0;

    prefetch_path(path, sizeof(path), track->id);
    prefetch_part_path(part, sizeof(part), track->id);
    if (vm_file_exists(path) && vm_file_size(path) >= VM_STREAM_MIN_BYTES)
        return 0;

    if (it_resolve_stream(track->id, &stream) != 0) {
        vm_log("prefetch: resolve %s failed: %s\n", track->id,
                 vm_net_last_error());
        return -1;
    }

    ua = stream.client == 2 ? YT_FALLBACK2_USER_AGENT :
         stream.client == 1 ? YT_FALLBACK_USER_AGENT : YT_PLAYER_USER_AGENT;
    headers[0] = (vm_http_header_t){ "Accept", "*/*" };
    headers[1] = (vm_http_header_t){ "User-Agent", ua };

    sceIoRemove(part);
    if (stream.hls) {
        rc = vm_net_hls_download(stream.hls_url, part, headers, 1, NULL,
                                 &g_pf.cancel);
        if (rc != 0 && !g_pf.cancel && stream.url[0]) {
            sceIoRemove(part);
            rc = vm_net_download(stream.url, part, headers, 1, NULL,
                                 &g_pf.cancel);
        }
    } else {
        rc = vm_net_download(stream.url, part, headers, 1, NULL, &g_pf.cancel);
    }

    if (rc != 0 || g_pf.cancel) {
        sceIoRemove(part);
        return -1;
    }
    return cache_commit(part, path);
}

static int prefetch_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    while (g_pl.running) {
        vm_track_t track;
        int have = 0;

        lock();
        if (g_pf.req) {
            g_pf.req = 0;
            track = g_pf.track;
            have = 1;
        }
        unlock();

        if (!have) {
            sceKernelDelayThread(20000);
            continue;
        }

        /* A stale cancel from an aborted previous job must not kill this one. */
        g_pf.cancel = 0;
        vm_log("prefetch: warming %s\n", track.id);
        prefetch_one(&track);
        if (!g_pf.cancel)
            vm_log("prefetch: %s done\n", track.id);
    }

    return 0;
}

/*
 * Speaker clock, on its own thread: hands one grain to the output port per
 * real-time tick and advances the published position by exactly what was
 * played. Because this thread never touches the network or the filesystem, an
 * IO stall in the decode loop can no longer starve the port — the deep FIFO
 * absorbs it. The clock stops while paused, seeking or before the start
 * threshold, which is also what freezes/restores the position counter.
 */
static int out_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    while (g_pl.running) {
        int grain;
        int rate;
        uint32_t grain_ms;

        if (!g_pl.out_go || g_pl.want_pause || g_pl.stop_req) {
            sceKernelDelayThread(20000);
            continue;
        }

        grain = vm_audio_grain();
        rate = vm_audio_output_rate();
        grain_ms = (uint32_t)((int64_t)grain * 1000 / (rate ? rate : VM_OUT_RATE));

        if (vm_audio_queued_frames() < grain) {
            /* Backlog empty (transfer stalled or track ending): the port is
             * allowed to run dry here, the position just stops advancing. */
            sceKernelDelayThread(4000);
            continue;
        }

        /* Blocking: sleeps for the grain's duration inside the driver, so
         * this loop is the wall clock playback is paced by. */
        vm_audio_drain_one();
        lock();
        g_pl.position_ms += grain_ms;
        unlock();
    }

    return 0;
}

static int player_main(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    while (g_pl.running) {
        vm_track_t track;
        int have = 0;

        lock();
        if (g_pl.play_req) {
            g_pl.play_req = 0;
            track = g_pl.play_track;
            have = 1;
        }
        unlock();

        if (!have) {
            sceKernelDelayThread(15000);
            continue;
        }

        play_track(&track);
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/* public API                                                                 */
/* -------------------------------------------------------------------------- */

void pl_init(void)
{
    memset(&g_pl, 0, sizeof(g_pl));
    g_pl.state = PL_IDLE;
    g_pl.message[0] = '\0';

    if (sceKernelCreateLwMutex(&g_pl.lock, "vm_player_lock", 2, 0, NULL) < 0)
        vm_log("player: lock creation failed\n");

    vm_dec_init();

    g_pl.running = 1;
    g_pl.dl_thread = -1;
    g_pl.thread = sceKernelCreateThread("vm_player", player_main,
                                        VM_PLAYER_PRIO, VM_PLAYER_STACK, 0,
                                        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                                        NULL);
    if (g_pl.thread < 0) {
        vm_log("player: thread failed 0x%08X\n", (unsigned int)g_pl.thread);
        return;
    }
    sceKernelStartThread(g_pl.thread, 0, NULL);

    g_pf.thread = sceKernelCreateThread("vm_pref", prefetch_main,
                                        VM_PLAYER_PRIO, VM_PLAYER_STACK, 0,
                                        SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                                        NULL);
    if (g_pf.thread < 0) {
        vm_log("player: prefetch thread failed 0x%08X\n",
                 (unsigned int)g_pf.thread);
    } else {
        sceKernelStartThread(g_pf.thread, 0, NULL);
    }

    g_pl.out_thread = sceKernelCreateThread("vm_out", out_main, VM_PLAYER_PRIO,
                                            0x4000, 0,
                                            SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT,
                                            NULL);
    if (g_pl.out_thread < 0) {
        vm_log("player: output thread failed 0x%08X\n",
                 (unsigned int)g_pl.out_thread);
    } else {
        sceKernelStartThread(g_pl.out_thread, 0, NULL);
    }
}

void pl_shutdown(void)
{
    g_pl.running = 0;
    g_pl.stop_req = 1;
    g_pl.cancel = 1;
    g_pf.cancel = 1;

    if (g_pf.thread >= 0) {
        sceKernelWaitThreadEnd(g_pf.thread, NULL, NULL);
        sceKernelDeleteThread(g_pf.thread);
        g_pf.thread = -1;
    }

    if (g_pl.thread >= 0) {
        sceKernelWaitThreadEnd(g_pl.thread, NULL, NULL);
        sceKernelDeleteThread(g_pl.thread);
        g_pl.thread = -1;
    }

    if (g_pl.out_thread >= 0) {
        sceKernelWaitThreadEnd(g_pl.out_thread, NULL, NULL);
        sceKernelDeleteThread(g_pl.out_thread);
        g_pl.out_thread = -1;
    }

    vm_audio_close();
    vm_dec_shutdown();
}

void pl_play(const vm_track_t *track)
{
    if (!track || !track->id[0])
        return;

    lock();
    g_pl.play_track = *track;
    g_pl.play_req = 1;
    g_pl.stop_req = 0;
    g_pl.cancel = 0;
    g_pl.want_pause = 0;
    g_pl.seek_pending = 0;
    g_pl.seek_ms = 0;
    g_pl.out_go = 0;
    g_pl.current = *track;
    g_pl.has_track = 1;
    g_pl.position_ms = 0;
    g_pl.duration_ms = track->duration_s > 0
                           ? (uint32_t)track->duration_s * 1000
                           : 0;
    g_pl.buffer_pct = 0;
    /* Whatever the prefetch was warming is about to be irrelevant (and it
     * could be writing this very file while we open it), so stand it down. */
    g_pf.cancel = 1;
    /* Published immediately so the UI (and the queue logic) never sees the
     * previous track's terminal state twice. */
    g_pl.state = PL_RESOLVING;
    /* The precision keeps the title from being clipped mid-way by the buffer
     * boundary, which would otherwise be a truncation the compiler flags. */
    snprintf(g_pl.message, sizeof(g_pl.message), "Cargando %.150s",
             track->title);
    unlock();
}

void pl_toggle_pause(void)
{
    pl_state state = g_pl.state;

    if (state != PL_PLAYING && state != PL_PAUSED && state != PL_BUFFERING)
        return;

    lock();
    g_pl.want_pause = !g_pl.want_pause;
    unlock();
}

void pl_stop(void)
{
    lock();
    g_pl.stop_req = 1;
    g_pl.play_req = 0;
    g_pl.cancel = 1;
    g_pl.want_pause = 0;
    g_pl.out_go = 0;
    g_pl.has_track = 0;
    g_pl.buffer_pct = 0;
    g_pf.cancel = 1;
    g_pl.state = PL_IDLE;
    vm_strlcpy(g_pl.message, "Detenido", sizeof(g_pl.message));
    unlock();
}

void pl_seek_ms(uint32_t ms)
{
    if (!g_pl.has_track)
        return;

    lock();
    g_pl.seek_ms = ms;
    g_pl.seek_pending = 1;
    unlock();
}

/*
 * Queues `track` to be fetched into the cache in the background, so the next
 * song is ready before it is asked for. Passing NULL (or an empty track)
 * stands any in-flight prefetch down.
 */
void pl_prefetch(const vm_track_t *track)
{
    lock();
    if (track && track->id[0]) {
        if (g_pf.track.id[0] && strcmp(g_pf.track.id, track->id) != 0)
            g_pf.cancel = 1; /* a different job is on the wire */
        g_pf.track = *track;
        g_pf.req = 1;
    } else {
        g_pf.cancel = 1;
    }
    unlock();
}

pl_state pl_get_state(void)
{
    return g_pl.state;
}

uint32_t pl_position_ms(void)
{
    return g_pl.position_ms;
}

uint32_t pl_duration_ms(void)
{
    return g_pl.duration_ms;
}

int pl_buffer_percent(void)
{
    int64_t total = g_pl.progress.total;
    int64_t done = g_pl.progress.downloaded;

    /* Still report fill while streaming into the cache during playback. */
    if (!g_pl.dl_active && g_pl.state != PL_BUFFERING)
        return 100;
    if (total <= 0) {
        /* No Content-Length: estimate from playback position vs duration. */
        uint32_t dur = g_pl.duration_ms;
        uint32_t pos = g_pl.position_ms;

        if (dur <= 0)
            return g_pl.dl_active ? 0 : 100;
        if (pos >= dur)
            return 95;
        return (int)(pos * 90 / dur);
    }
    if (done >= total)
        return 100;
    return (int)(done * 100 / total);
}

int pl_buffered_ms(void)
{
    return vm_audio_buffered_ms();
}

const char *pl_message(void)
{
    return g_pl.message[0] ? g_pl.message : "Idle";
}

int pl_current_track(vm_track_t *out)
{
    if (!g_pl.has_track)
        return 0;
    if (out)
        *out = g_pl.current;
    return 1;
}
