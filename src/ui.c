/*
 * VitaMusic - vita2d front end.
 *
 * List-driven UI with the system IME for search, a library hub (favourites +
 * account), and a now-playing screen with geometric transport icons.
 */
#include "ui.h"
#include "app.h"
#include "config.h"
#include "player.h"
#include "util.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/libime.h>
#include <psp2/sysmodule.h>
#include <psp2/touch.h>
#include <vita2d.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* -------------------------------------------------------------------------- */
/* layout                                                                     */
/* -------------------------------------------------------------------------- */

#define SCREEN_W     960
#define SCREEN_H     544

#define PAD          16
/* No header above the tabs: the tab strip is the topmost element. */
#define TAB_H        44
#define TAB_TOP      0
#define TAB_BOTTOM   (TAB_TOP + TAB_H)
#define FOOTER_H     40
#define LIST_TOP     (TAB_BOTTOM + 12)
#define LIST_BOTTOM  (SCREEN_H - FOOTER_H)
#define ROW_H        72
#define ART_SZ       48


/* -------------------------------------------------------------------------- */
/* palette                                                                    */
/* -------------------------------------------------------------------------- */

#define C_BG       RGBA8(0x08, 0x0b, 0x10, 0xff)
#define C_PANEL    RGBA8(0x14, 0x1a, 0x22, 0xff)
#define C_PANEL_HI RGBA8(0x1e, 0x28, 0x34, 0xff)
#define C_BORDER   RGBA8(0x2a, 0x36, 0x44, 0xff)
#define C_TEXT     RGBA8(0xf0, 0xf3, 0xf7, 0xff)
#define C_DIM      RGBA8(0x8a, 0x96, 0xa8, 0xff)
#define C_ACCENT   RGBA8(0x1a, 0xb4, 0x9a, 0xff)
#define C_BLUE     C_ACCENT
#define C_GREEN    RGBA8(0x3d, 0xd6, 0x8c, 0xff)
#define C_RED      RGBA8(0xff, 0x6b, 0x7a, 0xff)
#define C_AMBER    RGBA8(0xff, 0xc2, 0x5c, 0xff)
#define C_BUF      RGBA8(0x2a, 0x3e, 0x48, 0xff)

/* -------------------------------------------------------------------------- */
/* text                                                                       */
/* -------------------------------------------------------------------------- */

static vita2d_pgf *g_pgf;
static vita2d_pvf *g_pvf;

static void ui_text(int x, int y, unsigned int color, float scale,
                    const char *text)
{
    if (!text)
        return;
    if (g_pgf)
        vita2d_pgf_draw_text(g_pgf, x, y, color, scale, text);
    else if (g_pvf)
        vita2d_pvf_draw_text(g_pvf, x, y, color, scale, text);
}

static void ui_text_center(int cx, int y, unsigned int color, float scale,
                           const char *text)
{
    int w;

    if (!text)
        return;
    if (g_pgf)
        w = vita2d_pgf_text_width(g_pgf, scale, text);
    else if (g_pvf)
        w = vita2d_pvf_text_width(g_pvf, scale, text);
    else
        return;
    ui_text(cx - w / 2, y, color, scale, text);
}

static int ui_text_width(float scale, const char *text)
{
    if (!text)
        return 0;
    if (g_pgf)
        return vita2d_pgf_text_width(g_pgf, scale, text);
    if (g_pvf)
        return vita2d_pvf_text_width(g_pvf, scale, text);
    return 0;
}

static int ui_text_height(float scale)
{
    if (g_pgf)
        return vita2d_pgf_text_height(g_pgf, scale, "Ay");
    if (g_pvf)
        return vita2d_pvf_text_height(g_pvf, scale, "Ay");
    return (int)(18.0f * scale);
}

static int ui_baseline_in_box(int box_y, int box_h, float scale)
{
    int th = ui_text_height(scale);
    return box_y + (box_h + th) / 2;
}

static void ui_text_box_center(int x, int y, int w, int h, unsigned int color,
                               float scale, const char *text)
{
    int tw = ui_text_width(scale, text);
    int bx = x + (w - tw) / 2;
    int by = ui_baseline_in_box(y, h, scale);
    ui_text(bx, by, color, scale, text);
}


void ui_hud_load(void)
{
    if (g_pgf || g_pvf)
        return;
    sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);

    g_pgf = vita2d_load_default_pgf();
    if (!g_pgf) {
        vm_log("ui: PGF unavailable, trying PVF\n");
        g_pvf = vita2d_load_default_pvf();
    }
    if (!g_pgf && !g_pvf)
        vm_log("ui: no system font available\n");
}

void ui_hud_show(const char *phase)
{
    char text[96];

    if (!phase)
        phase = "?";
    vm_log("ui: %s\n", phase);
    snprintf(text, sizeof(text), "VitaMusic %s", phase);
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, C_BG);
    ui_text_center(SCREEN_W / 2, SCREEN_H / 2 - 12, C_TEXT, 1.0f, text);
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* Draws as much of `text` as fits, with an ellipsis; never splits a UTF-8
 * sequence in half. */
static void ui_text_clip(int x, int y, int max_width, unsigned int color,
                         float scale, const char *text)
{
    char buffer[512];
    size_t len;
    size_t i = 0;
    int right = x + max_width;

    if (!text || max_width <= 0)
        return;
    for (len = 0; len < sizeof(buffer) - 4;) {
        const unsigned char *p = (const unsigned char *)(text + len);
        size_t n = 1;

        if (*p == '\0')
            break;
        if ((p[0] & 0xE0) == 0xC0)
            n = 2;
        else if ((p[0] & 0xF0) == 0xE0)
            n = 3;
        else if ((p[0] & 0xF8) == 0xF0)
            n = 4;
        if (len + n > sizeof(buffer) - 4)
            break;
        memcpy(buffer + len, p, n);
        len += n;
        buffer[len] = '\0';
        if (x + ui_text_width(scale, buffer) > right)
            break;
        i = len;
    }
    if (i < len) {
        int w;
        char cut[512];

        memcpy(cut, buffer, i);
        cut[i] = '\0';
        w = ui_text_width(scale, cut);
        if (w + ui_text_width(scale, "...") <= max_width) {
            strcat(cut, "...");
            ui_text(x, y, C_DIM, scale, cut);
            return;
        }
    }
    buffer[i] = '\0';
    ui_text(x, y, color, scale, buffer);
}

/* Rounded-corner rectangle: three stacked rects shrink the corners in. */
static void draw_rounded(int x, int y, int w, int h, unsigned int color)
{
    vita2d_draw_rectangle(x + 2, y, w - 4, h, color);
    vita2d_draw_rectangle(x + 1, y + 1, w - 2, h - 2, color);
    vita2d_draw_rectangle(x, y + 2, w, h - 4, color);
}

static void ui_toast(void)
{
    const char *text = app_toast_text();
    int width;

    if (!text)
        return;
    width = ui_text_width(0.85f, text);
    if (width > SCREEN_W - 2 * PAD)
        width = SCREEN_W - 2 * PAD;
    vita2d_draw_rectangle(SCREEN_W / 2 - width / 2 - 18, SCREEN_H - FOOTER_H - 52,
                          width + 36, 40, C_PANEL_HI);
    ui_text(SCREEN_W / 2 - width / 2, SCREEN_H - FOOTER_H - 28, C_TEXT, 0.85f,
            text);
}

static void ui_footer(const char *hint)
{
    vita2d_draw_rectangle(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, C_PANEL);
    vita2d_draw_rectangle(0, SCREEN_H - FOOTER_H, SCREEN_W, 2, C_BORDER);
    ui_text_box_center(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, C_DIM, 0.75f,
                       hint);
}

/* -------------------------------------------------------------------------- */
/* tab bar                                                                     */
/* -------------------------------------------------------------------------- */

typedef enum {
    TAB_HOME = 0,
    TAB_SEARCH,
    TAB_LIB,
    TAB_NOW,
    TAB_SETTINGS,
    TAB_COUNT
} ui_tab;

static const char *const g_tab_labels[TAB_COUNT] = {
    "Inicio", "Buscar", "Biblio", "Ahora", "Ajuste"
};

/* -------------------------------------------------------------------------- */
/* artwork (cached JPEG thumbnails, loaded when present)                       */
/* -------------------------------------------------------------------------- */

struct thumb_slot {
    char id[16];
    vita2d_texture *texture;
    int failed;
    int attempts;
    uint32_t used_at;
};

static struct thumb_slot g_thumbs[VM_MAX_THUMBS];
static uint32_t g_frame;
static uint32_t g_tick;
static int g_thumb_loads_left; /* budget JPEG decodes per frame */

static vita2d_texture *ui_thumb(const vm_track_t *track)
{
    int i;
    int slot = -1;
    uint32_t oldest = 0;

    if (!track || !track->id[0])
        return NULL;

    g_frame++;
    for (i = 0; i < VM_MAX_THUMBS; i++) {
        if (g_thumbs[i].id[0] && strcmp(g_thumbs[i].id, track->id) == 0) {
            g_thumbs[i].used_at = g_frame;
            if (g_thumbs[i].texture)
                return g_thumbs[i].texture;
            if (g_thumbs[i].failed && g_thumbs[i].attempts < 2) {
                const char *path;

                if (g_thumb_loads_left <= 0)
                    return NULL;
                path = app_thumb_file(track);
                if (path) {
                    g_thumb_loads_left--;
                    g_thumbs[i].attempts++;
                    g_thumbs[i].texture = vita2d_load_JPEG_file(path);
                    g_thumbs[i].failed = g_thumbs[i].texture == NULL;
                    vm_log("ui: thumb retry %s\n", track->id);
                    if (g_thumbs[i].failed)
                        app_thumb_invalidate(track);
                    else
                        return g_thumbs[i].texture;
                }
            }
            return NULL;
        }
    }

    {
        const char *path = app_thumb_file(track);

        if (!path)
            return NULL;
        /* Decode at most one new JPEG per frame so scrolling stays smooth. */
        if (g_thumb_loads_left <= 0)
            return NULL;

        for (i = 0; i < VM_MAX_THUMBS; i++) {
            if (!g_thumbs[i].id[0] || g_thumbs[i].failed) {
                slot = i;
                break;
            }
            if (slot < 0 || g_thumbs[i].used_at < oldest) {
                oldest = g_thumbs[i].used_at;
                slot = i;
            }
        }
        if (slot < 0)
            slot = 0;

        if (g_thumbs[slot].texture) {
            vita2d_free_texture(g_thumbs[slot].texture);
            g_thumbs[slot].texture = NULL;
        }
        vm_strlcpy(g_thumbs[slot].id, track->id,
                     sizeof(g_thumbs[slot].id));
        g_thumbs[slot].attempts = 0;
        g_thumb_loads_left--;
        vm_log("ui: thumb load %s\n", track->id);
        g_thumbs[slot].texture = vita2d_load_JPEG_file(path);
        vm_log("ui: thumb %s\n", g_thumbs[slot].texture ? "ok" : "err");
        g_thumbs[slot].failed = g_thumbs[slot].texture == NULL;
        g_thumbs[slot].used_at = g_frame;

        if (g_thumbs[slot].failed) {
            vm_log("ui: cannot decode artwork for %s\n", track->id);
            g_thumbs[slot].attempts = 1;
            app_thumb_invalidate(track);
        }
        return g_thumbs[slot].texture;
    }
}

