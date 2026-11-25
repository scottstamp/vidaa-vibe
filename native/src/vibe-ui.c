/*
 * vibe-ui - the native UI for the 58A51HUV.
 *
 * One process, three vendor services:
 *   - DirectFB (world 0, slave): one full-screen ARGB window that keeps
 *     focus. Where nothing is drawn it is fully transparent, so the HDMI
 *     picture underneath stays visible.
 *   - the kilby model (biz.c): inputs, volume, mute, picture settings,
 *     input names, gui.loop.finished.
 *   - app_man (gam.c), only with --gam: registers as a client named "vibe"
 *     and announces itself "started", as the vendor sample UI does.
 *
 * Screens and keys (DirectFB symbols measured with `vibe keys`):
 *   nothing shown   HOME opens Home, MENU opens Picture, INPUT the quick
 *                   input panel; VOLUME / MUTE show the volume bar
 *   Home            Inputs row and Settings row; arrows move, OK picks,
 *                   BACK / HOME / EXIT close
 *   Quick inputs    UP / DOWN (or INPUT again), OK switches
 *   Picture         UP / DOWN pick a row, LEFT / RIGHT change it, live
 *   Rename          pick an input, then a name
 *   POWER           left to the system (see gui.loop.finished below)
 * Key repeats that the IR device re-injects with key_code -1 are dropped.
 *
 * Meant to run with the stock engine booted from slot B (native/slotb: an
 * empty page), so nothing else draws or acts on keys.
 *
 * usage: vibe-ui [--user=UID:GID[:G,...]] [--timeout=S] [--layer=N] [--gam]
 *   --timeout: exit after S seconds (default 600, 0 = never). A watchdog
 *   alarm kills the process 10 s after it.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <directfb.h>

#include "biz.h"
#include "common.h"
#include "gam.h"

#define FONT_REG  "/opt/font/Cobalt_fonts/Roboto-Regular.ttf"
#define FONT_BOLD "/opt/font/Cobalt_fonts/Roboto-Bold.ttf"

/* remote keys without a DirectFB name (vibe keys, 2026-11-25) */
#define KEY_INPUT  0xf068

/* colours (ARGB) */
#define C_SCRIM    0xd80b0f18
#define C_PANEL    0xf0101622
#define C_CARD     0xff1d2a40
#define C_FOCUS    0xff2a6fdb
#define C_TEXT     0xffffffff
#define C_DIM      0xffa0a8b8
#define C_GOOD     0xff9ff0b0
#define C_TRACK    0xff3a4458

static volatile sig_atomic_t stop_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

enum screen { S_NONE, S_HOME, S_QUICK, S_PICTURE, S_RENAME_INPUT, S_RENAME_NAME };

static const char *const picture_modes[] = {
     "Dynamic", "Standard", "Cinema Night", "Cinema Day", "Game", "Sport", "Calibrated"
};
#define N_MODES 7

/* picture rows: 0 = mode, the rest numeric 0..100 */
static const struct { const char *name, *key; } pic_rows[] = {
     { "Picture mode", NULL },
     { "Backlight",  BIZ_KEY_BACKLIGHT },
     { "Brightness", BIZ_KEY_BRIGHTNESS },
     { "Contrast",   BIZ_KEY_CONTRAST },
     { "Colour",     BIZ_KEY_COLOUR },
     { "Sharpness",  BIZ_KEY_SHARPNESS },
};
#define N_PIC 6

static const char *const name_presets[] = {
     NULL /* default: the input's own id */, "Apple TV", "PlayStation", "Xbox",
     "Nintendo Switch", "PC", "Chromecast", "Fire TV", "Roku", "Blu-ray",
     "Cable box", "Game console",
};
#define N_PRESETS 12

struct app {
     IDirectFB             *dfb;
     IDirectFBDisplayLayer *layer;
     IDirectFBWindow       *win;
     IDirectFBSurface      *s;
     IDirectFBEventBuffer  *events;
     IDirectFBFont         *f_title, *f_item, *f_bold, *f_small;
     int W, H;

     /* model state */
     struct biz_input in[BIZ_MAX_INPUTS];
     int  nin;
     int  vis[BIZ_MAX_INPUTS], nvis;      /* indexes of inputs we show */
     char cur[32];
     int  vol, mute, vol_known;
     int  pic[N_PIC], pic_known;

     /* UI state */
     enum screen screen;
     int  row, col;              /* Home focus */
     int  sel;                   /* list selection (quick, picture, rename) */
     int  rename_idx;            /* input being renamed (index into in[]) */
     int  top;                   /* first visible row of a long list */
     char toast[64];
     double toast_until, vol_until;
     double next_inputs, next_vol, next_pic;
     int  dirty;
     int  gam;
};

