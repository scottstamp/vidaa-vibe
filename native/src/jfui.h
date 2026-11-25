#ifndef VIBE_JFUI_H
#define VIBE_JFUI_H
/*
 * The Jellyfin panel inside vibe-ui: server pick, Quick Connect sign-in,
 * Home (continue watching, next up, libraries), folder lists, item detail,
 * and playback through the vibe-play child.
 *
 * Threads: everything here runs on vibe-ui's main thread except the jobs
 * (network, JSON), which run on one worker thread started on first open
 * and post results back; jfui_tick() applies them. Nothing runs, and no
 * memory is taken, until the panel is first opened.
 *
 * Switch: /var/local/vibe/jf.off hides the panel's entry (checked at start).
 */
#include <directfb.h>
#include "gfx.h"

struct jfui_host {
     IDirectFBSurface *s;
     IDirectFBFont *f_title, *f_head, *f_item, *f_bold, *f_small, *f_tiny;
     int W, H;
     void *ctx;
     /* vibe-ui's own drawing helpers (see vibe-ui.c) */
     void (*text)(void *ctx, IDirectFBFont *f, int x, int box_y, int box_h,
                  uint32_t colour, const char *str, int align);
     void (*focus)(struct canvas *c, int x, int y, int w, int h, int r);
     void (*hints)(void *ctx, struct canvas *c, int cx, int y, const char *const *pairs, int n);
     void (*sheet)(void *ctx, struct canvas *c, int x, int y, int w, int h, const char *title);
};

/* 1 unless switched off */
int  jfui_available(void);
/* show the panel (first call starts the worker and loads the session) */
void jfui_open(const struct jfui_host *h, double now);
/* hide the panel; stops playback (reporting the position first) */
void jfui_close(double now);
/* playback running: vibe-ui keeps the panel's screen and draws nothing of
 * its own but HUDs */
int  jfui_playing(void);

/* one layout pass: c = shapes, NULL = text */
void jfui_draw(struct canvas *c, double now);
/* a key while the panel is shown; returns 0 when the panel wants to close
 * (BACK on its first screen) */
int  jfui_key(int key, int repeat, double now);
/* call every loop: applies job results, reads the player; returns 1 when
 * the panel needs a redraw */
int  jfui_tick(double now);

#endif