static void draw_art_box(int x, int y, int size, const vm_track_t *track)
{
    vita2d_texture *texture = ui_thumb(track);

    vita2d_draw_rectangle(x, y, size, size, C_PANEL_HI);
    if (texture) {
        int w = vita2d_texture_get_width(texture);
        int h = vita2d_texture_get_height(texture);
        float scale;

        if (w > 0 && h > 0) {
            /* No upscale cap: the cover must fill its whole area edge to
             * edge, with no letterboxing frame around it. */
            scale = (float)size / (w > h ? w : h);
            vita2d_draw_texture_scale_rotate_hotspot(texture,
                                                     x + (float)size / 2,
                                                     y + (float)size / 2,
                                                     scale, scale, 0.0f,
                                                     (float)w / 2,
                                                     (float)h / 2);
        }
        return;
    }
    if (track && track->title[0]) {
        char initial[8];
        const unsigned char *src = (const unsigned char *)track->title;
        size_t n = 1;
        int w;

        if ((src[0] & 0xF0) == 0xF0)
            n = 4;
        else if ((src[0] & 0xE0) == 0xC0)
            n = 2;
        else if ((src[0] & 0xF0) == 0xE0)
            n = 3;
        if (n > strlen(track->title))
            n = strlen(track->title);
        memcpy(initial, track->title, n);
        initial[n] = '\0';

        w = ui_text_width(1.0f, initial);
        ui_text(x + (size - w) / 2, y + size / 2 - 12, C_DIM, 1.0f, initial);
    }
}

/* -------------------------------------------------------------------------- */
/* list rows                                                                  */
/* -------------------------------------------------------------------------- */

static void format_time(char *out, size_t cap, uint32_t ms)
{
    uint32_t total = ms / 1000;

    if (total >= 3600)
        snprintf(out, cap, "%u:%02u:%02u", total / 3600, (total / 60) % 60,
                 total % 60);
    else
        snprintf(out, cap, "%u:%02u", total / 60, total % 60);
}

static void draw_row(int y, const vm_track_t *track, int selected,
                     int playing)
{
    char duration[24];
    int row_h = ROW_H - 8;
    int text_x = PAD + ART_SZ + 28;
    int text_w = SCREEN_W - 2 * PAD - ART_SZ - 40;
    int title_y = y + row_h / 2 - 10;
    int artist_y = y + row_h / 2 + 14;
    int right = 0;

    vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, row_h,
                          selected ? C_PANEL_HI : C_PANEL);
    if (selected)
        vita2d_draw_rectangle(PAD, y, 4, row_h, C_BLUE);
    else if (playing)
        vita2d_draw_rectangle(PAD, y, 4, row_h, C_GREEN);

    draw_art_box(PAD + 8, y + (row_h - ART_SZ) / 2, ART_SZ, track);

    duration[0] = 0;
    if (track->duration_s > 0) {
        int minutes = track->duration_s / 60;

        snprintf(duration, sizeof(duration), "%d:%02d", minutes,
                 track->duration_s % 60);
        right += ui_text_width(0.8f, duration) + 16;
    }
    if (app_is_cached(track->id))
        right += 26;
    text_w -= right;

    ui_text_clip(text_x, title_y, text_w, C_TEXT, 0.9f, track->title);
    ui_text_clip(text_x, artist_y, text_w, C_DIM, 0.75f,
                 track->artist[0] ? track->artist : "Artista desconocido");

    if (app_is_cached(track->id)) {
        vita2d_draw_rectangle(SCREEN_W - PAD - 22, y + row_h / 2 - 8, 10, 14,
                              C_GREEN);
        vita2d_draw_rectangle(SCREEN_W - PAD - 24, y + row_h / 2 + 4, 14, 3,
                              C_GREEN);
    }

    if (duration[0]) {
        int width = ui_text_width(0.8f, duration);
        int dx = SCREEN_W - PAD - 12 - width -
                 (app_is_cached(track->id) ? 26 : 0);

        ui_text(dx, ui_baseline_in_box(y, row_h, 0.8f), C_DIM, 0.8f, duration);
    }

    if (app_is_favorite(track->id))
        ui_text(SCREEN_W - PAD - 14, y + 8, C_RED, 0.8f, "*");
}

static int list_first(int selected, int count)
{
    int vis = (LIST_BOTTOM - LIST_TOP) / ROW_H;
    int first = selected - (vis - 1);

    if (first < 0)
        first = 0;
    if (count > vis && first > count - vis)
        first = count - vis;
    return first;
}

static void draw_list(const vm_track_t *tracks, int count, int selected,
                      int playing_index)
{
    int vis = (LIST_BOTTOM - LIST_TOP) / ROW_H;
    int first = list_first(selected, count);
    int i;

    for (i = 0; i < vis && first + i < count; i++) {
        int y = LIST_TOP + i * ROW_H;

        draw_row(y, &tracks[first + i], first + i == selected,
                 playing_index == first + i);
    }
    if (count == 0)
        ui_text(PAD, LIST_TOP + 30, C_DIM, 0.85f, "Sin resultados todav\xc3\xad" "a.");
}

static void draw_empty_screen(const char *title, const char *message,
                              const char *hint)
{
    (void)title;
    ui_text(PAD + 4, LIST_TOP + 40, C_DIM, 0.9f, message);
    ui_footer(hint);
}

/* Iterates a visible list with the pad, the analog stick and the pad's first
 * touch. Returns -1 normally, or the index to activate (CROSS / tap). */
static int list_handle(int pressed, float ly, int count, int *selection,
                       int touch_x, int touch_y, int touch_rising)
{
    int first;
    int ret = -1;

    if (count <= 0)
        return -1;

    if (pressed & SCE_CTRL_UP && *selection > 0)
        (*selection)--;
    if (pressed & SCE_CTRL_DOWN && *selection < count - 1)
        (*selection)++;

    if (ly > 160 && (g_tick & 7) == 0 && *selection < count - 1)
        (*selection)++;
    if (ly < 96 && (g_tick & 7) == 0 && *selection > 0)
        (*selection)--;

    first = list_first(*selection, count);
    if (touch_rising && touch_y >= LIST_TOP && touch_y < LIST_BOTTOM) {
        int hit = (touch_y - LIST_TOP) / ROW_H;

        if (first + hit < count && touch_x >= PAD &&
            touch_x <= SCREEN_W - PAD) {
            *selection = first + hit;
            ret = *selection;
        }
    }
    return ret;
}

/* -------------------------------------------------------------------------- */
/* system IME (Vita keyboard)                                                 */
/* -------------------------------------------------------------------------- */

struct ui_state {
    app_screen screen;
    app_screen back;

    int home_sel;
    int results_sel;
    int favorites_sel;
    int library_sel;
    int settings_sel;
    int queue_sel;