#define CHECK(what, call)                                                    \
     do {                                                                    \
          DFBResult r_ = (call);                                             \
          if (r_ != DFB_OK) {                                                \
               fprintf(stderr, "vibe-ui: %s failed: %s\n", what,             \
                       DirectFBErrorString(r_));                             \
               return -1;                                                    \
          }                                                                  \
     } while (0)

/* ------------------------------------------------------------- drawing */

static void fill(struct app *a, int x, int y, int w, int h, u32 argb)
{
     a->s->SetDrawingFlags(a->s, DSDRAW_NOFX);
     a->s->SetColor(a->s, argb >> 16 & 0xff, argb >> 8 & 0xff, argb & 0xff, argb >> 24);
     a->s->FillRectangle(a->s, x, y, w, h);
}

static void text(struct app *a, IDirectFBFont *f, int x, int y, u32 argb,
                 const char *str, DFBSurfaceTextFlags flags)
{
     a->s->SetFont(a->s, f);
     a->s->SetColor(a->s, argb >> 16 & 0xff, argb >> 8 & 0xff, argb & 0xff, argb >> 24);
     a->s->DrawString(a->s, str, -1, x, y, flags);
}

static int text_width(IDirectFBFont *f, const char *str)
{
     int w = 0;
     f->GetStringWidth(f, str, -1, &w);
     return w;
}

static void bar(struct app *a, int x, int y, int w, int v)
{
     v = v < 0 ? 0 : v > 100 ? 100 : v;
     fill(a, x, y, w, 10, C_TRACK);
     fill(a, x, y, w * v / 100, 10, C_FOCUS);
}

static const char *input_label(struct app *a, int i)
{
     return a->in[i].label[0] ? a->in[i].label : a->in[i].id;
}

static void draw_hint(struct app *a, int x, const char *hint)
{
     text(a, a->f_small, x, a->H - 90, C_DIM, hint, DSTF_TOPLEFT);
}

/* Home: Inputs row, Settings row */
#define CARD_W 320
#define CARD_H 190
#define CARD_GAP 32
#define HOME_X 96

static const char *const settings_cards[] = { "Picture", "Rename inputs" };
#define N_SETTINGS 2

static void card(struct app *a, int x, int y, int focus, const char *title,
                 const char *sub, u32 sub_colour, const char *tag)
{
     fill(a, x, y, CARD_W, CARD_H, focus ? C_FOCUS : C_CARD);
     text(a, a->f_bold, x + 28, y + 32, C_TEXT, title, DSTF_TOPLEFT);
     if (sub)
          text(a, a->f_small, x + 28, y + 100, sub_colour, sub, DSTF_TOPLEFT);
     if (tag)
          text(a, a->f_small, x + 28, y + 140, focus ? C_TEXT : C_DIM, tag, DSTF_TOPLEFT);
}

static void draw_home(struct app *a)
{
     char sub[96];
     int ci = -1;

     fill(a, 0, 0, a->W, a->H, C_SCRIM);
     text(a, a->f_title, HOME_X, 72, C_TEXT, "Home", DSTF_TOPLEFT);
     for (int i = 0; i < a->nin; i++)
          if (!strcmp(a->in[i].id, a->cur))
               ci = i;
     snprintf(sub, sizeof(sub), "Now showing: %s", ci >= 0 ? input_label(a, ci) : "-");
     text(a, a->f_item, HOME_X, 160, C_DIM, sub, DSTF_TOPLEFT);

     text(a, a->f_small, HOME_X, 250, C_DIM, "INPUTS", DSTF_TOPLEFT);
     for (int k = 0; k < a->nvis; k++) {
          struct biz_input *in = &a->in[a->vis[k]];
          int x = HOME_X + k * (CARD_W + CARD_GAP);
          card(a, x, 300, a->row == 0 && a->col == k, input_label(a, a->vis[k]),
               in->signal ? "signal" : "no signal", in->signal ? C_GOOD : C_DIM,
               !strcmp(in->id, a->cur) ? "now showing" : NULL);
     }

     text(a, a->f_small, HOME_X, 570, C_DIM, "SETTINGS", DSTF_TOPLEFT);
     for (int k = 0; k < N_SETTINGS; k++) {
          int x = HOME_X + k * (CARD_W + CARD_GAP);
          card(a, x, 620, a->row == 1 && a->col == k, settings_cards[k], NULL, 0, NULL);
     }
     draw_hint(a, HOME_X, "Arrows  move        OK  select        BACK  close");
}

