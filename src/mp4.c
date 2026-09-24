#include "mp4.h"
#include "util.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* byte helpers                                                               */
/* -------------------------------------------------------------------------- */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int read_at(int fd, uint64_t offset, void *buf, uint32_t len)
{
    if (len == 0)
        return 0;
    return sceIoPread(fd, buf, (SceSize)len, (SceOff)offset);
}

/* -------------------------------------------------------------------------- */
/* box walking                                                                */
/* -------------------------------------------------------------------------- */

/*
 * Describes the box starting at `pos`: its payload range plus the offset of
 * the next sibling box.
 */
static int box_at(int fd, uint64_t pos, uint64_t limit, char type[5],
                  uint64_t *payload, uint64_t *payload_end, uint64_t *next)
{
    uint8_t hdr[16];
    uint64_t size;
    uint32_t size32;
    uint32_t header_len;

    if (pos + 8 > limit)
        return -1;
    if (read_at(fd, pos, hdr, 8) != 8)
        return -1;

    size32 = be32(hdr);
    memcpy(type, hdr + 4, 4);
    type[4] = '\0';

    if (size32 == 1) {
        if (pos + 16 > limit)
            return -1;
        if (read_at(fd, pos + 8, hdr + 8, 8) != 8)
            return -1;
        size = be64(hdr + 8);
        header_len = 16;
    } else if (size32 == 0) {
        /* Runs to the end of the enclosing box, which is how mdat is often
         * written. */
        size = limit - pos;
        header_len = 8;
    } else {
        size = size32;
        header_len = 8;
    }

    if (size < header_len || pos + size > limit)
        size = limit - pos; /* tolerate a container that only claims to end late */
    if (size < header_len)
        return -1;

    *payload = pos + header_len;
    *payload_end = pos + size;
    *next = pos + size;
    return 0;
}

/* Finds the first child named `type` inside [start, end). */
static int find_box(int fd, uint64_t start, uint64_t end, const char *type,
                    uint64_t *payload, uint64_t *payload_end)
{
    uint64_t pos = start;

    while (pos + 8 <= end) {
        char name[5];
        uint64_t pl, pe, next;

        if (box_at(fd, pos, end, name, &pl, &pe, &next) != 0)
            return -1;
        if (strcmp(name, type) == 0) {
            *payload = pl;
            *payload_end = pe;
            return 0;
        }
        if (next <= pos)
            return -1;
        pos = next;
    }
    return -1;
}

/* -------------------------------------------------------------------------- */
/* descriptors and the audio specific config                                  */
/* -------------------------------------------------------------------------- */

struct bits {
    const uint8_t *data;
    size_t bit;
    size_t limit_bits;
};

static uint32_t bits_read(struct bits *b, unsigned count)
{
    uint32_t value = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        size_t byte;
        unsigned shift;

        /* Past the end of the config: pad with zeros rather than reading
         * whatever follows in the heap. */
        if (b->bit >= b->limit_bits)
            return value << (count - i);

        byte = b->bit >> 3;
        shift = 7 - (unsigned)(b->bit & 7);
        value = (value << 1) | ((b->data[byte] >> shift) & 1u);
        b->bit++;
    }
    return value;
}

static const uint32_t aac_rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                      24000, 22050, 16000, 12000, 11025, 8000,
                                      7350 };

/* Number of samples per frame implied by an audioObjectType. */
static uint32_t aac_frame_samples(int object_type)
{
    switch (object_type) {
    case 1:  /* AAC Main */
    case 2:  /* AAC LC */
    case 3:  /* AAC SSR */
    case 4:  /* AAC LTP */
    case 6:  /* AAC Scalable */
    case 7:  /* TwinVQ */
    case 17: /* ER AAC LC */
    case 19: /* ER AAC LTP */
    case 20: /* ER AAC Scalable */
    case 21: /* ER TwinVQ */
    case 22: /* ER BSAC */
    case 23: /* ER AAC LD */
        return 1024;
    case 5:
    case 29:
        return 2048; /* SBR / PS */
    default:
        return 1024;
    }
}