    int ime_active;
    int ime_purpose; /* 0=search, 1=account login, 2=queue save, 3=track add */
    uint16_t ime_title[64];
    uint16_t ime_initial[160];
    uint16_t ime_input[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

    int lyrics_manual;
    int lyrics_scroll;

    int plist_sel;
    int pl_view;  /* playlist being browsed on APP_SCR_PLVIEW */
    int picker;   /* playlists screen acts as an add-track chooser */

    uint32_t prev_buttons;
    int touch_was_down;
};

static struct ui_state g_ui;

static void utf8_to_utf16(const char *src, uint16_t *dst, size_t max)
{
    size_t o = 0;

    if (!src || max == 0) {
        if (max)
            dst[0] = 0;
        return;
    }
    while (*src && o + 1 < max) {
        unsigned char c = (unsigned char)*src++;
        uint32_t cp;

        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && (src[0] & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (*src++ & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && (src[0] & 0xC0) == 0x80 &&
                   (src[1] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((src[0] & 0x3F) << 6) | (src[1] & 0x3F);
            src += 2;
        } else {
            cp = '?';
        }
        if (cp > 0xFFFF)
            cp = '?';
        dst[o++] = (uint16_t)cp;
    }
    dst[o] = 0;
}

static void utf16_to_utf8(const uint16_t *src, char *dst, size_t max)
{
    size_t o = 0;

    if (!src || max == 0) {
        if (max)
            dst[0] = '\0';
        return;
    }
    while (*src && o + 1 < max) {
        uint16_t c = *src++;

        if (c < 0x80) {
            dst[o++] = (char)c;
        } else if (c < 0x800) {
            if (o + 2 >= max)
                break;
            dst[o++] = (char)(0xC0 | (c >> 6));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            if (o + 3 >= max)
                break;
            dst[o++] = (char)(0xE0 | (c >> 12));
            dst[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    dst[o] = '\0';
}

static int open_ime(const char *title, const char *initial, int purpose)
{
    SceImeDialogParam param;
    int rc;

    if (g_ui.ime_active)
        return 0;

    memset(g_ui.ime_input, 0, sizeof(g_ui.ime_input));
    utf8_to_utf16(title ? title : "Texto", g_ui.ime_title,
                  sizeof(g_ui.ime_title) / sizeof(g_ui.ime_title[0]));
    utf8_to_utf16(initial ? initial : "", g_ui.ime_initial,
                  sizeof(g_ui.ime_initial) / sizeof(g_ui.ime_initial[0]));

    sceImeDialogParamInit(&param);
    param.supportedLanguages =
        SCE_IME_LANGUAGE_SPANISH | SCE_IME_LANGUAGE_ENGLISH;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.option = 0;
    param.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    param.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = g_ui.ime_title;
    param.maxTextLength = 120;
    param.initialText = g_ui.ime_initial;
    param.inputTextBuffer = g_ui.ime_input;

    rc = sceImeDialogInit(&param);
    if (rc < 0) {
        vm_log("ui: ime init failed 0x%08X\n", (unsigned)rc);
        app_toast("No se pudo abrir el teclado");
        return 0;
    }
    g_ui.ime_active = 1;
    g_ui.ime_purpose = purpose;
    return 1;
}

static void open_keyboard(void)
{
    open_ime("Buscar canciones", g_app.query[0] ? g_app.query : "", 0);
}

static void open_account_ime(void)
{
    open_ime("Nombre de cuenta", "", 1);
}

/* Returns 1 while the IME is still showing. CIRCLE / cancel closes it. */
static int update_ime(uint32_t pressed)
{
    SceCommonDialogStatus st;
    SceImeDialogResult result;
    char text[160];

    if (!g_ui.ime_active)
        return 0;

    /* Cancel must be requested while the dialog is still running; otherwise
     * WITH_CANCEL never reaches FINISHED on some firmwares. */
    if (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START)) {
        sceImeDialogAbort();
    }

    st = sceImeDialogGetStatus();
    if (st != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 1;

    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    g_ui.ime_active = 0;
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    if (result.button != SCE_IME_DIALOG_BUTTON_ENTER)
        return 0;

    utf16_to_utf8(g_ui.ime_input, text, sizeof(text));
    vm_trim(text);
    if (!text[0])
        return 0;

    if (g_ui.ime_purpose == 1) {
        app_account_login(text);
        g_ui.screen = APP_SCR_SETTINGS;
    } else if (g_ui.ime_purpose == 2) {
        int rc = app_playlist_save(text);

        if (rc == 0)
            app_toast("Playlist guardada: %s", text);
        else if (rc == -2)
            app_toast("Ya existe una playlist con ese nombre");
        else if (rc == -3)
            app_toast("No caben mas playlists");
        else
            app_toast("La cola esta vacia");
        g_ui.screen = APP_SCR_PLAYLISTS;
    } else if (g_ui.ime_purpose == 3) {
        /* New playlist holding just the track that is playing. */
        vm_track_t tr;
        int rc;

        memset(&tr, 0, sizeof(tr));
        pl_current_track(&tr);
        rc = app_playlist_create(text, &tr);
        if (rc == 0)
            app_toast("Cancion aniadida a %s", text);
        else if (rc == -2)
            app_toast("Ya existe una playlist con ese nombre");
        else if (rc == -3)
            app_toast("No caben mas playlists");
        else
            app_toast("No hay cancion en reproduccion");
        g_ui.screen = APP_SCR_NOW;
    } else {
        g_ui.screen = APP_SCR_RESULTS;
        g_ui.results_sel = 0;
        app_start_search(text);
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/* screens                                                                    */
/* -------------------------------------------------------------------------- */

static void open_screen(app_screen screen)
{
    if (screen == APP_SCR_NOW || screen == APP_SCR_LYRICS ||
        screen == APP_SCR_QUEUE)
        g_ui.back = g_ui.screen;
    g_ui.screen = screen;
}

static ui_tab tab_for_screen(app_screen screen)
{
    switch (screen) {
    case APP_SCR_HOME:
        return TAB_HOME;
    case APP_SCR_RESULTS:
        return TAB_SEARCH;
    case APP_SCR_LIBRARY:
    case APP_SCR_FAVORITES:
    case APP_SCR_ACCOUNT:
    case APP_SCR_PLAYLISTS:
        return TAB_LIB;
    case APP_SCR_NOW:
    case APP_SCR_LYRICS:
    case APP_SCR_QUEUE:
        return TAB_NOW;
    case APP_SCR_SETTINGS:
        return TAB_SETTINGS;
    default:
        return TAB_HOME;
    }
}

static void draw_tabs(void)
{
    int col = SCREEN_W / TAB_COUNT;
    ui_tab active = tab_for_screen(g_ui.screen);
    int i;

    if (g_ui.ime_active && g_ui.ime_purpose == 0)
        active = TAB_SEARCH;

    for (i = 0; i < TAB_COUNT; i++) {
        int x = i * col;
        int on = i == (int)active;

        vita2d_draw_rectangle(x, TAB_TOP, col, TAB_H, on ? C_PANEL_HI : C_PANEL);
        if (on)
            vita2d_draw_rectangle(x, TAB_TOP, col, 3, C_ACCENT);
        if (i < TAB_COUNT - 1)
            vita2d_draw_rectangle(x + col - 1, TAB_TOP, 1, TAB_H, C_BORDER);
        ui_text_box_center(x, TAB_TOP, col, TAB_H, on ? C_TEXT : C_DIM, 0.8f,
                           g_tab_labels[i]);
    }
    vita2d_draw_rectangle(0, TAB_BOTTOM - 1, SCREEN_W, 1, C_BORDER);
}

static int tab_hit(int x, int y)
{
    int col = SCREEN_W / TAB_COUNT;

    if (y < TAB_TOP || y >= TAB_BOTTOM)
        return -1;
    return x / col;
}

static void activate_tab(ui_tab tab)
{
    switch (tab) {
    default:
    case TAB_HOME:
        g_ui.screen = APP_SCR_HOME;
        break;
    case TAB_SEARCH:
        /* Results only — never auto-open IME here. Cancel + a leftover tab
         * tap used to reopen the keyboard and felt like it would not close.
         * Square / search-bar tap still open it explicitly. */
        g_ui.screen = APP_SCR_RESULTS;
        break;
    case TAB_LIB:
        g_ui.screen = APP_SCR_LIBRARY;
        break;
    case TAB_NOW:
        open_screen(APP_SCR_NOW);
        break;
    case TAB_SETTINGS:
        g_ui.screen = APP_SCR_SETTINGS;
        break;
    }
}

static void tab_switch(int dir)
{
    int cur = (int)tab_for_screen(g_ui.screen);

    activate_tab((ui_tab)((cur + dir + TAB_COUNT) % TAB_COUNT));
}

static int playing_screen_index(void)
{
    vm_track_t t;
    int i;

    if (!pl_current_track(&t))
        return -1;
    switch (g_ui.screen) {
    case APP_SCR_HOME:
        for (i = 0; i < g_app.history_count; i++)
            if (strcmp(g_app.history[i].id, t.id) == 0)
                return i;
        break;
    case APP_SCR_RESULTS:
        for (i = 0; i < g_app.result_count; i++)
            if (strcmp(g_app.results[i].id, t.id) == 0)
                return i;
        break;
    case APP_SCR_FAVORITES:
        for (i = 0; i < g_app.favorite_count; i++)
            if (strcmp(g_app.favorites[i].id, t.id) == 0)
                return i;
        break;
    default:
        break;
    }
    return -1;
}

static void draw_home(void)
{
    int playing = playing_screen_index();

    if (g_app.history_count > 0) {
        ui_text(PAD, LIST_TOP - 6, C_DIM, 0.65f, "RECIENTES");
        draw_list(g_app.history, g_app.history_count, g_ui.home_sel, playing);
    } else {
        ui_text(PAD, LIST_TOP + 40, C_DIM, 0.9f,
                "A\xc3\xban no has reproducido nada.");
        vita2d_draw_rectangle(PAD, LIST_TOP + 80, SCREEN_W - 2 * PAD, 56,
                              C_PANEL_HI);
        ui_text_center(SCREEN_W / 2, LIST_TOP + 104, C_TEXT, 0.9f,
                       "Pulsa X o abre la pesta\xc3\xb1" "a Buscar");
    }

    ui_footer("L/R pesta\xc3\xb1" "as   X play   SELECT descargar   TRI fav");
}

static void handle_home(uint32_t pressed, float ly, int touch_x, int touch_y,
                        int touch_rising)
{
    if (pressed & SCE_CTRL_TRIANGLE) {
        if (g_ui.home_sel < g_app.history_count)
            app_toggle_favorite(&g_app.history[g_ui.home_sel]);
    }
    if (pressed & SCE_CTRL_SELECT) {
        if (g_ui.home_sel < g_app.history_count)
            app_download_offline(&g_app.history[g_ui.home_sel]);
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (g_app.history_count > 0) {
            if (app_play_from_history(g_ui.home_sel) == 0)
                open_screen(APP_SCR_NOW);
        } else {
            open_keyboard();
        }
    }
    if (touch_rising && touch_y >= LIST_TOP && touch_y < LIST_BOTTOM &&
        g_app.history_count == 0) {
        open_keyboard();
    }
    if (list_handle(pressed, ly, g_app.history_count, &g_ui.home_sel,
                    touch_x, touch_y, touch_rising) >= 0) {
        if (app_play_from_history(g_ui.home_sel) == 0)
            open_screen(APP_SCR_NOW);
    }
}

static void draw_results(void)
{
    int playing = playing_screen_index();
    int list_top = LIST_TOP + 48;

    /* Search bar — tap or press SQUARE to type. */
    vita2d_draw_rectangle(PAD, LIST_TOP, SCREEN_W - 2 * PAD, 40, C_PANEL_HI);
    vita2d_draw_rectangle(PAD, LIST_TOP, 4, 40, C_ACCENT);
    if (g_app.query[0])
        ui_text_clip(PAD + 14, ui_baseline_in_box(LIST_TOP, 40, 0.85f),
                     SCREEN_W - 2 * PAD - 28, C_TEXT, 0.85f, g_app.query);
    else
        ui_text(PAD + 14, ui_baseline_in_box(LIST_TOP, 40, 0.85f), C_DIM,
                0.85f, "Toca aqu\xc3\xad o pulsa \xe2\x96\xa0 para buscar...");

    if (g_app.search_state == JOB_RUNNING) {
        ui_text(PAD, list_top + 30, C_DIM, 0.85f, "Buscando...");
    } else if (g_app.search_state == JOB_FAILED) {
        ui_text(PAD, list_top + 30, C_RED, 0.85f, g_app.search_error[0]
                                                       ? g_app.search_error
                                                       : "La b\xc3\xbasqueda fall\xc3\xb3.");
    } else if (g_app.result_count == 0) {
        ui_text(PAD, list_top + 30, C_DIM, 0.85f,
                "Escribe un t\xc3\xadtulo o artista y pulsa OK.");
    } else {
        int vis = (LIST_BOTTOM - list_top) / ROW_H;
        int first = g_ui.results_sel - (vis - 1);
        int i;

        if (first < 0)
            first = 0;
        if (g_app.result_count > vis && first > g_app.result_count - vis)
            first = g_app.result_count - vis;
        for (i = 0; i < vis && first + i < g_app.result_count; i++) {
            int y = list_top + i * ROW_H;

            draw_row(y, &g_app.results[first + i], first + i == g_ui.results_sel,
                     playing == first + i);
        }
    }

    ui_footer("X play   \xe2\x96\xa0 buscar   SELECT descargar   TRI fav");
}

static void handle_results(uint32_t pressed, float ly, int touch_x,
                           int touch_y, int touch_rising)
{
    int list_top = LIST_TOP + 48;
    int count = g_app.result_count;
    int vis = (LIST_BOTTOM - list_top) / ROW_H;
    int first;
    int activate = -1;

    if (g_app.search_state == JOB_DONE)
        app_search_consume();

    if (pressed & SCE_CTRL_SQUARE)
        open_keyboard();
    if (pressed & SCE_CTRL_SELECT) {
        if (g_ui.results_sel < count)
            app_download_offline(&g_app.results[g_ui.results_sel]);
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        if (g_ui.results_sel < count)
            app_toggle_favorite(&g_app.results[g_ui.results_sel]);
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (count > 0) {
            if (app_play_from_results(g_ui.results_sel) == 0)
                open_screen(APP_SCR_NOW);
        } else {
            open_keyboard();
        }
    }
    if (pressed & SCE_CTRL_CIRCLE)
        g_ui.screen = APP_SCR_HOME;

    if (count > 0) {
        if (pressed & SCE_CTRL_UP && g_ui.results_sel > 0)
            g_ui.results_sel--;
        if (pressed & SCE_CTRL_DOWN && g_ui.results_sel < count - 1)
            g_ui.results_sel++;
        if (ly > 160 && (g_tick & 7) == 0 && g_ui.results_sel < count - 1)
            g_ui.results_sel++;
        if (ly < 96 && (g_tick & 7) == 0 && g_ui.results_sel > 0)
            g_ui.results_sel--;
    }

    first = g_ui.results_sel - (vis - 1);
    if (first < 0)
        first = 0;
    if (count > vis && first > count - vis)
        first = count - vis;

    if (touch_rising) {
        if (touch_y >= LIST_TOP && touch_y < list_top && touch_x >= PAD &&
            touch_x <= SCREEN_W - PAD) {
            open_keyboard();
            return;
        }
        if (count > 0 && touch_y >= list_top && touch_y < LIST_BOTTOM &&
            touch_x >= PAD && touch_x <= SCREEN_W - PAD) {
            int hit = (touch_y - list_top) / ROW_H;

            if (first + hit < count) {
                g_ui.results_sel = first + hit;
                activate = g_ui.results_sel;
            }
        }
    }

    if (activate >= 0) {
        if (app_play_from_results(activate) == 0)
            open_screen(APP_SCR_NOW);
    }
}

static void draw_library_item(int y, int selected, const char *title,
                               const char *sub)
{
    int h = 64;

    vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, h,
                          selected ? C_PANEL_HI : C_PANEL);
    if (selected)
        vita2d_draw_rectangle(PAD, y, 4, h, C_ACCENT);
    ui_text(PAD + 20, y + 24, C_TEXT, 0.95f, title);
    ui_text(PAD + 20, y + 46, C_DIM, 0.75f, sub);
}

static void draw_library(void)
{
    int y = LIST_TOP;
    char fav_sub[64];
    char pl_sub[64];

    snprintf(fav_sub, sizeof(fav_sub), "%d canciones", g_app.favorite_count);
    draw_library_item(y, g_ui.library_sel == 0, "Favoritas", fav_sub);
    y += 64;
    snprintf(pl_sub, sizeof(pl_sub), "%d listas", g_app.playlist_count);
    draw_library_item(y, g_ui.library_sel == 1, "Playlists", pl_sub);

    ui_footer("X abrir   O atras");
}

static void handle_library(uint32_t pressed)
{
    if (pressed & SCE_CTRL_UP)
        g_ui.library_sel = 0;
    if (pressed & SCE_CTRL_DOWN)
        g_ui.library_sel = 1;
    if (pressed & SCE_CTRL_CROSS) {
        if (g_ui.library_sel == 1) {
            g_ui.screen = APP_SCR_PLAYLISTS;
            if (g_ui.plist_sel >= g_app.playlist_count)
                g_ui.plist_sel = 0;
        } else {
            g_ui.screen = APP_SCR_FAVORITES;
        }
    }
    if (pressed & SCE_CTRL_CIRCLE)
        g_ui.screen = APP_SCR_HOME;
}

static void draw_favorites(void)
{
    int playing = playing_screen_index();

    ui_text(PAD, LIST_TOP - 2, C_DIM, 0.7f, "Favoritas");
    if (g_app.favorite_count > 0)
        draw_list(g_app.favorites, g_app.favorite_count, g_ui.favorites_sel,
                  playing);
    else
        ui_text(PAD, LIST_TOP + 36, C_DIM, 0.85f,
                "Aun no hay favoritos: pulsa TRI en una cancion.");

    ui_footer("X play   CUADRADO descargar   TRI quitar   O atras");
}

static void handle_favorites(uint32_t pressed, float ly, int touch_x,
                             int touch_y, int touch_rising)
{
    int activate = -1;

    if (pressed & SCE_CTRL_TRIANGLE) {
        if (g_ui.favorites_sel < g_app.favorite_count) {
            app_toggle_favorite(&g_app.favorites[g_ui.favorites_sel]);
            if (g_ui.favorites_sel >= g_app.favorite_count &&
                g_ui.favorites_sel > 0)
                g_ui.favorites_sel--;
            if (g_ui.favorites_sel > g_app.favorite_count - 1)
                g_ui.favorites_sel =
                    g_app.favorite_count > 0 ? g_app.favorite_count - 1 : 0;
        }
    }
    if (pressed & SCE_CTRL_SQUARE) {
        if (g_ui.favorites_sel < g_app.favorite_count)
            app_download_offline(&g_app.favorites[g_ui.favorites_sel]);
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (g_app.favorite_count > 0 &&
            app_play_from_favorites(g_ui.favorites_sel) == 0)
            open_screen(APP_SCR_NOW);
    }
    if (pressed & SCE_CTRL_CIRCLE)
        g_ui.screen = APP_SCR_LIBRARY;

    activate = list_handle(pressed, ly, g_app.favorite_count,
                           &g_ui.favorites_sel, touch_x, touch_y,
                           touch_rising);
    if (activate >= 0) {
        if (app_play_from_favorites(activate) == 0)
            open_screen(APP_SCR_NOW);
    }
}

static void draw_account(void)
{
    int y = LIST_TOP + 20;

    ui_text(PAD, y, C_TEXT, 1.0f, "Mi cuenta");
    y += 40;
    if (app_account_logged_in()) {
        ui_text(PAD, y, C_DIM, 0.85f, "Sesion local");
        y += 28;
        ui_text(PAD, y, C_ACCENT, 1.05f, app_account_name());
        y += 56;
        vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, 48, C_PANEL);
        ui_text_box_center(PAD, y, SCREEN_W - 2 * PAD, 48, C_RED, 0.9f,
                           "Cerrar sesion");
    } else {
        ui_text(PAD, y, C_DIM, 0.85f, "No has iniciado sesion.");
        y += 48;
        vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, 48, C_ACCENT);
        ui_text_box_center(PAD, y, SCREEN_W - 2 * PAD, 48, C_BG, 0.9f,
                           "Iniciar sesion");
    }
    ui_footer("X accion   O atras");
}

static void handle_account(uint32_t pressed, int touch_x, int touch_y,
                           int touch_rising)
{
    int btn_y = LIST_TOP + 20 + 40 + 28 + 56;

    if (!app_account_logged_in())
        btn_y = LIST_TOP + 20 + 40 + 48;

    if (pressed & SCE_CTRL_CROSS) {
        if (app_account_logged_in())
            app_account_logout();
        else
            open_account_ime();
    }
    if (pressed & SCE_CTRL_CIRCLE)
        g_ui.screen = APP_SCR_LIBRARY;

    if (touch_rising && touch_y >= btn_y && touch_y < btn_y + 48 &&
        touch_x >= PAD && touch_x <= SCREEN_W - PAD) {
        if (app_account_logged_in())
            app_account_logout();
        else
            open_account_ime();
    }
}

/* -------------------------------------------------------------------------- */
/* now playing                                                                */
/* -------------------------------------------------------------------------- */

/* Split layout: cover square on the left, aligned to the same left margin as
 * every other screen so it does not float toward the center; artist/title,
 * times, progress bar, transport and icon tiles live in the right column.
 * The bottom icon rows sit mid-column (right below the cover) instead of
 * sinking into the footer. */
#define NOW_ART_SZ     400
#define NOW_ART_X      PAD
#define NOW_ART_Y      (TAB_BOTTOM + 8)
#define NOW_QUEUE_X    (NOW_ART_X + NOW_ART_SZ - 56)
#define NOW_QUEUE_Y    (NOW_ART_Y + 8)
#define NOW_QUEUE_SZ   48
#define NOW_BACK_X     (NOW_ART_X + 8)
#define NOW_BACK_Y     (NOW_ART_Y + NOW_ART_SZ - 64)
#define NOW_BACK_SZ    48
#define NOW_TXT_X      (NOW_ART_X + NOW_ART_SZ + 28)
#define NOW_TXT_W      (SCREEN_W - NOW_TXT_X - PAD)
#define NOW_BAR_X      NOW_TXT_X
#define NOW_BAR_W      NOW_TXT_W
#define NOW_PROG_Y     168
#define NOW_CTRL_Y     206
#define NOW_CTRL_H     64
#define NOW_PREV_W     76
#define NOW_PLAY_W     92
#define NOW_NEXT_W     76
#define NOW_TX         (NOW_TXT_X + (NOW_TXT_W - (NOW_PREV_W + NOW_PLAY_W + \
                                     NOW_NEXT_W + 32)) / 2)
#define NOW_PREV_X     NOW_TX
#define NOW_PLAY_X     (NOW_TX + NOW_PREV_W + 16)
#define NOW_NEXT_X     (NOW_PLAY_X + NOW_PLAY_W + 16)
#define NOW_TRIO_Y     (NOW_CTRL_Y + NOW_CTRL_H + 18)
#define NOW_TRIO_H     56
#define NOW_SUB_Y      (NOW_TRIO_Y + NOW_TRIO_H + 10)
#define NOW_BTN_H      58

static void draw_icon_prev(int cx, int cy, unsigned int color)
{
    int i;

    vita2d_draw_rectangle(cx - 14, cy - 12, 4, 24, color);
    for (i = 0; i < 12; i++)
        vita2d_draw_rectangle(cx + 2 - i, cy - 12 + i, 3, 24 - 2 * i, color);
}

static void draw_icon_next(int cx, int cy, unsigned int color)
{
    int i;

    for (i = 0; i < 12; i++)
        vita2d_draw_rectangle(cx - 10 + i, cy - 12 + i, 3, 24 - 2 * i, color);
    vita2d_draw_rectangle(cx + 8, cy - 12, 4, 24, color);
}

static void draw_icon_play(int cx, int cy, unsigned int color)
{
    int i;

    for (i = 0; i < 14; i++)
        vita2d_draw_rectangle(cx - 8 + i, cy - 14 + i, 3, 28 - 2 * i, color);
}

static void draw_icon_pause(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx - 11, cy - 14, 8, 28, color);
    vita2d_draw_rectangle(cx + 3, cy - 14, 8, 28, color);
}

/* Repeat button glyphs: flat, current-player style. */
static void draw_icon_repeat(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx - 11, cy - 9, 22, 3, color);
    vita2d_draw_rectangle(cx - 11, cy - 9, 3, 16, color);
    vita2d_draw_rectangle(cx + 8, cy - 9, 3, 16, color);
    vita2d_draw_rectangle(cx + 2, cy - 14, 10, 6, color);
    vita2d_draw_rectangle(cx + 8, cy - 12, 3, 4, color);
    vita2d_draw_rectangle(cx - 11, cy + 7, 22, 3, color);
    vita2d_draw_rectangle(cx + 8, cy + 4, 3, 6, color);
    vita2d_draw_rectangle(cx - 11, cy + 4, 3, 6, color);
    vita2d_draw_rectangle(cx - 11, cy - 2, 3, 6, color);
}