/* right-hand panel used by Picture and Rename */
#define PANEL_W 760

static int panel_x(struct app *a) { return a->W - PANEL_W; }

static void panel(struct app *a, const char *title)
{
     int x = panel_x(a);
     fill(a, x, 0, PANEL_W, a->H, C_PANEL);
     fill(a, x - 4, 0, 4, a->H, C_FOCUS);
     text(a, a->f_title, x + 64, 72, C_TEXT, title, DSTF_TOPLEFT);
}

static void draw_picture(struct app *a)
{
     int x = panel_x(a) + 64, w = PANEL_W - 128, row_h = 118, top = 200;

     panel(a, "Picture");
     for (int i = 0; i < N_PIC; i++) {
          int y = top + i * row_h;
          char val[48];
          if (i == a->sel)
               fill(a, panel_x(a) + 32, y - 12, PANEL_W - 64, row_h - 12, C_CARD);
          text(a, a->f_item, x, y, C_TEXT, pic_rows[i].name, DSTF_TOPLEFT);
          if (!a->pic_known)
               snprintf(val, sizeof(val), "...");
          else if (i == 0)
               snprintf(val, sizeof(val), "%s%s%s", i == a->sel ? "<  " : "",
                        a->pic[0] >= 0 && a->pic[0] < N_MODES ? picture_modes[a->pic[0]] : "?",
                        i == a->sel ? "  >" : "");
          else
               snprintf(val, sizeof(val), "%d", a->pic[i]);
          text(a, a->f_item, x + w - text_width(a->f_item, val), y, i == a->sel ? C_TEXT : C_DIM,
               val, DSTF_TOPLEFT);
          if (i > 0 && a->pic_known)
               bar(a, x, y + 66, w, a->pic[i]);
     }
     draw_hint(a, panel_x(a) + 64, "UP / DOWN  choose     LEFT / RIGHT  change     BACK");
}

/* generic scrolling list on the right panel */
#define LIST_ROW_H 84
#define LIST_TOP   200
#define LIST_ROWS  9

static void draw_list(struct app *a, const char *title, int n,
                      const char *(*label)(struct app *, int), const char *(*sub)(struct app *, int))
{
     int x = panel_x(a) + 64;

     panel(a, title);
     if (a->sel < a->top) a->top = a->sel;
     if (a->sel >= a->top + LIST_ROWS) a->top = a->sel - LIST_ROWS + 1;
     for (int i = a->top; i < n && i < a->top + LIST_ROWS; i++) {
          int y = LIST_TOP + (i - a->top) * LIST_ROW_H;
          const char *s2 = sub ? sub(a, i) : NULL;
          if (i == a->sel)
               fill(a, panel_x(a) + 32, y - 10, PANEL_W - 64, LIST_ROW_H - 8, C_FOCUS);
          text(a, a->f_item, x, y, C_TEXT, label(a, i), DSTF_TOPLEFT);
          if (s2)
               text(a, a->f_small, x + PANEL_W - 128 - text_width(a->f_small, s2), y + 8,
                    i == a->sel ? C_TEXT : C_DIM, s2, DSTF_TOPLEFT);
     }
     draw_hint(a, x, "UP / DOWN  choose        OK  select        BACK");
}

static const char *rename_input_label(struct app *a, int i) { return input_label(a, a->vis[i]); }
static const char *rename_input_sub(struct app *a, int i)   { return a->in[a->vis[i]].id; }

static const char *preset_label(struct app *a, int i)
{
     static char buf[64];
     if (name_presets[i])
          return name_presets[i];
     snprintf(buf, sizeof(buf), "Default (%s)", a->in[a->rename_idx].id);
     return buf;
}

static void draw_quick(struct app *a)
{
     const int pw = 640, x0 = 64, row = 112, top = 220;

     fill(a, 0, 0, pw, a->H, C_PANEL);
     fill(a, pw, 0, 4, a->H, C_FOCUS);
     text(a, a->f_title, x0, 72, C_TEXT, "Inputs", DSTF_TOPLEFT);
     for (int k = 0; k < a->nvis; k++) {
          struct biz_input *in = &a->in[a->vis[k]];
          int y = top + k * row, cur = !strcmp(in->id, a->cur);
          char sub[64];
          if (k == a->sel)
               fill(a, 32, y, pw - 64, row - 16, C_FOCUS);
          else if (cur)
               fill(a, 32, y, pw - 64, row - 16, C_CARD);
          text(a, a->f_item, x0, y + 18, C_TEXT, input_label(a, a->vis[k]), DSTF_TOPLEFT);
          snprintf(sub, sizeof(sub), "%s%s", in->signal ? "signal" : "no signal",
                   cur ? "   \xc2\xb7   now showing" : "");
          text(a, a->f_small, x0, y + 64, in->signal ? C_GOOD : C_DIM, sub, DSTF_TOPLEFT);
     }
     draw_hint(a, x0, "OK  switch        BACK  close");
}

