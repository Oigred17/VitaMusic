/*
 * VitaMusic - lyrics from lrclib.net.
 *
 * Lyrics text lives in one blob with (offset, length) pairs rather than in a
 * few hundred fixed size strings: the whole container then stays small enough
 * that the app can keep two of them for double buffering without noticing.
 */
#ifndef VM_LYRICS_H
#define VM_LYRICS_H

#include <stdint.h>

#include "config.h"
#include "innertube.h"

typedef struct {
    uint32_t time_ms;
    uint16_t offset; /* into vm_lyrics_t.text */
    uint16_t length;
} vm_lyric_line_t;

typedef struct {
    vm_lyric_line_t lines[VM_MAX_LYRIC_LINES];
    int count;
    int synced; /* timestamps were present */
    int valid;  /* the container holds a finished lookup */
    char text[VM_LYRIC_BLOB];
} vm_lyrics_t;

/* Looks `track` up on lrclib. Returns 0 when lyrics were found. */
int lyrics_fetch(const vm_track_t *track, vm_lyrics_t *out);

/* Index of the line to highlight at `position_ms`, or -1. */
int lyrics_line_at(const vm_lyrics_t *lyrics, uint32_t position_ms);

#endif /* VM_LYRICS_H */