static void draw_icon_repeat_one(int cx, int cy, unsigned int color)
{
    draw_icon_repeat(cx, cy, color);
    ui_text(cx - 4, cy + 1, color, 0.6f, "1");
}

/* Heart (favourite). */
static void draw_icon_heart(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx - 9, cy - 8, 7, 7, color);
    vita2d_draw_rectangle(cx + 2, cy - 8, 7, 7, color);
    vita2d_draw_rectangle(cx - 5, cy - 4, 10, 6, color);
    vita2d_draw_rectangle(cx - 7, cy + 2, 14, 3, color);
    vita2d_draw_rectangle(cx - 4, cy + 5, 8, 3, color);
    vita2d_draw_rectangle(cx - 2, cy + 8, 4, 2, color);
}

/* Down arrow onto a tray (download / cached). */
static void draw_icon_download(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx - 3, cy - 10, 6, 10, color);
    vita2d_draw_rectangle(cx - 9, cy - 1, 18, 3, color);
    vita2d_draw_rectangle(cx - 7, cy + 2, 14, 2, color);
    vita2d_draw_rectangle(cx - 5, cy + 4, 10, 2, color);
    vita2d_draw_rectangle(cx - 3, cy + 6, 6, 2, color);
    vita2d_draw_rectangle(cx - 11, cy + 9, 22, 3, color);
}