static void draw_toast(struct app *a)
{
     int w = 520, h = 120, x = 64, y = 64;
     fill(a, x, y, w, h, 0xe0101622);
     fill(a, x, y, 8, h, C_FOCUS);
     text(a, a->f_title, x + 40, y + h / 2, C_TEXT, a->toast, DSTF_LEFT);
}

static void draw_volume(struct app *a)
{
     int w = 760, h = 120, x = (a->W - w) / 2, y = a->H - 220;
     char label[32];

     fill(a, x, y, w, h, 0xe0101622);
     snprintf(label, sizeof(label), a->mute ? "Muted" : "%d", a->vol);
     text(a, a->f_title, x + 40, y + h / 2, C_TEXT, label, DSTF_LEFT);
     fill(a, x + 220, y + h / 2 - 6, w - 260, 12, C_TRACK);
     if (!a->mute) {
          int v = a->vol < 0 ? 0 : a->vol > 100 ? 100 : a->vol;
          fill(a, x + 220, y + h / 2 - 6, (w - 260) * v / 100, 12, C_FOCUS);
     }
}

static void redraw(struct app *a, double now)
{
     a->s->Clear(a->s, 0, 0, 0, 0);    /* transparent: the picture shows */
     switch (a->screen) {
     case S_HOME:         draw_home(a); break;
     case S_QUICK:        draw_quick(a); break;
     case S_PICTURE:      draw_picture(a); break;
     case S_RENAME_INPUT: draw_list(a, "Rename inputs", a->nvis, rename_input_label, rename_input_sub); break;
     case S_RENAME_NAME:  draw_list(a, input_label(a, a->rename_idx), N_PRESETS, preset_label, NULL); break;
     case S_NONE:         break;
     }
     if (now < a->toast_until)
          draw_toast(a);
     if (now < a->vol_until)
          draw_volume(a);
     a->s->Flip(a->s, NULL, DSFLIP_WAITFORSYNC);
     a->dirty = 0;
}

/* --------------------------------------------------------------- model */

static void poll_inputs(struct app *a)
{
     struct biz_input in[BIZ_MAX_INPUTS];
     char cur[32] = "";
     int n = biz_inputs(in, BIZ_MAX_INPUTS);

     biz_current_input(cur, sizeof(cur));
     if (n > 0 && (n != a->nin || memcmp(in, a->in, n * sizeof(in[0])))) {
          memcpy(a->in, in, n * sizeof(in[0]));
          a->nin = n;
          a->nvis = 0;
          for (int i = 0; i < n; i++)
               if (strcmp(in[i].id, "TV"))      /* the tuner is not used */
                    a->vis[a->nvis++] = i;
          a->dirty = a->screen != S_NONE;
     }
     if (cur[0] && strcmp(cur, a->cur)) {
          printf("current input: %s\n", cur);
          snprintf(a->cur, sizeof(a->cur), "%s", cur);
          a->dirty = a->screen != S_NONE;
     }
}

static void poll_volume(struct app *a, double now)
{
     int vol = a->vol, mute = a->mute;

     biz_get_int(BIZ_KEY_VOLUME, &vol);
     biz_get_int(BIZ_KEY_MUTE, &mute);
     if (a->vol_known && (vol != a->vol || mute != a->mute)) {
          printf("volume %d mute %d\n", vol, mute);
          a->vol_until = now + 2.5;
          a->dirty = 1;
     }
     a->vol = vol;
     a->mute = mute;
     a->vol_known = 1;
}

static void poll_picture(struct app *a)
{
     int v[N_PIC];
     biz_get_int(BIZ_KEY_PICTURE_MODE, &v[0]);
     for (int i = 1; i < N_PIC; i++)
          biz_get_int(pic_rows[i].key, &v[i]);
     if (!a->pic_known || memcmp(v, a->pic, sizeof(v))) {
          memcpy(a->pic, v, sizeof(v));
          a->pic_known = 1;
          a->dirty = a->screen == S_PICTURE;
     }
}

/* ---------------------------------------------------------------- keys */

