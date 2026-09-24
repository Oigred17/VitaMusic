# VitaMusic

A native YouTube Music client for the PlayStation Vita, written in C.

VitaMusic streams audio from YouTube Music over Wi-Fi, caches what it plays for
instant replays and offline listening, and renders a full touch + button UI on
top of vita2d. Audio decoding runs on the console's hardware AAC codec, so
playback costs almost no CPU and leaves the screen buttery smooth.

---

## Features

- **Search** YouTube Music by song, artist or album (system keyboard, touch or
  physical buttons).
- **Hardware AAC playback** through `SceAudiodec` — MP4 (DASH) and ADTS (HLS)
  streams, resampled only when the source rate is one the output port cannot
  take natively.
- **Stream-while-downloading**: playback starts as soon as the first AAC frames
  land, while the rest of the track keeps filling the cache file in the
  background.
- **Deep audio pipeline**: an ~11-second decoded backlog rides out Wi-Fi
  stalls, and every track is fully drained before the queue advances — no cut
  endings, no phantom skips.
- **Smart cache**: completed downloads are promoted to a durable cache and
  replayed without touching the network; prefetch warms the next queued track
  when the current one is fully buffered.
- **Saved playlists**: store the current queue under a name, replay it, delete
  it. Persisted in a plain-text file you can edit from VitaShell.
- **Favourites & history** with automatic persistence.
- **Now Playing screen**: large left-anchored album art, transport row with a
  modern repeat button (off / all / one), seekable progress bar with download
  buffer underlay, favourites, queue, download and lyrics at one tap.
- **Synced lyrics** when the track has timed lines; manual scroll otherwise.
- **Queue management** with shuffle and repeat (off / all / one).
- **LiveArea** assets, boot progress HUD and a log file at
  `ux0:data/VitaMusic/vitamusic.log` for diagnostics.
- **Background-safe playback**: CPU/BUS/GPU clocks are pinned to their maximum
  and re-asserted periodically, so audio keeps filling its buffer even with
  the screen off (the OS otherwise throttles into a power-saving profile).

## Installation

1. Grab the latest `VitaMusic.vpk`.
2. Copy it to your Vita (USB or FTP with VitaShell).
3. Install it from VitaShell. Your library and cache live in `ux0:data/VitaMusic`
   and survive reinstalls.

A PlayStation Vita (or PS TV) with firmware 3.60+ / HENkaku-compatible
custom firmware is required. Wi-Fi must be enabled before starting the app.

## Building

The build needs [vitasdk](https://vitasdk.org) with `vita2d` and libcurl
(available via `vdpm`), plus CMake ≥ 3.16 and Ninja.

```sh
export VITASDK=/usr/local/vitasdk   # your toolchain path
git clone <repo-url> VitaMusic
cd VitaMusic
cmake -B build -G Ninja
cmake --build build
```

The result is `build/VitaMusic.vpk`.

Every push to `main` and every release tag (`v*`) is built on GitHub Actions
using the official `vitasdk/vitasdk` Docker image; the `.vpk` is attached to
the matching release automatically.

## Controls

| Action                | Button / touch                              |
| --------------------- | ------------------------------------------- |
| Navigate lists        | D-pad / left stick / touch                  |
| Play item             | Cross or tap                                |
| Search                | Square on results, or tap the search bar    |
| Play / pause          | Cross on Now Playing                        |
| Prev / next           | Left / right d-pad, transport buttons       |
| Seek ±10 s            | L / R shoulder on Now Playing               |
| Repeat mode           | Triangle on Now Playing, or the repeat icon |
| Save queue as playlist| Square on the Playlists screen              |
| Delete playlist       | Triangle on the Playlists screen            |
| Download for offline  | Select (lists) or Square (Now Playing)      |
| Favourite             | Triangle (lists)                            |
| Tabs                  | L / R shoulder, or tap the tab strip        |
| Back                  | Circle                                      |

## Architecture

```
src/
  main.c        bootstrap: sysmodules, vita2d, main loop
  app.c         application state, worker thread, persistence (TSV files)
  ui.c          vita2d front end: lists, now-playing, lyrics, IME search
  player.c      playback thread: resolve → download → decode → output
  innertube.c   YouTube Music InnerTube API client (search + stream resolve)
  net.c         libcurl wrapper, HLS downloader, TLS/CA handling
  mp4.c         minimal MP4 (moov/stbl) and ADTS stream parser
  decoder.c     SceAudiodec AAC decoder wrapper
  audio.c       BGM output port, grain FIFO, volume
  lyrics.c      timed-lyrics lookup
  util.c        logging, strings, filesystem helpers
  json.c        small JSON parser for the API replies
```

### Playback pipeline

1. **Resolve** — the player thread asks InnerTube for a stream URL (multiple
   client identities are tried in order for reliability).
2. **Download** — a dedicated thread streams the audio (direct DASH URL or an
   HLS segment walk) into `cache/<id>.m4a.part`.
3. **Decode** — the player opens the growing file as soon as it is parseable,
   gates every AAC frame on its bytes having landed, and feeds the hardware
   decoder.
4. **Output** — decoded PCM lands in a deep FIFO; a blocking grain drain paces
   decode against real time and keeps a multi-second backlog so network stalls
   never reach the speaker. When the transfer completes, the file is promoted
   to `cache/<id>.m4a` and future plays skip the network entirely.

### Data layout

| Path                              | Content                        |
| --------------------------------- | ------------------------------ |
| `ux0:data/VitaMusic/favorites.tsv`| Favourite tracks               |
| `ux0:data/VitaMusic/history.tsv`  | Recently played                |
| `ux0:data/VitaMusic/playlists.txt`| Saved playlists (named queues) |
| `ux0:data/VitaMusic/settings.cfg` | Volume, repeat, shuffle        |
| `ux0:data/VitaMusic/cache/`       | Cached audio streams + artwork |
| `ux0:data/VitaMusic/vitamusic.log`| Diagnostic log                 |

All files are plain text or standard JPEG/MP4, repairable and cleanable from
VitaShell. *Clear cache* in Settings removes cached audio and artwork.

## Known limitations

- Audio only (no video), AAC streams only — what YouTube Music serves.
- Track resolution occasionally needs a retry on very unstable networks; a
  failed resolve is surfaced in the Now Playing screen and the log.
- The queue holds up to 60 tracks and up to 8 playlists can be saved.

## Disclaimer

This project is not affiliated with, endorsed by, or connected to Google or
YouTube in any way. It is a personal homebrew client and does not bypass any
technical protection: playback uses the same public endpoints any browser
uses. Respect YouTube's Terms of Service and the copyright laws that apply to
you when using it.

## License

Licensed under the [GNU General Public License v3.0](LICENSE).