/* Eighth note (lyrics). */
static void draw_icon_note(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx + 3, cy - 11, 3, 16, color);
    vita2d_draw_rectangle(cx + 6, cy - 11, 6, 4, color);
    vita2d_draw_rectangle(cx - 3, cy + 2, 8, 2, color);
    vita2d_draw_rectangle(cx - 5, cy + 4, 12, 4, color);
    vita2d_draw_rectangle(cx - 4, cy + 8, 10, 2, color);
    vita2d_draw_rectangle(cx - 2, cy + 10, 6, 1, color);
}

/* Plus (add to playlist). */
static void draw_icon_plus(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx - 9, cy - 2, 18, 4, color);
    vita2d_draw_rectangle(cx - 2, cy - 9, 4, 18, color);
}

/* Crossed arrows (shuffle). */
static void draw_icon_shuffle(int cx, int cy, unsigned int color)
{
    vita2d_draw_rectangle(cx - 10, cy - 7, 3, 2, color);
    vita2d_draw_rectangle(cx - 7, cy - 5, 3, 2, color);
    vita2d_draw_rectangle(cx - 4, cy - 3, 3, 2, color);
    vita2d_draw_rectangle(cx - 1, cy - 1, 3, 2, color);
    vita2d_draw_rectangle(cx + 2, cy + 1, 3, 2, color);
    vita2d_draw_rectangle(cx + 5, cy + 3, 3, 2, color);
    vita2d_draw_rectangle(cx + 8, cy + 2, 3, 6, color);
    vita2d_draw_rectangle(cx + 5, cy + 6, 3, 3, color);

    vita2d_draw_rectangle(cx - 10, cy + 5, 3, 2, color);
    vita2d_draw_rectangle(cx - 7, cy + 3, 3, 2, color);
    vita2d_draw_rectangle(cx - 4, cy + 1, 3, 2, color);
    vita2d_draw_rectangle(cx + 1, cy - 3, 3, 2, color);
    vita2d_draw_rectangle(cx + 4, cy - 5, 3, 2, color);
    vita2d_draw_rectangle(cx + 7, cy - 8, 3, 6, color);
    vita2d_draw_rectangle(cx + 4, cy - 10, 3, 3, color);
}

/* Left-pointing arrow (back): solid triangle apexing left plus a shaft. */
static void draw_icon_back(int cx, int cy, unsigned int color)
{
    int i;

    for (i = 0; i < 7; i++)
        vita2d_draw_rectangle(cx - 8 + 2 * i, cy - 7 + i, 2, 14 - 2 * i,
                              color);
    vita2d_draw_rectangle(cx + 5, cy - 2, 8, 4, color);
}

static void draw_now(void)
{
    vm_track_t track;
    uint32_t pos = pl_position_ms();
    uint32_t total = pl_duration_ms();
    pl_state state = pl_get_state();
    char time_buf[24];
    char cur[96];
    const char *status;
    int playing = state == PL_PLAYING;
    int buf_pct = pl_buffer_percent();

    memset(&track, 0, sizeof(track));

    if (!pl_current_track(&track)) {
        draw_empty_screen("Ahora suena",
                          "No hay nada en reproduccion.",
                          "Elige una cancion desde Inicio o Buscar");
        return;
    }

    /* Left: huge cover square + back button in its bottom-left corner. */
    draw_art_box(NOW_ART_X, NOW_ART_Y, NOW_ART_SZ, &track);
    draw_rounded(NOW_BACK_X, NOW_BACK_Y, NOW_BACK_SZ, NOW_BACK_SZ, C_PANEL_HI);
    draw_icon_back(NOW_BACK_X + NOW_BACK_SZ / 2,
                   NOW_BACK_Y + NOW_BACK_SZ / 2, C_TEXT);

    /* Queue button, top-right corner of the cover. */
    draw_rounded(NOW_QUEUE_X, NOW_QUEUE_Y, NOW_QUEUE_SZ, NOW_QUEUE_SZ,
                 C_PANEL_HI);
    {
        int qx = NOW_QUEUE_X + NOW_QUEUE_SZ / 2;
        int qy = NOW_QUEUE_Y + NOW_QUEUE_SZ / 2;
        int l;

        for (l = 0; l < 3; l++) {
            vita2d_draw_rectangle(qx - 8, qy - 7 + l * 7, 16, 2, C_TEXT);
            vita2d_draw_rectangle(qx - 8, qy - 7 + l * 7, 2, 2,
                                  l == 0 ? C_ACCENT : C_TEXT);
        }
    }

    /* Right, top: artist (small) over title (larger), times to the right.
     * Both drop a little below the tab strip for breathing room. */
    ui_text_clip(NOW_TXT_X, NOW_ART_Y + 26, NOW_TXT_W - 84, C_DIM, 0.75f,
                 track.artist[0] ? track.artist : "Artista desconocido");
    ui_text_clip(NOW_TXT_X, NOW_ART_Y + 50, NOW_TXT_W - 84, C_TEXT, 1.1f,
                 track.title);

    if (state == PL_PLAYING)
        status = "Reproduciendo";
    else if (state == PL_PAUSED)
        status = "En pausa";
    else if (state == PL_BUFFERING)
        status = "Descargando";
    else if (state == PL_RESOLVING)
        status = "Preparando";
    else if (state == PL_ERROR)
        status = "Error";
    else
        status = "Listo";

    {
        int buffered = pl_buffered_ms();
        int ty = NOW_ART_Y + 30;

        if (state == PL_BUFFERING || state == PL_RESOLVING)
            snprintf(cur, sizeof(cur), "%d%%", buf_pct);
        else if (state == PL_PLAYING && buffered < PL_BUF_TARGET_MS)
            snprintf(cur, sizeof(cur), "buf %d s", (buffered + 500) / 1000);
        else
            snprintf(cur, sizeof(cur), "%s", status);
        ui_text(NOW_TXT_X + NOW_TXT_W - ui_text_width(0.7f, cur), ty, C_DIM,
                0.7f, cur);

        format_time(time_buf, sizeof(time_buf), pos);
        ui_text(NOW_TXT_X + NOW_TXT_W - ui_text_width(0.75f, time_buf),
                ty + 22, C_TEXT, 0.75f, time_buf);
        format_time(time_buf, sizeof(time_buf), total);
        ui_text(NOW_TXT_X + NOW_TXT_W - ui_text_width(0.7f, time_buf),
                ty + 44, C_DIM, 0.7f, time_buf);
    }

    /* Progress bar with a draggable-looking thumb. */
    {
        int fill = total > 0 ? (int)((int64_t)NOW_BAR_W * pos / total) : 0;
        int buf_fill = (int)((int64_t)NOW_BAR_W * buf_pct / 100);

        if (fill < 0)
            fill = 0;
        if (fill > NOW_BAR_W)
            fill = NOW_BAR_W;
        if (buf_fill < 0)
            buf_fill = 0;
        if (buf_fill > NOW_BAR_W)
            buf_fill = NOW_BAR_W;

        vita2d_draw_rectangle(NOW_BAR_X, NOW_PROG_Y, NOW_BAR_W, 8, C_PANEL_HI);
        if (buf_fill > 0)
            vita2d_draw_rectangle(NOW_BAR_X, NOW_PROG_Y, buf_fill, 8, C_BUF);
        if (fill > 0)
            vita2d_draw_rectangle(NOW_BAR_X, NOW_PROG_Y, fill, 8, C_ACCENT);
        if (fill > 0)
            vita2d_draw_rectangle(NOW_BAR_X + fill - 7, NOW_PROG_Y - 5, 14, 18,
                                  C_TEXT);
    }

    /* Transport: prev | play | next, centered in the right column. */
    draw_rounded(NOW_PREV_X, NOW_CTRL_Y, NOW_PREV_W, NOW_CTRL_H, C_PANEL);
    draw_icon_prev(NOW_PREV_X + NOW_PREV_W / 2, NOW_CTRL_Y + NOW_CTRL_H / 2,
                   C_TEXT);

    draw_rounded(NOW_PLAY_X, NOW_CTRL_Y, NOW_PLAY_W, NOW_CTRL_H,
                 playing ? C_ACCENT : C_PANEL_HI);
    if (playing)
        draw_icon_pause(NOW_PLAY_X + NOW_PLAY_W / 2,
                        NOW_CTRL_Y + NOW_CTRL_H / 2, C_BG);
    else
        draw_icon_play(NOW_PLAY_X + NOW_PLAY_W / 2,
                       NOW_CTRL_Y + NOW_CTRL_H / 2, C_TEXT);

    draw_rounded(NOW_NEXT_X, NOW_CTRL_Y, NOW_NEXT_W, NOW_CTRL_H, C_PANEL);
    draw_icon_next(NOW_NEXT_X + NOW_NEXT_W / 2, NOW_CTRL_Y + NOW_CTRL_H / 2,
                   C_TEXT);

    /* Right column, below the transport: two icon rows, no labels.
     * Favourite / download / lyrics, then add-to-playlist / shuffle / repeat. */
    {
        int gap = 10;
        int w = (NOW_TXT_W - 2 * gap) / 3;
        int fav = app_is_favorite(track.id);
        int cached = app_is_cached(track.id);
        int x1 = NOW_TXT_X;
        int x2 = x1 + w + gap;
        int x3 = x2 + w + gap;
        int y = NOW_TRIO_Y;
        int by = NOW_SUB_Y;

        draw_rounded(x1, y, w, NOW_TRIO_H, fav ? C_PANEL_HI : C_PANEL);
        draw_icon_heart(x1 + w / 2, y + NOW_TRIO_H / 2,
                        fav ? C_RED : C_TEXT);

        draw_rounded(x2, y, w, NOW_TRIO_H, cached ? C_PANEL_HI : C_PANEL);
        draw_icon_download(x2 + w / 2, y + NOW_TRIO_H / 2,
                           cached ? C_GREEN : C_TEXT);

        draw_rounded(x3, y, w, NOW_TRIO_H, C_PANEL);
        draw_icon_note(x3 + w / 2, y + NOW_TRIO_H / 2, C_TEXT);

        draw_rounded(x1, by, w, NOW_TRIO_H, C_PANEL);
        draw_icon_plus(x1 + w / 2, by + NOW_TRIO_H / 2, C_TEXT);

        draw_rounded(x2, by, w, NOW_TRIO_H,
                     g_app.shuffle ? C_PANEL_HI : C_PANEL);
        draw_icon_shuffle(x2 + w / 2, by + NOW_TRIO_H / 2,
                          g_app.shuffle ? C_ACCENT : C_TEXT);

        {
            int repeat = g_app.repeat_mode;

            draw_rounded(x3, by, w, NOW_TRIO_H,
                         repeat != REPEAT_OFF ? C_PANEL_HI : C_PANEL);
            if (repeat == REPEAT_ONE)
                draw_icon_repeat_one(x3 + w / 2, by + NOW_TRIO_H / 2, C_GREEN);
            else
                draw_icon_repeat(x3 + w / 2, by + NOW_TRIO_H / 2,
                                 repeat == REPEAT_ALL ? C_ACCENT : C_TEXT);
        }
    }

    ui_footer("X play   SELECT cola   CUADRADO descargar   O atras");
}