static void open_screen(struct app *a, enum screen s, double now)
{
     a->screen = s;
     a->top = 0;
     a->dirty = 1;
     if (s == S_QUICK) {
          a->sel = 0;
          for (int k = 0; k < a->nvis; k++)
               if (!strcmp(a->in[a->vis[k]].id, a->cur))
                    a->sel = k;
          a->next_inputs = now;
     } else if (s == S_HOME) {
          a->next_inputs = now;
     } else if (s == S_PICTURE) {
          a->sel = 0;
          a->next_pic = now;
     } else if (s == S_RENAME_INPUT) {
          a->sel = 0;
     } else if (s == S_RENAME_NAME) {
          a->sel = 0;
     }
}

static void toast(struct app *a, const char *msg, double now)
{
     snprintf(a->toast, sizeof(a->toast), "%s", msg);
     a->toast_until = now + 3;
     a->dirty = 1;
}

static void switch_to(struct app *a, int idx, double now)
{
     struct biz_input *in = &a->in[idx];
     if (strcmp(in->id, a->cur)) {
          int r = biz_switch_input(in->id);
          printf("switch to %s -> r=%d\n", in->id, r);
          a->next_inputs = now + 0.5;
     }
     toast(a, input_label(a, idx), now);
}

static void list_move(struct app *a, int n, int d)
{
     int s = a->sel + d;
     if (s >= 0 && s < n) {
          a->sel = s;
          a->dirty = 1;
     }
}

static void key_home(struct app *a, int k, double now)
{
     int ncol = a->row == 0 ? a->nvis : N_SETTINGS;
     switch (k) {
     case DIKS_CURSOR_LEFT:  if (a->col > 0) { a->col--; a->dirty = 1; } break;
     case DIKS_CURSOR_RIGHT: if (a->col + 1 < ncol) { a->col++; a->dirty = 1; } break;
     case DIKS_CURSOR_UP:
          if (a->row > 0) { a->row = 0; if (a->col >= a->nvis) a->col = a->nvis - 1; a->dirty = 1; }
          break;
     case DIKS_CURSOR_DOWN:
          if (a->row < 1) { a->row = 1; if (a->col >= N_SETTINGS) a->col = N_SETTINGS - 1; a->dirty = 1; }
          break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (a->row == 0 && a->col < a->nvis) {
               switch_to(a, a->vis[a->col], now);
               a->screen = S_NONE;
               a->dirty = 1;
          } else if (a->row == 1) {
               open_screen(a, a->col == 0 ? S_PICTURE : S_RENAME_INPUT, now);
          }
          break;
     case DIKS_BACK: case DIKS_EXIT:
          a->screen = S_NONE; a->dirty = 1;
          break;
     }
}

static void key_quick(struct app *a, int k, double now)
{
     switch (k) {
     case DIKS_CURSOR_UP:   list_move(a, a->nvis, -1); break;
     case DIKS_CURSOR_DOWN: list_move(a, a->nvis, +1); break;
     case KEY_INPUT:
          if (a->nvis) { a->sel = (a->sel + 1) % a->nvis; a->dirty = 1; }
          break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (a->sel < a->nvis)
               switch_to(a, a->vis[a->sel], now);
          a->screen = S_NONE; a->dirty = 1;
          break;
     case DIKS_BACK: case DIKS_EXIT:
          a->screen = S_NONE; a->dirty = 1;
          break;
     }
}

static void key_picture(struct app *a, int k, int repeat, double now)
{
     int d = 0;
     switch (k) {
     case DIKS_CURSOR_UP:    list_move(a, N_PIC, -1); return;
     case DIKS_CURSOR_DOWN:  list_move(a, N_PIC, +1); return;
     case DIKS_CURSOR_LEFT:  d = -1; break;
     case DIKS_CURSOR_RIGHT: d = +1; break;
     case DIKS_BACK:
          open_screen(a, S_HOME, now); a->row = 1; a->col = 0;
          return;
     case DIKS_EXIT:
          a->screen = S_NONE; a->dirty = 1;
          return;
     default:
          return;
     }
     if (!a->pic_known)
          return;
     if (a->sel == 0) {
          if (repeat)
               return;                  /* one mode per press */
          int m = (a->pic[0] + d + N_MODES) % N_MODES;
          int r = biz_set_picture_mode(m);
          printf("picture mode %d (%s) -> r=%d\n", m, picture_modes[m], r);
          a->pic[0] = m;
          a->next_pic = now + 0.6;      /* a mode change moves the other values */
     } else {
          int v = a->pic[a->sel] + d * (repeat ? 2 : 1);
          v = v < 0 ? 0 : v > 100 ? 100 : v;
          int r = biz_set_int(pic_rows[a->sel].key, v);
          printf("%s %d -> r=%d\n", pic_rows[a->sel].name, v, r);
          a->pic[a->sel] = v;
          a->next_pic = now + 1.0;
     }
     a->dirty = 1;
}

