/*
 * VitaMusic - central configuration.
 */
#ifndef VM_CONFIG_H
#define VM_CONFIG_H

#define VM_APP_NAME    "VitaMusic"
#define VM_APP_VERSION "01.00"

/* Everything the app writes lives under ux0:data so that a reinstall of the
 * VPK never touches the user's library or cache. */
#define VM_DATA_DIR   "ux0:data/VitaMusic"
#define VM_CACHE_DIR  "ux0:data/VitaMusic/cache"
#define VM_THUMB_DIR  VM_CACHE_DIR "/thumbs"
#define VM_LOG_FILE   "ux0:data/VitaMusic/vitamusic.log"
/* Downloaded on first run from CA_BUNDLE_URL, then used for certificate
 * verification of every other request. */
#define VM_CA_FILE    "ux0:data/VitaMusic/cacert.pem"
#define VM_CA_URL     "https://curl.se/ca/cacert.pem"

/* Small text databases. All of them are plain TSV/cfg so they stay readable
 * and repairable from VitaShell without a companion tool. */
#define VM_FAV_FILE   VM_DATA_DIR "/favorites.tsv"
#define VM_HIST_FILE  VM_DATA_DIR "/history.tsv"
#define VM_CFG_FILE   VM_DATA_DIR "/settings.cfg"
#define VM_VISITOR_FILE VM_DATA_DIR "/visitor.txt"
#define VM_ACCOUNT_FILE VM_DATA_DIR "/account.txt"
#define VM_PLAYLISTS_FILE VM_DATA_DIR "/playlists.txt"
/* Saved playlists are named copies of the playback queue. */
#define VM_MAX_PLAYLISTS 8
#define VM_MAX_PLAYLIST_NAME 48

/* Innertube endpoints and client identity.
 *
 * Search runs as WEB_REMIX on music.youtube.com. Player requests go to
 * www.youtube.com with the JS-less clients yt-dlp still trusts in late 2026:
 *
 *  which 0 = ANDROID   – HLS audio without a PO token (HTTPS needs one)
 *  which 1 = VISIONOS  – still PO-free; tried first when it answers OK
 *  which 2 = WEB_EMBEDDED – progressive itag 18 (muxed AAC) for embeddable
 *                           tracks when the others return LOGIN_REQUIRED
 *
 * ANDROID_VR and IOS were dropped: VR is 403/LOGIN across the board, and IOS
 * now returns signatureCipher-only adaptive formats without a JS runtime.
 * A visitor id (X-Goog-Visitor-Id) is fetched once at startup. */
#define YTM_ORIGIN           "https://music.youtube.com"
#define YTM_API_URL          YTM_ORIGIN "/youtubei/v1/"
#define YT_WWW_ORIGIN        "https://www.youtube.com"
#define YT_WWW_API_URL       YT_WWW_ORIGIN "/youtubei/v1/"
#define YTM_API_KEY          "AIzaSyC9XL3ZjWddXya6X74dJoCTL-WEYFDNX3"
#define YTM_WEB_CLIENT_NAME  "WEB_REMIX"
#define YTM_WEB_VERSION      "1.20260707.12.00"
#define YTM_WEB_CLIENT_ID    "67"

#define YT_PLAYER_CLIENT_NAME    "ANDROID"
#define YT_PLAYER_CLIENT_VERSION "21.26.364"
#define YT_PLAYER_CLIENT_ID      "3"
#define YT_PLAYER_USER_AGENT \
    "com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip"
#define YT_PLAYER_OS_NAME    "Android"
#define YT_PLAYER_OS_VERSION "11"
#define YT_PLAYER_SDK        30

#define YT_FALLBACK_CLIENT_NAME    "VISIONOS"
#define YT_FALLBACK_CLIENT_VERSION "1.02"
#define YT_FALLBACK_CLIENT_ID      "101"
#define YT_FALLBACK_USER_AGENT \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"
#define YT_FALLBACK_OS_NAME    "visionOS"
#define YT_FALLBACK_OS_VERSION "26.5.23O471"
#define YT_FALLBACK_DEVICE_MAKE   "Apple"
#define YT_FALLBACK_DEVICE_MODEL  "RealityDevice17,1"