static void handle_now(uint32_t pressed, int touch_x, int touch_y,
                       int touch_rising)
{
    if (pressed & SCE_CTRL_CROSS)
        app_toggle_pause();
    if (pressed & SCE_CTRL_TRIANGLE)
        app_cycle_repeat();
    if (pressed & SCE_CTRL_LEFT)
        app_play_prev();
    if (pressed & SCE_CTRL_RIGHT)
        app_play_next();
    if (pressed & SCE_CTRL_LTRIGGER)
        app_seek_relative(-10);
    if (pressed & SCE_CTRL_RTRIGGER)
        app_seek_relative(10);
    if (pressed & SCE_CTRL_SQUARE) {
        vm_track_t tr;

        memset(&tr, 0, sizeof(tr));
        if (pl_current_track(&tr))
            app_download_offline(&tr);
    }
    if (pressed & SCE_CTRL_SELECT) {
        g_ui.queue_sel = g_app.queue_index >= 0 ? g_app.queue_index : 0;
        open_screen(APP_SCR_QUEUE);
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        if (g_ui.back == APP_SCR_HOME || g_ui.back == APP_SCR_RECENT ||
            g_ui.back == APP_SCR_RESULTS || g_ui.back == APP_SCR_FAVORITES ||
            g_ui.back == APP_SCR_LIBRARY || g_ui.back == APP_SCR_QUEUE ||
            g_ui.back == APP_SCR_PLAYLISTS)
            g_ui.screen = g_ui.back;
        else
            g_ui.screen = APP_SCR_HOME;
        return;
    }

    if (!touch_rising)
        return;

    /* Queue button on the cover's top-right corner. */
    if (touch_x >= NOW_QUEUE_X && touch_x < NOW_QUEUE_X + NOW_QUEUE_SZ &&
        touch_y >= NOW_QUEUE_Y && touch_y < NOW_QUEUE_Y + NOW_QUEUE_SZ) {
        g_ui.queue_sel = g_app.queue_index >= 0 ? g_app.queue_index : 0;
        open_screen(APP_SCR_QUEUE);
        return;
    }

    /* Back button on the cover's bottom-left corner. */
    if (touch_x >= NOW_BACK_X && touch_x < NOW_BACK_X + NOW_BACK_SZ &&
        touch_y >= NOW_BACK_Y && touch_y < NOW_BACK_Y + NOW_BACK_SZ) {
        if (g_ui.back == APP_SCR_HOME || g_ui.back == APP_SCR_RECENT ||
            g_ui.back == APP_SCR_RESULTS || g_ui.back == APP_SCR_FAVORITES ||
            g_ui.back == APP_SCR_LIBRARY || g_ui.back == APP_SCR_QUEUE ||
            g_ui.back == APP_SCR_PLAYLISTS)
            g_ui.screen = g_ui.back;
        else
            g_ui.screen = APP_SCR_HOME;
        return;
    }

    /* Transport: prev | play | next. */
    if (touch_y >= NOW_CTRL_Y && touch_y < NOW_CTRL_Y + NOW_CTRL_H) {
        if (touch_x >= NOW_PREV_X && touch_x < NOW_PREV_X + NOW_PREV_W)
            app_play_prev();
        else if (touch_x >= NOW_PLAY_X && touch_x < NOW_PLAY_X + NOW_PLAY_W)
            app_toggle_pause();
        else if (touch_x >= NOW_NEXT_X && touch_x < NOW_NEXT_X + NOW_NEXT_W)
            app_play_next();
        return;
    }

    /* Two icon rows below the transport: favourite / download / lyrics, then
     * add-to-playlist / shuffle / repeat. */
    {
        int gap = 10;
        int w = (NOW_TXT_W - 2 * gap) / 3;
        int x1 = NOW_TXT_X;
        int x2 = x1 + w + gap;
        int x3 = x2 + w + gap;
        int fav_y = NOW_TRIO_Y;
        int sub_y = NOW_SUB_Y;

        if (touch_y >= fav_y && touch_y < fav_y + NOW_TRIO_H) {
            if (touch_x >= x1 && touch_x < x1 + w) {
                vm_track_t tr;

                memset(&tr, 0, sizeof(tr));
                if (pl_current_track(&tr))
                    app_toggle_favorite(&tr);
            } else if (touch_x >= x2 && touch_x < x2 + w) {
                vm_track_t tr;

                memset(&tr, 0, sizeof(tr));
                if (pl_current_track(&tr))
                    app_download_offline(&tr);
            } else if (touch_x >= x3 && touch_x < x3 + w) {
                app_request_lyrics();
                open_screen(APP_SCR_LYRICS);
            }
            return;
        }

        if (touch_y >= sub_y && touch_y < sub_y + NOW_TRIO_H) {
            if (touch_x >= x1 && touch_x < x1 + w) {
                /* Add-to-playlist chooser over the current track. */
                g_ui.picker = 1;
                g_ui.plist_sel = 0;
                g_ui.screen = APP_SCR_PLAYLISTS;
            } else if (touch_x >= x2 && touch_x < x2 + w) {
                app_toggle_shuffle();
            } else if (touch_x >= x3 && touch_x < x3 + w) {
                app_cycle_repeat();
            }
            return;
        }
    }

    if (touch_y >= NOW_PROG_Y - 8 && touch_y < NOW_PROG_Y + 18 &&
        touch_x >= NOW_BAR_X && touch_x <= NOW_BAR_X + NOW_BAR_W) {
        uint32_t tot = pl_duration_ms();

        if (tot > 0) {
            uint32_t ms = (uint32_t)((int64_t)(touch_x - NOW_BAR_X) * tot /
                                     NOW_BAR_W);

            app_seek_ms(ms);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* lyrics                                                                     */
/* -------------------------------------------------------------------------- */

static void draw_lyrics(void)
{
    int scroll = g_ui.lyrics_manual ? g_ui.lyrics_scroll : g_app.lyrics_scroll;
    int line_h = 28;
    int highlighted;
    int i;

    if (g_app.lyrics_state == JOB_FAILED) {
        ui_text(PAD, LIST_TOP + 30, C_RED, 0.85f,
                "No se encontr\xc3\xb3 la letra.");
    } else if (g_app.lyrics_state == JOB_RUNNING) {
        ui_text(PAD, LIST_TOP + 30, C_DIM, 0.85f, "Obteniendo letra...");
    } else if (g_app.lyrics_state == JOB_IDLE) {
        ui_text(PAD, LIST_TOP + 30, C_DIM, 0.85f,
                "Pulsa TRI\xc3\x81NGULO para pedir la letra.");
    } else {
        highlighted = lyrics_line_at(&g_app.lyrics, pl_position_ms());

        for (i = 0; i < 16 && i + scroll < g_app.lyrics.count; i++) {
            const vm_lyric_line_t *line =
                &g_app.lyrics.lines[i + scroll];
            char text[128];
            int n = line->length;
            int y = LIST_TOP + i * line_h;

            if (y + line_h > LIST_BOTTOM)
                break;
            if (n > (int)sizeof(text) - 1)
                n = (int)sizeof(text) - 1;
            memcpy(text, g_app.lyrics.text + line->offset, n);
            text[n] = '\0';
            ui_text_clip(PAD + 4, y + 10, SCREEN_W - 2 * PAD - 8,
                         i + scroll == highlighted ? C_TEXT : C_DIM, 0.85f,
                         text);
        }
    }

    ui_footer("X play/pause   TRI letra   ^/v scroll   O atr\xc3\xa1s");
}

static void handle_lyrics(uint32_t pressed)
{
    if (pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) {
        g_ui.lyrics_manual = 1;
        if (pressed & SCE_CTRL_DOWN)
            g_ui.lyrics_scroll += 2;
        else
            g_ui.lyrics_scroll -= 2;
        if (g_ui.lyrics_scroll < 0)
            g_ui.lyrics_scroll = 0;
    }
    if (pressed & SCE_CTRL_TRIANGLE)
        app_request_lyrics();
    if (pressed & SCE_CTRL_CROSS)
        app_toggle_pause();
    if (pressed & SCE_CTRL_CIRCLE)
        open_screen(APP_SCR_NOW);
}

/* -------------------------------------------------------------------------- */
/* saved playlists                                                            */
/* -------------------------------------------------------------------------- */

static void draw_playlists(void)
{
    int i;
    int rows;
    char sub[96];

    if (g_ui.picker) {
        vm_track_t cur;

        memset(&cur, 0, sizeof(cur));
        pl_current_track(&cur);
        snprintf(sub, sizeof(sub), "Anadir '%.40s' a...", cur.title);
        rows = g_app.playlist_count + 1;
    } else {
        snprintf(sub, sizeof(sub), "%d listas guardadas",
                 g_app.playlist_count);
        rows = g_app.playlist_count;
    }
    ui_text(PAD, LIST_TOP - 2, C_DIM, 0.7f, sub);

    if (g_ui.picker) {
        int y = LIST_TOP + 8;

        draw_library_item(y, g_ui.plist_sel == 0, "+ Nueva playlist",
                          "Crea una lista con esta cancion");
        for (i = 0; i < g_app.playlist_count && i < 4; i++) {
            char name[64];
            int yy = y + (i + 1) * 64;

            snprintf(name, sizeof(name), "%s", g_app.playlist_names[i]);
            draw_library_item(yy, g_ui.plist_sel == i + 1, name,
                              "Anadir la cancion aqui");
        }
        ui_footer("X anadir   O cancelar");
        return;
    }

    for (i = 0; i < g_app.playlist_count && i < 5; i++) {
        int y = LIST_TOP + 8 + i * ROW_H;
        char line[96];

        draw_rounded(PAD, y, SCREEN_W - 2 * PAD, ROW_H - 8,
                     g_ui.plist_sel == i ? C_PANEL_HI : C_PANEL);
        if (g_ui.plist_sel == i)
            vita2d_draw_rectangle(PAD, y, 4, ROW_H - 8, C_ACCENT);
        snprintf(line, sizeof(line), "%s", g_app.playlist_names[i]);
        ui_text_clip(PAD + 20, y + 24, SCREEN_W - 3 * PAD - 60, C_TEXT, 0.9f,
                     line);
        snprintf(line, sizeof(line), "%d canciones  (toca para ver)",
                 g_app.playlist_counts[i]);
        ui_text(PAD + 20, y + 46, C_DIM, 0.75f, line);
    }

    if (g_app.playlist_count == 0)
        ui_text(PAD, LIST_TOP + 30, C_DIM, 0.85f,
                "Sin playlists. Reproduce algo y guardalo.");

    ui_footer("X ver   CUADRADO guardar cola   TRI borrar   O atras");
}

static void handle_playlists(uint32_t pressed, float ly, int touch_x,
                             int touch_y, int touch_rising)
{
    int count = g_app.playlist_count;
    int rows = g_ui.picker ? count + 1 : count;

    if (g_ui.picker) {
        vm_track_t cur;

        memset(&cur, 0, sizeof(cur));
        pl_current_track(&cur);

        if (pressed & SCE_CTRL_UP && g_ui.plist_sel > 0)
            g_ui.plist_sel--;
        if (pressed & SCE_CTRL_DOWN && g_ui.plist_sel < rows - 1)
            g_ui.plist_sel++;
        if (pressed & SCE_CTRL_CIRCLE ||
            (pressed & SCE_CTRL_TRIANGLE)) {
            g_ui.picker = 0;
            g_ui.screen = APP_SCR_NOW;
            return;
        }
        if (pressed & SCE_CTRL_CROSS) {
            if (g_ui.plist_sel == 0) {
                open_ime("Nueva playlist", "", 3);
                return;
            }
            if (g_ui.plist_sel - 1 < count) {
                int rc = app_playlist_add_track(g_ui.plist_sel - 1, &cur);

                if (rc == 0)
                    app_toast("Aniadida a la playlist");
                else if (rc == -2)
                    app_toast("Ya esta en esa playlist");
                else if (rc == -3)
                    app_toast("Playlist llena");
            }
            g_ui.picker = 0;
            g_ui.screen = APP_SCR_NOW;
            return;
        }
        if (touch_rising && touch_y >= LIST_TOP &&
            touch_y < LIST_TOP + 8 + rows * 64) {
            int hit = (touch_y - LIST_TOP - 8) / 64;

            if (hit >= 0 && hit < rows) {
                g_ui.plist_sel = hit;
                if (hit == 0) {
                    open_ime("Nueva playlist", "", 3);
                    return;
                }
                {
                    int rc = app_playlist_add_track(hit - 1, &cur);

                    if (rc == 0)
                        app_toast("Aniadida a la playlist");
                    else if (rc == -2)
                        app_toast("Ya esta en esa playlist");
                    else if (rc == -3)
                        app_toast("Playlist llena");
                }
                g_ui.picker = 0;
                g_ui.screen = APP_SCR_NOW;
            }
        }
        return;
    }

    if (pressed & SCE_CTRL_UP && g_ui.plist_sel > 0)
        g_ui.plist_sel--;
    if (pressed & SCE_CTRL_DOWN && g_ui.plist_sel < count - 1)
        g_ui.plist_sel++;
    if (pressed & SCE_CTRL_SQUARE) {
        open_ime("Nombre de la playlist", "", 2);
        return;
    }
    if (pressed & SCE_CTRL_TRIANGLE && count > 0) {
        app_playlist_delete(g_ui.plist_sel);
        if (g_ui.plist_sel >= count - 1 && g_ui.plist_sel > 0)
            g_ui.plist_sel--;
        return;
    }
    if (pressed & SCE_CTRL_CROSS && count > 0) {
        g_ui.pl_view = g_ui.plist_sel;
        g_ui.screen = APP_SCR_PLVIEW;
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        g_ui.screen = APP_SCR_LIBRARY;
        return;
    }

    if (touch_rising && touch_y >= LIST_TOP && touch_y < LIST_BOTTOM) {
        int hit = (touch_y - LIST_TOP - 8) / ROW_H;

        if (hit >= 0 && hit < count && touch_x >= PAD &&
            touch_x <= SCREEN_W - PAD) {
            g_ui.plist_sel = hit;
            g_ui.pl_view = hit;
            g_ui.screen = APP_SCR_PLVIEW;
        }
    }
}

/* -------------------------------------------------------------------------- */
/* playlist contents (browsed like favourites)                                 */
/* -------------------------------------------------------------------------- */

static void draw_pl_view(void)
{
    int count;
    char title[80];

    if (g_ui.pl_view < 0 || g_ui.pl_view >= g_app.playlist_count) {
        g_ui.screen = APP_SCR_PLAYLISTS;
        return;
    }

    count = g_app.playlist_counts[g_ui.pl_view];
    snprintf(title, sizeof(title), "%s (%d)", g_app.playlist_names[g_ui.pl_view],
             count);
    ui_text(PAD, LIST_TOP - 2, C_DIM, 0.7f, title);

    if (count <= 0) {
        ui_text(PAD, LIST_TOP + 36, C_DIM, 0.85f,
                "Playlist vacia. Anade canciones con el boton +.");
        ui_footer("O atras");
        return;
    }

    draw_list(g_app.playlist_tracks[g_ui.pl_view], count, g_ui.plist_sel,
              -1);
    ui_footer("X reproducir   SELECT descargar   TRI quitar   O atras");
}

static void handle_pl_view(uint32_t pressed, float ly, int touch_x,
                           int touch_y, int touch_rising)
{
    int count;
    int activate = -1;

    if (g_ui.pl_view < 0 || g_ui.pl_view >= g_app.playlist_count) {
        g_ui.screen = APP_SCR_PLAYLISTS;
        return;
    }
    count = g_app.playlist_counts[g_ui.pl_view];

    if (pressed & SCE_CTRL_TRIANGLE) {
        /* Remove the selected track from this playlist. */
        if (g_ui.plist_sel < count) {
            app_playlist_remove_track(g_ui.pl_view, g_ui.plist_sel);
            if (g_ui.plist_sel >=
                    g_app.playlist_counts[g_ui.pl_view] &&
                g_ui.plist_sel > 0)
                g_ui.plist_sel--;
        }
        return;
    }
    if (pressed & SCE_CTRL_SELECT) {
        if (g_ui.plist_sel < count)
            app_download_offline(
                &g_app.playlist_tracks[g_ui.pl_view][g_ui.plist_sel]);
        return;
    }
    if (pressed & SCE_CTRL_CROSS && count > 0) {
        if (app_play_from_playlist(g_ui.pl_view, g_ui.plist_sel) == 0)
            open_screen(APP_SCR_NOW);
        return;
    }
    if (pressed & SCE_CTRL_CIRCLE) {
        g_ui.screen = APP_SCR_PLAYLISTS;
        return;
    }

    activate = list_handle(pressed, ly, count, &g_ui.plist_sel, touch_x,
                           touch_y, touch_rising);
    if (activate >= 0) {
        if (app_play_from_playlist(g_ui.pl_view, activate) == 0)
            open_screen(APP_SCR_NOW);
    }
}

/* -------------------------------------------------------------------------- */
/* queue                                                                       */
/* -------------------------------------------------------------------------- */

static void draw_queue(void)
{
    int playing = g_app.queue_index;
    char title[64];

    snprintf(title, sizeof(title), "Cola (%d)", g_app.queue_count);
    ui_text(PAD, LIST_TOP - 2, C_DIM, 0.7f, title);

    if (g_app.queue_count <= 0) {
        ui_text(PAD, LIST_TOP + 36, C_DIM, 0.85f,
                "No hay canciones en la cola.");
        ui_footer("O atras");
        return;
    }

    if (g_ui.queue_sel < 0)
        g_ui.queue_sel = 0;
    if (g_ui.queue_sel >= g_app.queue_count)
        g_ui.queue_sel = g_app.queue_count - 1;

    draw_list(g_app.queue, g_app.queue_count, g_ui.queue_sel, playing);
    ui_footer("X saltar   ^/v mover   O atras");
}

static void handle_queue(uint32_t pressed, float ly, int touch_x, int touch_y,
                         int touch_rising)
{
    int activate = -1;

    if (pressed & SCE_CTRL_CIRCLE) {
        open_screen(APP_SCR_NOW);
        return;
    }
    if (pressed & SCE_CTRL_CROSS) {
        if (g_app.queue_count > 0 &&
            app_play_from_queue(g_ui.queue_sel) == 0)
            open_screen(APP_SCR_NOW);
        return;
    }

    activate = list_handle(pressed, ly, g_app.queue_count, &g_ui.queue_sel,
                           touch_x, touch_y, touch_rising);
    if (activate >= 0) {
        if (app_play_from_queue(activate) == 0)
            open_screen(APP_SCR_NOW);
    }
}

/* -------------------------------------------------------------------------- */
/* settings                                                                   */
/* -------------------------------------------------------------------------- */

static void draw_settings(void)
{
    char vol[32];
    const char *repeat;
    int y;
    int acct_y;

    snprintf(vol, sizeof(vol), "Volumen: %d%%", g_app.volume);
    ui_text(PAD, LIST_TOP + 20, C_TEXT, 0.9f, vol);
    vita2d_draw_rectangle(PAD, LIST_TOP + 38, SCREEN_W - 2 * PAD, 8,
                          C_PANEL_HI);
    if (g_app.volume > 0)
        vita2d_draw_rectangle(PAD, LIST_TOP + 38,
                              (SCREEN_W - 2 * PAD) * g_app.volume / 100, 8,
                              C_BLUE);
    ui_text(PAD, LIST_TOP + 58, C_DIM, 0.7f, "L/R ajustan el volumen.");

    repeat = g_app.repeat_mode == REPEAT_ONE
                 ? "Repetir: una"
                 : (g_app.repeat_mode == REPEAT_ALL ? "Repetir: todas"
                                                    : "Repetir: off");
    ui_text(PAD, LIST_TOP + 100, C_TEXT, 0.9f, repeat);
    ui_text(PAD, LIST_TOP + 124, C_DIM, 0.7f,
            "X cambia el modo de repetici\xc3\xb3n.");

    y = LIST_TOP + 170;
    ui_text(PAD, y, C_DIM, 0.75f, "Cuenta");
    y += 28;
    acct_y = y;
    if (app_account_logged_in()) {
        char line[96];

        snprintf(line, sizeof(line), "Sesion: %s", app_account_name());
        ui_text_clip(PAD, y, SCREEN_W - 2 * PAD, C_ACCENT, 0.9f, line);
        y += 36;
        vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, 44,
                              g_ui.settings_sel == 0 ? C_PANEL_HI : C_PANEL);
        ui_text_box_center(PAD, y, SCREEN_W - 2 * PAD, 44, C_RED, 0.85f,
                           "Cerrar sesion");
    } else {
        ui_text(PAD, y, C_DIM, 0.85f, "No has iniciado sesion.");
        y += 36;
        vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, 44,
                              g_ui.settings_sel == 0 ? C_ACCENT : C_PANEL_HI);
        ui_text_box_center(PAD, y, SCREEN_W - 2 * PAD, 44,
                           g_ui.settings_sel == 0 ? C_BG : C_TEXT, 0.85f,
                           "Iniciar sesion");
    }

    y = LIST_TOP + 340;
    vita2d_draw_rectangle(PAD, y, SCREEN_W - 2 * PAD, 44,
                          g_ui.settings_sel == 1 ? C_PANEL_HI : C_PANEL);
    ui_text_box_center(PAD, y, SCREEN_W - 2 * PAD, 44, C_RED, 0.85f,
                       "Vaciar cache");

    ui_footer("L/R vol   X accion   TRI repetir   O atr\xc3\xa1s");
    (void)acct_y;
}

static void handle_settings(uint32_t pressed, int touch_x, int touch_y,
                            int touch_rising)
{
    int login_y = LIST_TOP + 170 + 28 + 36;
    int cache_y = LIST_TOP + 340;

    if (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) {
        if (pressed & SCE_CTRL_LTRIGGER)
            app_set_volume(g_app.volume - 5 > 0 ? g_app.volume - 5 : 0);
        else
            app_set_volume(g_app.volume + 5 < 100 ? g_app.volume + 5 : 100);
    }
    if (pressed & SCE_CTRL_UP)
        g_ui.settings_sel = 0;
    if (pressed & SCE_CTRL_DOWN)
        g_ui.settings_sel = 1;
    if (pressed & SCE_CTRL_CROSS) {
        if (g_ui.settings_sel == 0) {
            if (app_account_logged_in())
                app_account_logout();
            else
                open_account_ime();
        } else {
            app_clear_cache();
        }
    }
    if (pressed & SCE_CTRL_TRIANGLE)
        app_cycle_repeat();
    if (pressed & SCE_CTRL_CIRCLE)
        g_ui.screen = APP_SCR_HOME;

    if (touch_rising) {
        if (touch_y >= login_y && touch_y < login_y + 44 && touch_x >= PAD &&
            touch_x <= SCREEN_W - PAD) {
            g_ui.settings_sel = 0;
            if (app_account_logged_in())
                app_account_logout();
            else
                open_account_ime();
        } else if (touch_y >= cache_y && touch_y < cache_y + 44 &&
                   touch_x >= PAD && touch_x <= SCREEN_W - PAD) {
            g_ui.settings_sel = 1;
            app_clear_cache();
        }
    }
}

/* -------------------------------------------------------------------------- */
/* ui lifecycle                                                               */
/* -------------------------------------------------------------------------- */

void ui_init(void)
{
    memset(&g_ui, 0, sizeof(g_ui));
    g_ui.screen = APP_SCR_HOME;

    /* The HUD may already have loaded the font for the boot phases. */
    ui_hud_load();
    if (!g_pgf && !g_pvf)
        vm_log("ui: no system font available\n");
}

void ui_fini(void)
{
    int i;

    for (i = 0; i < VM_MAX_THUMBS; i++) {
        if (g_thumbs[i].texture)
            vita2d_free_texture(g_thumbs[i].texture);
        g_thumbs[i].texture = NULL;
    }
    if (g_pgf)
        vita2d_free_pgf(g_pgf);
    if (g_pvf)
        vita2d_free_pvf(g_pvf);
    g_pgf = NULL;
    g_pvf = NULL;
}

/* -------------------------------------------------------------------------- */
/* input                                                                      */
/* -------------------------------------------------------------------------- */

static int touch_point(int *x, int *y)
{
    SceTouchData touch;

    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 &&
        touch.reportNum > 0) {
        const SceTouchReport *r = &touch.report[0];

        *x = r->x >> 1;
        *y = r->y >> 1;
        return 1;
    }
    return 0;
}