static void parse_audio_specific_config(vm_mp4_t *mp4, const uint8_t *asc,
                                        uint32_t len)
{
    struct bits b;
    uint32_t object_type;
    uint32_t rate_index;
    uint32_t rate;
    uint32_t channels;

    if (len == 0)
        return;

    b.data = asc;
    b.bit = 0;
    b.limit_bits = (size_t)len * 8;

    object_type = bits_read(&b, 5);
    if (object_type == 31)
        object_type = 32 + bits_read(&b, 6);

    rate_index = bits_read(&b, 4);
    if (rate_index == 0xF)
        rate = bits_read(&b, 24);
    else if (rate_index < sizeof(aac_rates) / sizeof(aac_rates[0]))
        rate = aac_rates[rate_index];
    else
        rate = 0;

    channels = bits_read(&b, 4);

    if (object_type == 5 || object_type == 29) {
        /* Explicit SBR (or PS): the base object type follows the extension
         * sampling rate. */
        uint32_t ext_index = bits_read(&b, 4);

        if (ext_index == 0xF)
            bits_read(&b, 24);
        mp4->sbr = 1;
        object_type = bits_read(&b, 5);
        if (object_type == 31)
            object_type = 32 + bits_read(&b, 6);
    }

    if (rate > 0)
        mp4->sample_rate = (int)rate;
    if (channels > 0)
        mp4->channels = (int)channels;
    mp4->object_type = (int)object_type;

    mp4->frame_samples = aac_frame_samples(mp4->sbr ? 5 : (int)object_type);
}

/* Walks a chain of MPEG-4 descriptors looking for `tag`. */
static int desc_find(const uint8_t *data, uint32_t len, uint8_t tag,
                     const uint8_t **payload, uint32_t *payload_len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    while (p + 2 <= end) {
        uint8_t id = *p++;
        uint32_t size = 0;
        int i;

        /* Sizes are variable length: seven bits per byte, high bit means
         * "another byte follows". */
        for (i = 0; i < 4 && p < end; i++) {
            uint8_t byte = *p++;
            size = (size << 7) | (uint32_t)(byte & 0x7F);
            if (!(byte & 0x80))
                break;
        }
        if ((uint64_t)(end - p) < size)
            size = (uint32_t)(end - p);

        if (id == tag) {
            *payload = p;
            *payload_len = size;
            return 0;
        }
        p += size;
    }
    return -1;
}

/* Extracts the DecoderSpecificInfo (the audio specific config) from an esds. */
static void parse_esds(vm_mp4_t *mp4, const uint8_t *data, uint32_t len)
{
    const uint8_t *cfg;
    uint32_t cfg_len;

    /* The elementary stream descriptor wraps the decoder config descriptor,
     * which in turn carries the audio specific config. */
    if (desc_find(data, len, 0x03, &cfg, &cfg_len) == 0) {
        if (desc_find(cfg, cfg_len, 0x04, &cfg, &cfg_len) == 0) {
            if (desc_find(cfg, cfg_len, 0x05, &cfg, &cfg_len) == 0) {
                parse_audio_specific_config(mp4, cfg, cfg_len);
                return;
            }
        }
    }

    /* Some muxers leave the config at the top level. */
    if (desc_find(data, len, 0x05, &cfg, &cfg_len) == 0)
        parse_audio_specific_config(mp4, cfg, cfg_len);
}

/* -------------------------------------------------------------------------- */
/* sample tables                                                              */
/* -------------------------------------------------------------------------- */