static void key_rename(struct app *a, int k, double now)
{
     int n = a->screen == S_RENAME_INPUT ? a->nvis : N_PRESETS;
     switch (k) {
     case DIKS_CURSOR_UP:   list_move(a, n, -1); break;
     case DIKS_CURSOR_DOWN: list_move(a, n, +1); break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (a->screen == S_RENAME_INPUT) {
               if (a->sel < a->nvis) {
                    a->rename_idx = a->vis[a->sel];
                    open_screen(a, S_RENAME_NAME, now);
               }
          } else {
               const char *id = a->in[a->rename_idx].id;
               const char *name = name_presets[a->sel] ? name_presets[a->sel] : id;
               int r = biz_rename_input(id, name);
               char msg[64];
               printf("rename %s -> '%s' r=%d\n", id, name, r);
               snprintf(msg, sizeof(msg), "%s: %s", id, name);
               toast(a, msg, now);
               a->next_inputs = now + 0.5;
               open_screen(a, S_RENAME_INPUT, now);
          }
          break;
     case DIKS_BACK:
          if (a->screen == S_RENAME_NAME) open_screen(a, S_RENAME_INPUT, now);
          else { open_screen(a, S_HOME, now); a->row = 1; a->col = 1; }
          break;
     case DIKS_EXIT:
          a->screen = S_NONE; a->dirty = 1;
          break;
     }
}

static void on_key(struct app *a, int k, int repeat, double now)
{
     /* keys that mean the same everywhere */
     switch (k) {
     case DIKS_POWER:
     case DIKS_POWER2:
          /* handled natively (app_man -> apm_bond) once gui.loop.finished
           * is 1; in practice it never reaches us */
          printf("power key: left to the system\n");
          return;
     case DIKS_VOLUME_UP:
     case DIKS_VOLUME_DOWN:
     case DIKS_MUTE: {
          /* nothing else acts on these with the stock page blanked; the stock
           * UI used to call modeljs.sound.setMainVolume */
          int r;
          if (k == DIKS_MUTE) {
               a->mute = !a->mute;
               r = biz_set_int(BIZ_KEY_MUTE, a->mute);
          } else {
               int v = a->vol + (k == DIKS_VOLUME_UP ? 1 : -1);
               a->vol = v < 0 ? 0 : v > 100 ? 100 : v;
               if (a->mute) {               /* volume keys unmute */
                    a->mute = 0;
                    biz_set_int(BIZ_KEY_MUTE, 0);
               }
               r = biz_set_int(BIZ_KEY_VOLUME, a->vol);
          }
          printf("volume %d mute %d (set r=%d)\n", a->vol, a->mute, r);
          a->vol_until = now + 2.5;
          a->next_vol = now + 0.5;
          a->dirty = 1;
          return;
     }
     case DIKS_HOME:
          if (a->screen == S_NONE) { open_screen(a, S_HOME, now); a->row = 0; a->col = 0; }
          else { a->screen = S_NONE; a->dirty = 1; }
          return;
     case DIKS_MENU:
          if (a->screen == S_PICTURE) { a->screen = S_NONE; a->dirty = 1; }
          else open_screen(a, S_PICTURE, now);
          return;
     case KEY_INPUT:
          if (a->screen != S_QUICK) { open_screen(a, S_QUICK, now); return; }
          break;
     }
     if (repeat && a->screen != S_PICTURE &&
         k != DIKS_CURSOR_UP && k != DIKS_CURSOR_DOWN &&
         k != DIKS_CURSOR_LEFT && k != DIKS_CURSOR_RIGHT)
          return;
     switch (a->screen) {
     case S_HOME:         key_home(a, k, now); break;
     case S_QUICK:        key_quick(a, k, now); break;
     case S_PICTURE:      key_picture(a, k, repeat, now); break;
     case S_RENAME_INPUT:
     case S_RENAME_NAME:  key_rename(a, k, now); break;
     case S_NONE:         break;
     }
}

/* ---------------------------------------------------------------- GAM */

static void gam_log(const char *tag, int type, const void *data, int len)
{
     printf("[gam %s] type=0x%x len=%d", tag, type, len);
     if (data && len > 0) {
          const unsigned char *p = data;
          printf(" '");
          for (int i = 0; i < len && i < 160; i++)
               putchar(p[i] >= 32 && p[i] < 127 ? p[i] : '.');
          printf("'");
     }
     printf("\n");
}
static void gam_on_sync(int type, const void *data, int len)  { gam_log("sync", type, data, len); }
static void gam_on_async(int type, const void *data, int len) { gam_log("async", type, data, len); }