#define YT_FALLBACK2_CLIENT_NAME    "WEB_EMBEDDED_PLAYER"
#define YT_FALLBACK2_CLIENT_VERSION "2.20260708.00.00"
#define YT_FALLBACK2_CLIENT_ID      "56"
#define YT_FALLBACK2_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/127.0.0.0 Safari/537.36"
#define YT_FALLBACK2_EMBED_URL "https://www.reddit.com/"

/* WEB_REMIX reports a desktop Firefox user agent; matching it keeps the
 * search response shape stable. */
#define YTM_WEB_USER_AGENT \
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:140.0) Gecko/20100101 Firefox/140.0"

/* Sent with every non-YouTube request (lrclib, thumbnails, CA bundle). */
#define VM_USER_AGENT "VitaMusic/" VM_APP_VERSION " (PS Vita homebrew)"

/* Parameter that restricts an Innertube search to the "Songs" shelf. */
#define YTM_PARAM_SONGS "EgWKAQIIAWoKEAkQBRAKEAMQBA%3D%3D"

#define VM_MAX_RESULTS 40
#define VM_MAX_LYRIC_LINES 400
/* Lyrics are kept in one blob with offsets rather than in 400 fixed strings,
 * which keeps the two swap buffers small enough to be cheap to copy. */
#define VM_LYRIC_BLOB (64 * 1024)

/* Playlists / library sizes. These are fixed arrays in the app state, so the
 * total stays well under a megabyte of BSS. */
#define VM_MAX_QUEUE   60
#define VM_MAX_LIBRARY 100
#define VM_MAX_HISTORY 60

/* Audio output.
 *
 * sceAudioOut only accepts 8000/11025/12000/16000/22050/24000/32000/44100/
 * 48000 Hz. The port is opened at the stream's own rate (AAC is almost always
 * 44.1 kHz) and only a rate outside that list is resampled to VM_OUT_RATE.
 *
 * VM_OUT_GRAIN is the number of frames handed to sceAudioOutOutput per call:
 * a multiple of 64 (SCE_AUDIO_MIN_LEN rounds there) and no more than 65472.
 * 1024 frames is exactly one AAC frame at 44.1 kHz. */
#define VM_OUT_RATE     44100
#define VM_OUT_GRAIN    1024
#define VM_OUT_VOLUME_MAX 32768

/* Album art. Thumbnails are requested at this size on both axes and cached as
 * JPEG under VM_CACHE_DIR/thumbs. */
#define VM_THUMB_PX 300
/* Number of decoded textures kept resident. 300x300 RGBA is ~350 KB each. */
#define VM_MAX_THUMBS 12

/* --- HTTP ------------------------------------------------------------------ */
#define VM_HTTP_CONNECT_TIMEOUT 10L  /* seconds */
#define VM_HTTP_TIMEOUT         30L  /* seconds, for API calls */
/* A stalled stream is aborted when it stays under this rate for this long.
 * Downloads get no total timeout: a 10 MB track over a weak Wi-Fi connection
 * legitimately takes minutes. */
#define VM_HTTP_LOW_SPEED      512L
#define VM_HTTP_LOW_SPEED_TIME 25L
#define VM_JSON_MAX_BYTES (8 * 1024 * 1024)
/* A cached stream smaller than this is treated as a broken leftover. */
#define VM_STREAM_MIN_BYTES 65536L
/* A CA bundle smaller than this cannot be a full Mozilla bundle; treat it as
 * a truncated download and fetch it again. */
#define VM_CA_MIN_BYTES 65536L

/* Network buffer handed to sceNetInit. libcurl allocates its own socket
 * buffers on top of this. */
#define VM_NET_POOL_BYTES (1 * 1024 * 1024)

#endif /* VM_CONFIG_H */