static int parse_stsz(vm_mp4_t *mp4, uint64_t start, uint64_t end)
{
    uint8_t hdr[12];
    uint32_t fixed_size;
    uint32_t count;
    uint32_t i = 0;

    if (start + 12 > end || read_at(mp4->fd, start, hdr, 12) != 12)
        return -1;

    fixed_size = be32(hdr + 4);
    count = be32(hdr + 8);
    if (count == 0 || count > 4000000u)
        return -1;

    mp4->sizes = calloc(count, sizeof(uint32_t));
    if (!mp4->sizes)
        return -1;
    mp4->count = count;

    if (fixed_size != 0) {
        for (i = 0; i < count; i++)
            mp4->sizes[i] = fixed_size;
        return 0;
    }

    {
        uint64_t pos = start + 12;
        uint32_t remaining = count;

        while (remaining > 0) {
            uint8_t chunk[512];
            uint32_t want = remaining * 4;
            uint32_t n;
            uint32_t j;

            if (want > sizeof(chunk))
                want = (uint32_t)(sizeof(chunk) / 4) * 4;
            n = (uint32_t)read_at(mp4->fd, pos, chunk, want);
            if (n < 4)
                return -1;
            n &= ~3u;

            for (j = 0; j < n / 4; j++)
                mp4->sizes[i++] = be32(chunk + j * 4);
            pos += n;
            remaining -= n / 4;
        }
    }
    return 0;
}

struct stsc_entry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
};

/* Builds the per-sample offsets, which needs stsc and stco/co64 together. */
static int parse_chunk_offsets(vm_mp4_t *mp4, uint64_t start, uint64_t end,
                               int is64)
{
    uint8_t hdr[8];
    uint32_t entry_count;
    uint32_t i;
    uint32_t sample = 0;
    uint64_t pos;
    uint32_t stride = is64 ? 8 : 4;

    if (start + 8 > end || read_at(mp4->fd, start, hdr, 8) != 8)
        return -1;

    entry_count = be32(hdr + 4);
    if (entry_count == 0 || entry_count > 1000000u)
        return -1;

    pos = start + 8;
    for (i = 0; i < entry_count && sample < mp4->count; i++) {
        uint8_t raw[8];
        uint64_t chunk_offset;
        uint32_t samples_here;
        uint32_t s;

        if (read_at(mp4->fd, pos, raw, stride) != (int)stride)
            return -1;
        chunk_offset = is64 ? be64(raw) : (uint64_t)be32(raw);
        pos += stride;

        samples_here = mp4->samples_in_chunk[i];
        for (s = 0; s < samples_here && sample < mp4->count; s++) {
            mp4->offsets[sample] = chunk_offset;
            chunk_offset += mp4->sizes[sample];
            sample++;
        }
    }

    return sample == mp4->count ? 0 : -1;
}

/*
 * stsc maps chunks to sample counts; the offsets builder needs the count for
 * every chunk, so expand it into a flat array first.
 */
static int parse_stsc(vm_mp4_t *mp4, uint64_t start, uint64_t end,
                      uint32_t chunk_count)
{
    uint8_t hdr[8];
    uint32_t entry_count;
    struct stsc_entry *entries = NULL;
    uint32_t i;
    uint32_t chunk;
    int ret = -1;

    if (start + 8 > end || read_at(mp4->fd, start, hdr, 8) != 8)
        return -1;

    entry_count = be32(hdr + 4);
    if (entry_count == 0 || entry_count > 1000000u)
        return -1;

    entries = calloc(entry_count, sizeof(*entries));
    mp4->samples_in_chunk = calloc(chunk_count, sizeof(uint32_t));
    if (!entries || !mp4->samples_in_chunk)
        goto done;

    /* Entries are 12 big-endian bytes each; decoding them field by field keeps
     * this independent of the host's struct layout. */
    for (i = 0; i < entry_count; i++) {
        uint8_t raw[12];

        if (read_at(mp4->fd, start + 8 + (uint64_t)i * 12, raw, 12) != 12)
            goto done;
        entries[i].first_chunk = be32(raw);
        entries[i].samples_per_chunk = be32(raw + 4);
        if (entries[i].samples_per_chunk == 0)
            goto done;
    }

    /* Entries are in first_chunk order, so one forward pass is enough. */
    for (chunk = 0; chunk < chunk_count; chunk++) {
        uint32_t per_chunk = 0;

        for (i = 0; i < entry_count; i++) {
            if (entries[i].first_chunk <= chunk + 1)
                per_chunk = entries[i].samples_per_chunk;
            else
                break;
        }
        if (per_chunk == 0) {
            vm_log("mp4: no stsc entry covers chunk %u\n", chunk + 1);
            goto done;
        }
        mp4->samples_in_chunk[chunk] = per_chunk;
    }

    ret = 0;

done:
    free(entries);
    return ret;
}

