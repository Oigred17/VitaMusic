/*
 * VitaMusic - vita2d front end.
 *
 * Everything is drawn immediately each frame; the only persistent view state
 * is the current screen, the selected row and a small LRU cache of decoded
 * album art. Input comes from the pad and the front touchscreen.
 */
#ifndef VM_UI_H
#define VM_UI_H

/* Call after vita2d_init(). */
void ui_init(void);
void ui_fini(void);

/* Draws one frame and handles input. Sets *quit when the user leaves. */
void ui_frame(int *quit);

/* Boot diagnostics: a tiny on-screen status line drawn even before the app
 * state is up, so a hang anywhere during startup leaves the last phase
 * visible instead of a black screen. */
void ui_hud_load(void);
void ui_hud_show(const char *phase);

#endif /* VM_UI_H */