/* ---------------------------------------------------------------- setup */

static int font(struct app *a, const char *path, int height, IDirectFBFont **out)
{
     PADDED(DFBFontDescription, fdp);
     memset(&fdp, 0, sizeof(fdp));
     fdp.v.flags = DFDESC_HEIGHT;
     fdp.v.height = height;
     CHECK(path, a->dfb->CreateFont(a->dfb, path, &fdp.v, out));
     return 0;
}

static int setup(struct app *a, int layer_id)
{
     PADDED(DFBDisplayLayerConfig, cfg);
     PADDED(DFBWindowDescription, wd);
     int dargc = 1;
     char *dargv_store[] = { "vibe-ui", NULL }, **dargv = dargv_store;

     CHECK("DirectFBInit", DirectFBInit(&dargc, &dargv));
     /* join world 0 as a slave, never start a new master */
     CHECK("session", DirectFBSetOption("session", "0"));
     CHECK("force-slave", DirectFBSetOption("force-slave", NULL));
     CHECK("no-banner", DirectFBSetOption("no-banner", NULL));
     CHECK("DirectFBCreate", DirectFBCreate(&a->dfb));

     CHECK("GetDisplayLayer", a->dfb->GetDisplayLayer(a->dfb, layer_id, &a->layer));
     memset(&cfg, 0, sizeof(cfg));
     CHECK("GetConfiguration", a->layer->GetConfiguration(a->layer, &cfg.v));
     a->W = cfg.v.width;
     a->H = cfg.v.height;
     if (a->W < 1280 || a->H < 720 || a->W > 4096 || a->H > 4096) {
          fprintf(stderr, "vibe-ui: layer size %dx%d looks wrong\n", a->W, a->H);
          return -1;
     }

     memset(&wd, 0, sizeof(wd));
     wd.v.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
                  DWDESC_CAPS | DWDESC_PIXELFORMAT | DWDESC_STACKING;
     wd.v.width = a->W;
     wd.v.height = a->H;
     wd.v.caps = DWCAPS_ALPHACHANNEL | DWCAPS_DOUBLEBUFFER;
     wd.v.pixelformat = DSPF_ARGB;
     wd.v.stacking = DWSC_UPPER;
     CHECK("CreateWindow", a->layer->CreateWindow(a->layer, &wd.v, &a->win));
     CHECK("GetSurface", a->win->GetSurface(a->win, &a->s));
     CHECK("CreateEventBuffer", a->win->CreateEventBuffer(a->win, &a->events));

     if (font(a, FONT_BOLD, 64, &a->f_title) || font(a, FONT_REG, 40, &a->f_item) ||
         font(a, FONT_BOLD, 40, &a->f_bold) || font(a, FONT_REG, 28, &a->f_small))
          return -1;

     a->s->Clear(a->s, 0, 0, 0, 0);
     a->s->Flip(a->s, NULL, 0);
     CHECK("SetOpacity", a->win->SetOpacity(a->win, 0xff));
     a->win->RaiseToTop(a->win);
     printf("RequestFocus: %s\n", DirectFBErrorString(a->win->RequestFocus(a->win)));
     printf("window %dx%d on layer %d\n", a->W, a->H, layer_id);
     return 0;
}

static void teardown(struct app *a)
{
     if (a->win) a->win->SetOpacity(a->win, 0);
     if (a->f_small) a->f_small->Release(a->f_small);
     if (a->f_bold) a->f_bold->Release(a->f_bold);
     if (a->f_item) a->f_item->Release(a->f_item);
     if (a->f_title) a->f_title->Release(a->f_title);
     if (a->events) a->events->Release(a->events);
     if (a->s) a->s->Release(a->s);
     if (a->win) { a->win->Destroy(a->win); a->win->Release(a->win); }
     if (a->layer) a->layer->Release(a->layer);
     if (a->dfb) a->dfb->Release(a->dfb);
}

/* ----------------------------------------------------------------- main */