static int parse_co64_or_stco(vm_mp4_t *mp4, uint64_t start, uint64_t end,
                              int is64)
{
    uint8_t hdr[8];
    uint32_t chunk_count;

    if (start + 8 > end || read_at(mp4->fd, start, hdr, 8) != 8)
        return -1;

    chunk_count = be32(hdr + 4);
    if (chunk_count == 0 || chunk_count > 2000000u)
        return -1;

    if (parse_stsc(mp4, mp4->stsc_start, mp4->stsc_end, chunk_count) != 0)
        return -1;

    mp4->offsets = calloc(mp4->count, sizeof(uint64_t));
    if (!mp4->offsets)
        return -1;

    return parse_chunk_offsets(mp4, start, end, is64);
}

static void parse_mdhd(vm_mp4_t *mp4, uint64_t start, uint64_t end)
{
    uint8_t hdr[4];
    uint8_t body[32];
    uint8_t version;

    if (start + 4 > end || read_at(mp4->fd, start, hdr, 4) != 4)
        return;
    version = hdr[0];

    if (version == 1) {
        uint32_t timescale;
        uint64_t duration;

        if (start + 4 + 28 > end || read_at(mp4->fd, start + 4, body, 28) != 28)
            return;
        timescale = be32(body + 16);
        duration = be64(body + 20);
        if (timescale)
            mp4->duration_ms =
                (uint32_t)((duration * 1000ULL) / timescale);
    } else {
        uint32_t timescale;
        uint32_t duration;

        if (start + 4 + 16 > end || read_at(mp4->fd, start + 4, body, 16) != 16)
            return;
        timescale = be32(body + 8);
        duration = be32(body + 12);
        if (timescale)
            mp4->duration_ms =
                (uint32_t)(((uint64_t)duration * 1000ULL) / timescale);
    }
}

/* Reads the mp4a sample entry in an stsd and pulls out the decoder config. */
static int parse_stsd(vm_mp4_t *mp4, uint64_t start, uint64_t end)
{
    uint8_t hdr[8];
    uint32_t entry_count;
    uint64_t pos;

    if (start + 8 > end || read_at(mp4->fd, start, hdr, 8) != 8)
        return -1;

    entry_count = be32(hdr + 4);
    pos = start + 8;

    while (entry_count-- > 0 && pos + 8 <= end) {
        uint8_t entry[36];
        uint32_t size;
        uint64_t payload;
        uint64_t payload_end;

        if (read_at(mp4->fd, pos, entry, 8) != 8)
            return -1;
        size = be32(entry);
        if (size < 8 || pos + size > end)
            return -1;
        payload = pos + 8;
        payload_end = pos + size;

        if (memcmp(entry + 4, "mp4a", 4) == 0) {
            uint8_t sample_entry[36];
            uint32_t version;
            uint16_t channels;
            uint32_t rate;

            if (read_at(mp4->fd, payload, sample_entry, 28) != 28)
                return -1;

            version = be16(sample_entry + 8);
            channels = be16(sample_entry + 16);
            /* The last field is 16.16 fixed point; the integer half is the
             * sample rate. */
            rate = be32(sample_entry + 24) >> 16;

            if (channels > 0)
                mp4->channels = channels;
            if (rate > 0)
                mp4->sample_rate = (int)rate;

            {
                uint64_t child = payload + 28;
                uint64_t child_end = payload_end;

                if (version == 1)
                    child += 16;
                else if (version == 2)
                    child += 36;

                {
                    uint64_t esds_p, esds_e;
                    if (child < child_end &&
                        find_box(mp4->fd, child, child_end, "esds", &esds_p,
                                 &esds_e) == 0) {
                        uint8_t buf[512];
                        uint32_t len;
                        uint32_t got;

                        /* Skip the version/flags word in front of the
                         * descriptors. */
                        len = (uint32_t)(esds_e - esds_p);
                        if (len > 4) {
                            got = (uint32_t)read_at(mp4->fd, esds_p + 4, buf,
                                                    (uint32_t)sizeof(buf));
                            if (got > len - 4)
                                got = len - 4;
                            if (got > 0)
                                parse_esds(mp4, buf, got);
                        }
                    }
                }
            }

            /* esds carries the authoritative values, so prefer them. */
            if (mp4->frame_samples == 0)
                mp4->frame_samples = 1024;
            return 0;
        }

        pos = payload_end;
    }

    return -1;
}