void ui_frame(int *quit)
{
    SceCtrlData pad;
    uint32_t pressed;
    float ly = 128.0f;
    int touch_x = 0;
    int touch_y = 0;
    int touching;
    int touch_rising;

    g_tick++;
    g_thumb_loads_left = 1;

    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
        pressed = pad.buttons & ~g_ui.prev_buttons;
        g_ui.prev_buttons = pad.buttons;
        ly = (float)pad.ly;
    } else {
        pressed = 0;
    }

    touching = touch_point(&touch_x, &touch_y);
    touch_rising = touching && !g_ui.touch_was_down;
    g_ui.touch_was_down = touching;

    /* Tab gestures: a tap on the tab strip, or L/R on the list screens. */
    if (touch_rising && !g_ui.ime_active) {
        int tab = tab_hit(touch_x, touch_y);

        if (tab >= 0) {
            touch_rising = 0;
            activate_tab((ui_tab)tab);
        }
    }
    if (!g_ui.ime_active &&
        (g_ui.screen == APP_SCR_HOME || g_ui.screen == APP_SCR_RESULTS ||
         g_ui.screen == APP_SCR_LIBRARY || g_ui.screen == APP_SCR_FAVORITES ||
         g_ui.screen == APP_SCR_ACCOUNT || g_ui.screen == APP_SCR_LYRICS)) {
        if (pressed & SCE_CTRL_LTRIGGER)
            tab_switch(-1);
        else if (pressed & SCE_CTRL_RTRIGGER)
            tab_switch(+1);
    }

    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H, C_BG);

    switch (g_ui.screen) {
    case APP_SCR_HOME:
        draw_home();
        break;

    case APP_SCR_RESULTS:
        draw_results();
        break;

    case APP_SCR_LIBRARY:
        draw_library();
        break;

    case APP_SCR_FAVORITES:
        draw_favorites();
        break;

    case APP_SCR_PLAYLISTS:
        draw_playlists();
        break;

    case APP_SCR_PLVIEW:
        draw_pl_view();
        break;

    case APP_SCR_ACCOUNT:
        draw_account();
        break;

    case APP_SCR_NOW:
        draw_now();
        break;

    case APP_SCR_LYRICS:
        draw_lyrics();
        break;

    case APP_SCR_QUEUE:
        draw_queue();
        break;

    case APP_SCR_SETTINGS:
        draw_settings();
        break;

    default:
        g_ui.screen = APP_SCR_HOME;
        break;
    }

    draw_tabs();

    ui_toast();
    vita2d_end_drawing();

    /* Common-dialog update must run after drawing and before reading IME
     * status; otherwise Cancel/CIRCLE never finishes the dialog. */
    if (g_ui.ime_active)
        vita2d_common_dialog_update();

    vita2d_swap_buffers();

    if (g_ui.ime_active) {
        update_ime(pressed);
        return;
    }

    switch (g_ui.screen) {
    case APP_SCR_HOME:
        handle_home(pressed, ly, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_RESULTS:
        handle_results(pressed, ly, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_LIBRARY:
        handle_library(pressed);
        break;
    case APP_SCR_FAVORITES:
        handle_favorites(pressed, ly, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_PLAYLISTS:
        handle_playlists(pressed, ly, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_PLVIEW:
        handle_pl_view(pressed, ly, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_ACCOUNT:
        handle_account(pressed, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_NOW:
        handle_now(pressed, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_LYRICS:
        handle_lyrics(pressed);
        break;
    case APP_SCR_QUEUE:
        handle_queue(pressed, ly, touch_x, touch_y, touch_rising);
        break;
    case APP_SCR_SETTINGS:
        handle_settings(pressed, touch_x, touch_y, touch_rising);
        break;
    default:
        break;
    }
}