int main(int argc, char **argv)
{
     static struct app app;
     struct app *a = &app;
     const char *user = NULL;
     int timeout = 600, layer = DLID_PRIMARY;
     struct sigaction sa;
     double t0, deadline, last_toast_end = 0, last_vol_end = 0;

     for (int i = 1; i < argc; i++) {
          if (!strncmp(argv[i], "--user=", 7))          user = argv[i] + 7;
          else if (!strncmp(argv[i], "--timeout=", 10)) timeout = atoi(argv[i] + 10);
          else if (!strncmp(argv[i], "--layer=", 8))    layer = atoi(argv[i] + 8);
          else if (!strcmp(argv[i], "--gam"))           a->gam = 1;
          else {
               fprintf(stderr, "usage: vibe-ui [--user=UID:GID[:G,...]] [--timeout=S] [--layer=N] [--gam]\n");
               return 2;
          }
     }
     setvbuf(stdout, NULL, _IOLBF, 0);
     memset(&sa, 0, sizeof(sa));
     sa.sa_handler = on_signal;
     sigaction(SIGTERM, &sa, NULL);
     sigaction(SIGINT, &sa, NULL);
     sigaction(SIGHUP, &sa, NULL);
     if (timeout > 0)
          alarm((unsigned)timeout + 10);   /* default action kills us */

     if (user && vibe_drop_privileges(user))
          return 1;

     t0 = vibe_now_s();
     if (biz_open())
          return 1;
     /* tell the system the UI is up, as the stock SDK does at the end of
      * its init (uiLoadFinish(1)); without it POWER does nothing */
     {
          int v = -1, r;
          biz_get_int(BIZ_KEY_GUI_LOOP_FINISHED, &v);
          r = biz_set_int(BIZ_KEY_GUI_LOOP_FINISHED, 1);
          printf("gui.loop.finished was %d, set 1 -> r=%d\n", v, r);
     }
     poll_inputs(a);
     poll_volume(a, t0);
     poll_picture(a);
     printf("model: %d inputs, current %s, volume %d, mute %d, picture mode %d (%.0f ms)\n",
            a->nin, a->cur, a->vol, a->mute, a->pic[0], (vibe_now_s() - t0) * 1000);

     if (a->gam) {
          if (gam_open("vibe", gam_on_sync, gam_on_async) == 0) {
               int r = gam_send_cmd("am", "started", "vibe");
               printf("gam: client 'vibe' up, started -> r=%d\n", r);
          } else {
               printf("gam: not available, going on without it\n");
               a->gam = 0;
          }
     }

     if (setup(a, layer)) {
          teardown(a);
          if (a->gam) gam_close();
          biz_close();
          return 1;
     }

     deadline = timeout > 0 ? vibe_now_s() + timeout : 0;
     while (!stop_flag) {
          PADDED(DFBEvent, ev);
          double now = vibe_now_s();

          if (deadline && now >= deadline) {
               printf("timeout\n");
               break;
          }
          a->events->WaitForEventWithTimeout(a->events, 0, 100);
          now = vibe_now_s();
          while (a->events->GetEvent(a->events, &ev.v) == DFB_OK) {
               DFBWindowEvent *we = &ev.v.window;
               if (ev.v.clazz != DFEC_WINDOW)
                    continue;
               if (we->type == DWET_GOTFOCUS || we->type == DWET_LOSTFOCUS) {
                    printf("%s\n", we->type == DWET_GOTFOCUS ? "got focus" : "lost focus");
                    continue;
               }
               if (we->type != DWET_KEYDOWN)
                    continue;
               if (we->key_code < 0)    /* IR re-injection duplicate */
                    continue;
               {
                    /* 1.4 has no repeat flag: the IR driver repeats a held
                     * key every ~230 ms (first after 250 ms) */
                    static int last_key;
                    static double last_t;
                    int repeat = (int)we->key_symbol == last_key && now - last_t < 0.35;
                    last_key = (int)we->key_symbol;
                    last_t = now;
                    printf("key 0x%x code %d%s\n", we->key_symbol, we->key_code,
                           repeat ? " repeat" : "");
                    on_key(a, we->key_symbol, repeat, now);
               }
          }

          if (now >= a->next_vol) {
               poll_volume(a, now);
               a->next_vol = now + 0.4;
          }
          if (now >= a->next_inputs) {
               poll_inputs(a);
               a->next_inputs = now + (a->screen != S_NONE ? 1.0 : 3.0);
          }
          if (a->screen == S_PICTURE && now >= a->next_pic) {
               poll_picture(a);
               a->next_pic = now + 1.0;
          }
          /* overlays that just expired need one more redraw to vanish */
          if (a->toast_until != last_toast_end && now >= a->toast_until) {
               last_toast_end = a->toast_until;
               a->dirty = 1;
          }
          if (a->vol_until != last_vol_end && now >= a->vol_until) {
               last_vol_end = a->vol_until;
               a->dirty = 1;
          }
          if (a->dirty)
               redraw(a, now);
     }

     teardown(a);
     if (a->gam)
          gam_close();
     biz_close();
     alarm(0);
     printf("vibe-ui: exit\n");
     return 0;
}