/* -------------------------------------------------------------------------- */
/* ADTS (the HLS audio playlist serves bare ADTS frames, not MP4)             */
/* -------------------------------------------------------------------------- */

static const uint32_t adts_sf_table[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000,
    22050, 16000, 12000, 11025, 8000,  7350
};

/* Validates a 7 byte ADTS header at `p`; on success reports the frame
 * length (header included), the sampling rate index, the channel
 * configuration and the profile. Returns 1 when the header is plausible. */
static int adts_probe(const uint8_t *p, uint32_t remaining, uint16_t *frame_len,
                      uint8_t *sf_index, uint8_t *chcfg, uint8_t *profile)
{
    uint16_t len;
    uint8_t sf;

    if (p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return 0;
    sf = (p[2] >> 2) & 0x0F;
    if (sf > 12)
        return 0;
    len = (uint16_t)(((uint16_t)(p[3] & 0x03) << 11) | ((uint16_t)p[4] << 3) |
                     ((uint16_t)p[5] >> 5));
    if (len < 8 || (uint32_t)len > remaining)
        return 0;
    *frame_len = len;
    *sf_index = sf;
    *chcfg = (uint8_t)(((p[2] & 0x01) << 2) | ((p[3] >> 6) & 0x03));
    *profile = (p[2] >> 6) & 0x03;
    return 1;
}

/*
 * Tries to interpret `path` as a raw ADTS AAC stream (what the HLS audio
 * playlist episodes concatenate into). When it succeeds the sample table is
 * filled in pointing at the raw AAC payload of every frame, so the shared
 * read/sample-time helpers work unchanged. Returns 0 on success; on failure
 * the vm_mp4_t is left untouched so the MP4 path can still run.
 */
static int mp4_adts_open(vm_mp4_t *mp4, const char *path)
{
    struct a_frame {
        uint64_t payload_off;
        uint32_t payload_len;
        uint8_t sf, ch, prof;
    } *frames = NULL;
    uint32_t fcap = 0, fcount = 0;
    uint8_t *whole;
    uint64_t pos, n_read;
    uint8_t gate[2048];
    uint32_t i;
    uint32_t total_len = 0;

    /* Cheap gate before buffering the whole file: a plain MP4 starts with a
     * box header, HLS audio starts with an ID3 tag and/or an ADTS sync. */
    n_read = mp4->file_size < (uint64_t)sizeof(gate) ? mp4->file_size
                                                     : (uint64_t)sizeof(gate);
    if (n_read < 8)
        return -1;
    if (read_at(mp4->fd, 0, gate, (uint32_t)n_read) != (int)n_read)
        return -1;
    for (i = 0; i + 7 < n_read; i++) {
        uint16_t fl;
        uint8_t sf, ch, pr;
        if (adts_probe(gate + i, (uint32_t)n_read - i, &fl, &sf, &ch, &pr) == 1)
            break;
    }
    if (i + 7 >= n_read)
        return -1;

    /* Too little data yet: skip the full-file scan so the player can retry
     * cheaply while the download fills the first frames. */
    if (mp4->file_size < 2048)
        return -1;

    whole = malloc((size_t)mp4->file_size);
    if (!whole)
        return -1;
    if (read_at(mp4->fd, 0, whole, (uint32_t)mp4->file_size) !=
        (int)mp4->file_size) {
        free(whole);
        return -1;
    }

    /* Chain walk: a valid ADTS stream is one unbroken run of headers where
     * every frame's declared length lands exactly on the next sync. A byte
     * that fails the probe (ID3 tags between segments, transfer damage) is
     * never treated as audio: the walk resynchronizes on the next header that
     * itself lands on another valid header, so damaged spots can neither
     * inject phantom frames (the glitch source) nor poison the whole table
     * (which made perfectly good cached replays unplayable). */
    pos = i;
    while (pos + 7 < mp4->file_size) {
        uint16_t fl;
        uint8_t sf, ch, pr;

        if (adts_probe(whole + pos, (uint32_t)(mp4->file_size - pos), &fl,
                       &sf, &ch, &pr) == 1) {
            if (fcount == fcap) {
                uint32_t ncap = fcap ? fcap * 2 : 512;
                struct a_frame *nf =
                    realloc(frames, ncap * sizeof(*frames));
                if (!nf) {
                    free(whole);
                    free(frames);
                    return -1;
                }
                frames = nf;
                fcap = ncap;
            }
            frames[fcount].payload_off = pos;
            frames[fcount].payload_len = (uint32_t)fl;
            frames[fcount].sf = sf;
            frames[fcount].ch = ch;
            frames[fcount].prof = pr;
            fcount++;
            total_len += (uint32_t)fl;
            pos += fl;
        } else {
            uint64_t scan;
            int found = 0;

            for (scan = pos + 1;
                 scan < pos + 8192 && scan + 7 < mp4->file_size; scan++) {
                uint16_t fl2, fl3;
                uint8_t sf2, ch2, pr2, sf3, ch3, pr3;

                if (!adts_probe(whole + scan,
                                  (uint32_t)(mp4->file_size - scan), &fl2,
                                  &sf2, &ch2, &pr2))
                    continue;
                /* The resync candidate must itself chain to the frame after
                 * it; that look-ahead is what keeps stray lookalikes out. */
                if (scan + fl2 + 7 >= mp4->file_size ||
                    !adts_probe(whole + scan + fl2,
                                  (uint32_t)(mp4->file_size - scan - fl2),
                                  &fl3, &sf3, &ch3, &pr3))
                    continue;
                pos = scan;
                found = 1;
                break;
            }
            if (!found)
                break;
        }
    }
    free(whole);

    /* Start playback as soon as a handful of frames exist; the player
     * re-opens the growing file to pick up the rest. The coverage check
     * only rejects binary garbage that happens to look like ADTS syncs —
     * incomplete trailing bytes of a live download are fine. */
    if (fcount < 4)
        goto fail;
    if (mp4->file_size > 4096 && total_len < mp4->file_size / 8)
        goto fail;

    /* Install the walked frames as the sample table. */
    {
        uint32_t n = fcount;
        uint32_t j;

        mp4->count = n;
        mp4->sizes = malloc(n * sizeof(uint32_t));
        mp4->offsets = malloc(n * sizeof(uint64_t));
        if (!mp4->sizes || !mp4->offsets)
            goto fail_partial;
        for (j = 0; j < n; j++) {
            mp4->sizes[j] = frames[j].payload_len;
            mp4->offsets[j] = frames[j].payload_off;
        }
        mp4->sample_rate = adts_sf_table[frames[0].sf];
        mp4->channels = frames[0].ch ? frames[0].ch : 2;
        mp4->frame_samples = 1024;
        mp4->sbr = 0;
        mp4->is_adts = 1;
        mp4->adts_strict = 1;
        mp4->duration_ms =
            (uint32_t)(((uint64_t)mp4->count * 1024ULL * 1000ULL) /
                       (uint64_t)mp4->sample_rate);
        vm_log("mp4: %u ADTS samples, %d Hz, %d ch\n", mp4->count,
                 mp4->sample_rate, mp4->channels);
        free(frames);
        return 0;
    }

fail_partial:
    free(mp4->sizes);
    free(mp4->offsets);
    mp4->sizes = NULL;
    mp4->offsets = NULL;
    mp4->count = 0;
fail:
    free(frames);
    return -1;
}

/* -------------------------------------------------------------------------- */
/* public API                                                                 */
/* -------------------------------------------------------------------------- */

/* True when the leading bytes look like HLS/ADTS audio (ID3 and/or ADTS
 * sync) rather than an MP4 box. Used to skip the moov walk + log spam while
 * a progressive download has not gathered enough frames yet. */
static int file_looks_like_adts(int fd, uint64_t file_size)
{
    uint8_t gate[512];
    uint64_t n_read;
    uint32_t i;

    n_read = file_size < (uint64_t)sizeof(gate) ? file_size
                                                : (uint64_t)sizeof(gate);
    if (n_read < 8)
        return 0;
    if (read_at(fd, 0, gate, (uint32_t)n_read) != (int)n_read)
        return 0;
    if (gate[0] == 'I' && gate[1] == 'D' && gate[2] == '3')
        return 1;
    for (i = 0; i + 7 < n_read; i++) {
        uint16_t fl;
        uint8_t sf, ch, pr;

        if (adts_probe(gate + i, (uint32_t)n_read - i, &fl, &sf, &ch, &pr))
            return 1;
    }
    return 0;
}

int vm_mp4_open(vm_mp4_t *mp4, const char *path)
{
    SceIoStat st;
    uint64_t moov_p, moov_e;
    uint64_t pos;
    int found = -1;
    int adts_like = 0;

    memset(mp4, 0, sizeof(*mp4));
    mp4->fd = -1;
    mp4->channels = 2;
    mp4->sample_rate = 44100;
    mp4->frame_samples = 1024;

    mp4->fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (mp4->fd < 0)
        return -1;

    if (sceIoGetstat(path, &st) < 0) {
        vm_mp4_close(mp4);
        return -1;
    }
    mp4->file_size = (uint64_t)st.st_size;

    if (mp4_adts_open(mp4, path) == 0)
        return 0;

    adts_like = file_looks_like_adts(mp4->fd, mp4->file_size);

    /* Growing HLS/ADTS files are not MP4s; retry later without flooding the
     * log or walking for a moov that will never appear. */
    if (adts_like) {
        vm_mp4_close(mp4);
        return -1;
    }

    /* A stream that starts with an ID3 tag or a stray byte would throw the box
     * walk off, so locate the first plausible box first. */
    pos = 0;
    while (pos + 8 <= mp4->file_size) {
        char name[5];
        uint64_t pl, pe, next;

        if (box_at(mp4->fd, pos, mp4->file_size, name, &pl, &pe, &next) != 0)
            break;
        if (strcmp(name, "moov") == 0) {
            moov_p = pl;
            moov_e = pe;
            found = 0;
            break;
        }
        if (next <= pos)
            break;
        pos = next;
    }

    if (found != 0) {
        /* Only log once the file is large enough that a moov should have
         * shown up; during progressive DASH the header is still arriving, and
         * HLS files legitimately have no moov at all. */
        if (mp4->file_size >= 1048576)
            vm_log("mp4: no moov box in %s\n", path);
        vm_mp4_close(mp4);
        return -1;
    }

    /* Pick the first trak whose stsd holds an mp4a sample entry. */
    pos = moov_p;
    while (pos + 8 <= moov_e) {
        char name[5];
        uint64_t pl, pe, next;
        uint64_t mdia_p, mdia_e, minf_p, minf_e, stbl_p, stbl_e;

        if (box_at(mp4->fd, pos, moov_e, name, &pl, &pe, &next) != 0)
            break;

        if (strcmp(name, "trak") == 0 &&
            find_box(mp4->fd, pl, pe, "mdia", &mdia_p, &mdia_e) == 0 &&
            find_box(mp4->fd, mdia_p, mdia_e, "minf", &minf_p, &minf_e) == 0 &&
            find_box(mp4->fd, minf_p, minf_e, "stbl", &stbl_p, &stbl_e) == 0) {
            uint64_t stsd_p, stsd_e, stsz_p, stsz_e, stsc_p, stsc_e;
            uint64_t stco_p, stco_e, co64_p, co64_e, mdhd_p, mdhd_e;

            if (find_box(mp4->fd, stbl_p, stbl_e, "stsd", &stsd_p, &stsd_e) == 0 &&
                parse_stsd(mp4, stsd_p, stsd_e) == 0) {
                if (find_box(mp4->fd, stbl_p, stbl_e, "stsz", &stsz_p,
                             &stsz_e) != 0 ||
                    parse_stsz(mp4, stsz_p, stsz_e) != 0) {
                    vm_log("mp4: sample size table unreadable\n");
                    vm_mp4_close(mp4);
                    return -1;
                }

                if (find_box(mp4->fd, stbl_p, stbl_e, "stsc", &stsc_p,
                             &stsc_e) != 0) {
                    vm_log("mp4: no stsc\n");
                    vm_mp4_close(mp4);
                    return -1;
                }
                mp4->stsc_start = stsc_p;
                mp4->stsc_end = stsc_e;

                if (find_box(mp4->fd, stbl_p, stbl_e, "co64", &co64_p,
                             &co64_e) == 0)
                    found = parse_co64_or_stco(mp4, co64_p, co64_e, 1);
                else if (find_box(mp4->fd, stbl_p, stbl_e, "stco", &stco_p,
                                  &stco_e) == 0)
                    found = parse_co64_or_stco(mp4, stco_p, stco_e, 0);
                else
                    found = -1;

                if (found != 0) {
                    vm_log("mp4: chunk offset table unreadable\n");
                    vm_mp4_close(mp4);
                    return -1;
                }

                if (find_box(mp4->fd, mdia_p, mdia_e, "mdhd", &mdhd_p,
                             &mdhd_e) == 0)
                    parse_mdhd(mp4, mdhd_p, mdhd_e);

                vm_log("mp4: %u AAC samples, %d Hz, %d ch, aot %d%s\n",
                         mp4->count, mp4->sample_rate, mp4->channels,
                         mp4->object_type, mp4->sbr ? " (SBR)" : "");
                return 0;
            }
        }

        if (next <= pos)
            break;
        pos = next;
    }

    vm_log("mp4: no AAC track found in %s\n", path);
    vm_mp4_close(mp4);
    return -1;
}

void vm_mp4_close(vm_mp4_t *mp4)
{
    if (mp4->fd >= 0)
        sceIoClose(mp4->fd);
    free(mp4->sizes);
    free(mp4->offsets);
    free(mp4->samples_in_chunk);
    mp4->fd = -1;
    mp4->sizes = NULL;
    mp4->offsets = NULL;
    mp4->samples_in_chunk = NULL;
    mp4->count = 0;
}

int vm_mp4_read_sample(vm_mp4_t *mp4, uint32_t index, uint8_t *buf,
                         uint32_t cap)
{
    uint32_t size;

    if (index >= mp4->count)
        return -1;
    size = mp4->sizes[index];
    if (size > cap)
        return -1;
    return read_at(mp4->fd, mp4->offsets[index], buf, size);
}

uint32_t vm_mp4_sample_time_ms(const vm_mp4_t *mp4, uint32_t index)
{
    uint32_t per_frame = mp4->frame_samples ? mp4->frame_samples : 1024;

    if (mp4->sample_rate <= 0)
        return 0;
    return (uint32_t)(((uint64_t)index * per_frame * 1000ULL) /
                      (uint64_t)mp4->sample_rate);
}

uint32_t vm_mp4_time_to_sample(const vm_mp4_t *mp4, uint32_t ms)
{
    uint32_t per_frame = mp4->frame_samples ? mp4->frame_samples : 1024;
    uint64_t frames;

    if (mp4->sample_rate <= 0)
        return 0;
    frames = ((uint64_t)ms * (uint64_t)mp4->sample_rate) /
             (1000ULL * per_frame);
    if (frames >= mp4->count)
        frames = mp4->count ? mp4->count - 1 : 0;
    return (uint32_t)frames;
}
