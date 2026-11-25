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
 *               [--gam-name=NAME]   (implies --gam; default name "vibe")
 *   --timeout: exit after S seconds (default 600, 0 = never). A watchdog
 *   alarm kills the process 10 s after it.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <directfb.h>

#include "biz.h"
#include "common.h"
#include "gam.h"
#include "gfx.h"

#define FONT_REG  "/opt/font/Cobalt_fonts/Roboto-Regular.ttf"
#define FONT_BOLD "/opt/font/Cobalt_fonts/Roboto-Bold.ttf"

/* remote keys without a DirectFB name (vibe keys, 2026-11-25) */
#define KEY_INPUT  0xf068


static volatile sig_atomic_t stop_flag, shot_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }
/* SIGUSR1: write the next frame (our layer only) to /tmp/vibe-shot.raw */
static void on_usr1(int sig) { (void)sig; shot_flag = 1; }

enum screen { S_NONE, S_HOME, S_QUICK, S_SETTINGS, S_RENAME_INPUT, S_RENAME_NAME, S_KEYBOARD };

static const char *const picture_modes[] = {
     "Dynamic", "Standard", "Cinema Night", "Cinema Day", "Game", "Sport", "Calibrated"
};
#define N_MODES 7

/* ---- settings pages: rows of choices and sliders over model keys */
enum row_kind { R_CHOICE, R_SLIDER };

struct setting_row {
     const char *name;
     enum row_kind kind;
     const char *key;                 /* model key read and written */
     int nopts;
     const int *vals;                 /* choice values */
     const char *const *names;        /* choice labels */
     int (*set)(int v);               /* special setter (else set the key) */
     const char *needs;               /* only shown if this key reads 1 */
};

static int set_picture_mode(int v) { return biz_set_picture_mode(v); }

static const int v_mode[] = { 0, 1, 2, 3, 4, 5, 6 };
static const int v_off_on[] = { 0, 1 };
static const char *const n_off_on[] = { "Off", "On" };
static const int v_speaker[] = { 1, 2, 3 };
static const char *const n_speaker[] = { "TV speakers", "Soundbar (ARC)", "Optical" };
static const int v_sound_mode[] = { 0, 1, 2, 3, 4, 5 };
static const char *const n_sound_mode[] = { "Standard", "Speech", "Late Night", "Music", "Film", "Sport" };
static const int v_igr[] = { 0, 1, 2 };
static const char *const n_igr[] = { "Off", "On", "Auto" };
static const int v_sleep[] = { 0, 15, 30, 60, 90, 120 };
static const char *const n_sleep[] = { "Off", "15 min", "30 min", "60 min", "90 min", "120 min" };

static const struct setting_row rows_picture[] = {
     { "Picture mode", R_CHOICE, BIZ_KEY_PICTURE_MODE, 7, v_mode, picture_modes, set_picture_mode, NULL },
     { "Backlight",  R_SLIDER, BIZ_KEY_BACKLIGHT,  0, NULL, NULL, NULL, NULL },
     { "Brightness", R_SLIDER, BIZ_KEY_BRIGHTNESS, 0, NULL, NULL, NULL, NULL },
     { "Contrast",   R_SLIDER, BIZ_KEY_CONTRAST,   0, NULL, NULL, NULL, NULL },
     { "Colour",     R_SLIDER, BIZ_KEY_COLOUR,     0, NULL, NULL, NULL, NULL },
     { "Sharpness",  R_SLIDER, BIZ_KEY_SHARPNESS,  0, NULL, NULL, NULL, NULL },
};
static const struct setting_row rows_sound[] = {
     { "Output",     R_CHOICE, BIZ_KEY_SPEAKER,    3, v_speaker, n_speaker, NULL, NULL },
     { "eARC",       R_CHOICE, BIZ_KEY_EARC,       2, v_off_on, n_off_on, NULL, NULL },
     { "Sound mode", R_CHOICE, BIZ_KEY_SOUND_MODE, 6, v_sound_mode, n_sound_mode, NULL, NULL },
};
static const struct setting_row rows_game[] = {
     { "Game mode",  R_CHOICE, BIZ_KEY_GAME_MODE,  2, v_off_on, n_off_on, NULL, NULL },
     { "Low latency", R_CHOICE, BIZ_KEY_IGR, 3, v_igr, n_igr, NULL, BIZ_KEY_IGR_SUPPORTED },
};
static const struct setting_row rows_sleep[] = {
     { "Turn off after", R_CHOICE, BIZ_KEY_SLEEP_TIMER, 6, v_sleep, n_sleep, NULL, NULL },
};

#define MAX_ROWS 8
static const struct page {
     const char *title, *sub;
     const struct setting_row *rows;
     int nrows;
} pages[] = {
     { "Picture", "Mode, colour", rows_picture, 6 },
     { "Sound", "Output, mode", rows_sound, 3 },
     { "Game", "Low latency", rows_game, 2 },
     { "Sleep timer", "Turn off later", rows_sleep, 1 },
};
#define N_PAGES 4
#define PAGE_PICTURE 0

/* the "Custom name..." row: opens the on-screen keyboard */
static const char KB_ENTRY[] = "";
static const char *const name_presets[] = {
     NULL /* default: the input's own id */, KB_ENTRY, "Apple TV", "PlayStation",
     "Xbox", "Nintendo Switch", "PC", "Chromecast", "Fire TV", "Roku", "Blu-ray",
     "Cable box", "Game console",
};
#define N_PRESETS 13

/* on-screen keyboard: 4 rows of characters, then Shift, Space, Delete, Done */
#define KB_MAX   15                 /* as long as the longest preset */
#define KB_COLS  10
#define KB_ROWS  5
static const char *const kb_chars[4] = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-.&" };
enum { KB_SHIFT, KB_SPACE, KB_DELETE, KB_DONE, KB_NACT };
static const int kb_act_col[KB_NACT] = { 0, 2, 6, 8 };   /* first column of each */
static const int kb_act_w[KB_NACT]   = { 2, 4, 2, 2 };   /* columns it spans */
static const char *const kb_act_name[KB_NACT] = { "Shift", "Space", "Delete", "Done" };

/* "a soundbar is normally connected", kept across boots (the directory is
 * created by boot.sh, owned by the UI user) */
#define STATE_BAR "/var/local/vibe/state/soundbar"

struct rect { int x0, y0, x1, y1; };      /* empty when x0 >= x1 */

struct app {
     IDirectFB             *dfb;
     IDirectFBDisplayLayer *layer;
     IDirectFBWindow       *win;
     IDirectFBSurface      *s;
     IDirectFBEventBuffer  *events;
     IDirectFBFont         *f_title, *f_head, *f_item, *f_bold, *f_small, *f_tiny;
     int W, H;
     uint32_t *back;              /* W*H ARGB drawing buffer in RAM */
     /* partial redraws (dirty == 2): only the old and new focus (and, with
      * want_aux, the keyboard's text field; with want_hud, the volume HUD
      * and the no-signal notice) are drawn again */
     int  want_aux, want_hud, have_frame;
     int  last_screen;
     struct rect last_focus, last_hud, last_region;

     /* model state */
     struct biz_input in[BIZ_MAX_INPUTS];
     int  nin;
     int  vis[BIZ_MAX_INPUTS], nvis;      /* indexes of inputs we show */
     char cur[32];
     /* "No signal" notice: the current input (not the tuner) has had no
      * signal since nosig_since; shown after 3 s with no menu open */
     int  nosig_lost, nosig_shown;
     double nosig_since;
     char nosig_cur[32];           /* the input nosig_since is about */
     char nosig_text[96];          /* what the notice shows */
     int  vol, mute, vol_known;
     int  vol_shown;               /* what the volume bar shows */
     /* soundbar over ARC: the system forwards the remote's volume and mute
      * keys to it over CEC itself, and the model's volume mirrors what the
      * soundbar reports. Writing the volume there does nothing (the model
      * echoes the value, the soundbar does not move), so we only follow. */
     int  arc;                     /* output is ARC and a device is present */
     double follow_until;          /* poll fast until then, after a key */
     double next_arc;
     /* After a cold boot the TV finds the soundbar only ~30 s in, and then
      * drains a long CEC queue for ~45 s more (docs/findings/06). Before it
      * is found, volume keys would change the TV speakers' volume, so they
      * are dropped while a soundbar is expected. After, systemserver
      * forwards every press into that queue itself; we can only warn. */
     int  bar_expected;            /* remembered: a soundbar is normally there */
     int  bar_gone_polls;          /* consecutive polls with no ARC device */
     double arc_since;             /* when the output last became the soundbar */
     /* Sleep timer. tvmain counts down too, but at the end it only tells
      * the UI (one minute early) and leaves the switch-off to it; its flags
      * stay set afterwards, so they cannot mark the next expiry. We keep our
      * own deadline from when the timer was set (docs/findings/06). */
     double sleep_deadline;        /* 0: no timer */
     int  sleep_warned;            /* the last-minute countdown is showing */
     double next_sleep_tick;
     int  page;                             /* settings page shown */
     int  val[MAX_ROWS], val_known;         /* its row values */
     int  vis_row[MAX_ROWS], nrows_vis;     /* rows this TV supports */

     /* UI state */
     enum screen screen;
     int  row, col;              /* Home focus */
     int  sel;                   /* list selection (quick, picture, rename) */
     int  rename_idx;            /* input being renamed (index into in[]) */
     char kb_text[KB_MAX * 4 + 1];   /* keyboard text (UTF-8 if prefilled) */
     int  kb_row, kb_col, kb_shift;
     int  top;                   /* first visible row of a long list */
     char toast[64];
     double toast_until, vol_until;
     double next_inputs, next_vol, next_pic;
     int  dirty;
     int  gam;
     const char *gam_name;
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
/*
 * Each frame is drawn in two passes over the same layout code:
 *   pass SHAPES: the window surface is locked and shapes are rendered in
 *                software (gfx.c: anti-aliased rounded rects, gradients,
 *                glows) - DirectFB here only fills hard-edged rectangles;
 *   pass TEXT:   after Unlock, text through DirectFB (FreeType glyphs).
 * Layout functions take the canvas: non-NULL = shapes, NULL = text.
 */

/* palette (ARGB, not premultiplied) */
#define K_TEXT      0xfff0f3fa
#define K_TEXT2     0xff98a1b4
#define K_TEXT3     0xff6b7488
#define K_PANEL     0xf50f121a
#define K_PANEL_EDGE 0x26ffffff
#define K_CARD      0xeb1c2130
#define K_CARD_HI   0x24ffffff
#define K_ACCENT_L  0xff4f7cff
#define K_ACCENT_R  0xff8a5cff
#define K_GLOW      0xa06470ff
#define K_GOOD      0xff3ddc97
#define K_OFF       0xff5a6275
#define K_TRACK     0x38ffffff
#define K_KEYCAP    0x30ffffff

typedef struct canvas cv_t;

static void set_font_colour(struct app *a, IDirectFBFont *f, u32 c)
{
     a->s->SetFont(a->s, f);
     a->s->SetColor(a->s, c >> 16 & 0xff, c >> 8 & 0xff, c & 0xff, c >> 24);
}

static int text_width(IDirectFBFont *f, const char *str)
{
     int w = 0;
     f->GetStringWidth(f, str, -1, &w);
     return w;
}

/*
 * Text vertically centred in the box [box_y, box_y + box_h): the baseline
 * sits half the ink height (ascender + descender, descender negative)
 * below the box's middle. align: -1 left at x, 0 centred on x, 1 right at x.
 */
static void text_v(struct app *a, IDirectFBFont *f, int x, int box_y, int box_h,
                   u32 colour, const char *str, int align)
{
     int asc = 0, desc = 0;
     f->GetAscender(f, &asc);
     f->GetDescender(f, &desc);
     int base = box_y + box_h / 2 + (asc + desc) / 2;
     DFBSurfaceTextFlags fl = align < 0 ? DSTF_LEFT : align > 0 ? DSTF_RIGHT : DSTF_CENTER;
     set_font_colour(a, f, colour);
     a->s->DrawString(a->s, str, -1, x, base, fl);
}

static const char *input_label(struct app *a, int i)
{
     return a->in[i].label[0] ? a->in[i].label : a->in[i].id;
}

/* a sheet: big rounded panel with a hairline edge and a title */
static void sheet(struct app *a, cv_t *c, int x, int y, int w, int h, const char *title)
{
     if (c) {
          gfx_shadow(c, x, y, w, h, 36, 28, 0x90000000);
          gfx_rrect(c, x, y, w, h, 36, K_PANEL);
          gfx_rrect_outline(c, x, y, w, h, 36, 1, K_PANEL_EDGE);
     } else {
          text_v(a, a->f_title, x + 56, y + 44, 80, K_TEXT, title, -1);
     }
}

/* ---- partial redraws: what a focus move or typing touches */
static struct rect g_focus;               /* focused element(s) of this frame */
static struct rect g_aux;                 /* the keyboard's text field */
static struct rect g_hud;                 /* volume HUD and no-signal notice */

static int rect_empty(struct rect r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }

static void rect_add(struct rect *r, int x0, int y0, int x1, int y1)
{
     if (x0 >= x1 || y0 >= y1)
          return;
     if (rect_empty(*r)) {
          *r = (struct rect){ x0, y0, x1, y1 };
          return;
     }
     if (x0 < r->x0) r->x0 = x0;
     if (y0 < r->y0) r->y0 = y0;
     if (x1 > r->x1) r->x1 = x1;
     if (y1 > r->y1) r->y1 = y1;
}

#define FOCUS_GLOW 24                     /* focus_fill's shadow spread, and a bit */

/* the focus look: gradient pill with a soft glow */
static void focus_fill(cv_t *c, int x, int y, int w, int h, int r)
{
     rect_add(&g_focus, x - FOCUS_GLOW, y - FOCUS_GLOW, x + w + FOCUS_GLOW, y + h + FOCUS_GLOW);
     gfx_shadow(c, x, y, w, h, r, 22, K_GLOW);
     gfx_rrect_hgrad(c, x, y, w, h, r, K_ACCENT_L, K_ACCENT_R);
}

static void dot(cv_t *c, int cx, int cy, int d, u32 colour)
{
     gfx_rrect(c, cx - d / 2, cy - d / 2, d, d, d / 2, colour);
}

/* "NOW" style pill badge; returns its width */
static int badge(struct app *a, cv_t *c, int x_right, int y, const char *label, u32 bg, u32 fg)
{
     int w = text_width(a->f_tiny, label) + 28, h = 36;
     if (c)
          gfx_rrect(c, x_right - w, y, w, h, h / 2, bg);
     else
          text_v(a, a->f_tiny, x_right - w / 2, y, h, fg, label, 0);
     return w;
}

/* key hints: [key] label pairs with key caps, centred on cx */
static void hints(struct app *a, cv_t *c, int cx, int y, const char *const *pairs, int n)
{
     int total = 0, gap = 36, pad = 14, h = 40;
     for (int i = 0; i < n; i++)
          total += text_width(a->f_tiny, pairs[2 * i]) + 2 * pad + 12 +
                   text_width(a->f_small, pairs[2 * i + 1]) + (i ? gap : 0);
     int x = cx - total / 2;
     for (int i = 0; i < n; i++) {
          int kw = text_width(a->f_tiny, pairs[2 * i]) + 2 * pad;
          if (i) x += gap;
          if (c)
               gfx_rrect(c, x, y, kw, h, 10, K_KEYCAP);
          else
               text_v(a, a->f_tiny, x + kw / 2, y, h, K_TEXT, pairs[2 * i], 0);
          x += kw + 12;
          if (!c)
               text_v(a, a->f_small, x, y, h, K_TEXT2, pairs[2 * i + 1], -1);
          x += text_width(a->f_small, pairs[2 * i + 1]);
     }
}

/* ---- Home */
#define HOME_X   120
#define CARD_W   340
#define CARD_H   200
#define CARD_GAP 36
#define CARD_R   28

#define N_SETTINGS (N_PAGES + 1)          /* the pages, then Rename inputs */
#define SET_CARD_W 300
#define SET_CARD_GAP 28

static void home_card_w(struct app *a, cv_t *c, int x, int y, int cw, int focus, const char *title,
                      const char *sub, int dot_state /* -1 none, 0 off, 1 on */, int now)
{
     if (c) {
          if (focus)
               focus_fill(c, x, y, cw, CARD_H, CARD_R);
          else {
               gfx_rrect(c, x, y, cw, CARD_H, CARD_R, K_CARD);
               gfx_rrect_outline(c, x, y, cw, CARD_H, CARD_R, 1, K_PANEL_EDGE);
          }
          if (dot_state >= 0)
               dot(c, x + 40, y + 52, 14, dot_state ? K_GOOD : (focus ? 0x90ffffff : K_OFF));
     }
     if (now)
          badge(a, c, x + cw - 24, y + 34, "NOW", focus ? 0x40ffffff : 0x333ddc97,
                focus ? K_TEXT : K_GOOD);
     if (!c) {
          if (sub)
               text_v(a, a->f_small, dot_state >= 0 ? x + 60 : x + 32, y + 34, 36,
                      focus ? 0xe6ffffff : K_TEXT2, sub, -1);
          text_v(a, a->f_bold, x + 32, y + CARD_H - 96, 64, K_TEXT, title, -1);
     }
}

static void home_card(struct app *a, cv_t *c, int x, int y, int focus, const char *title,
                      const char *sub, int dot_state, int now)
{
     home_card_w(a, c, x, y, CARD_W, focus, title, sub, dot_state, now);
}

static void draw_home(struct app *a, cv_t *c)
{
     char buf[96];
     int ci = -1;

     if (c)
          gfx_fill_vgradient_full(c, 0xc005070c, 0xf205070c);
     for (int i = 0; i < a->nin; i++)
          if (!strcmp(a->in[i].id, a->cur))
               ci = i;

     if (!c) {
          time_t t = time(NULL);
          struct tm tm;
          localtime_r(&t, &tm);
          text_v(a, a->f_title, HOME_X, 96, 80, K_TEXT, "Home", -1);
          snprintf(buf, sizeof(buf), "Now showing  %s", ci >= 0 ? input_label(a, ci) : "-");
          text_v(a, a->f_item, HOME_X, 176, 48, K_TEXT2, buf, -1);
          strftime(buf, sizeof(buf), "%H:%M", &tm);
          text_v(a, a->f_title, a->W - HOME_X, 96, 80, K_TEXT, buf, 1);
          strftime(buf, sizeof(buf), "%A, %d %B", &tm);
          text_v(a, a->f_small, a->W - HOME_X, 176, 48, K_TEXT2, buf, 1);
          text_v(a, a->f_tiny, HOME_X, 290, 30, K_TEXT3, "INPUTS", -1);
          text_v(a, a->f_tiny, HOME_X, 600, 30, K_TEXT3, "SETTINGS", -1);
     }

     for (int k = 0; k < a->nvis; k++) {
          struct biz_input *in = &a->in[a->vis[k]];
          home_card(a, c, HOME_X + k * (CARD_W + CARD_GAP), 336, a->row == 0 && a->col == k,
                    input_label(a, a->vis[k]), in->signal ? "Signal" : "No signal",
                    in->signal, !strcmp(in->id, a->cur));
     }
     for (int k = 0; k < N_SETTINGS; k++)
          home_card_w(a, c, HOME_X + k * (SET_CARD_W + SET_CARD_GAP), 646, SET_CARD_W,
                      a->row == 1 && a->col == k,
                      k < N_PAGES ? pages[k].title : "Rename inputs",
                      k < N_PAGES ? pages[k].sub : "Device names", -1, 0);

     static const char *const h[] = { "ARROWS", "Navigate", "OK", "Select", "BACK", "Close" };
     hints(a, c, a->W / 2, a->H - 96, h, 3);
}

/* ---- quick inputs: left sheet */
#define Q_X 40
#define Q_W 600

static void draw_quick(struct app *a, cv_t *c)
{
     int row_h = 104, rx = Q_X + 28, rw = Q_W - 56;
     /* sheet sized to its content, centred vertically */
     int h = 150 + a->nvis * (row_h + 12) + 130;
     if (h > a->H - 80) h = a->H - 80;
     int y0 = (a->H - h) / 2, top = y0 + 150;

     sheet(a, c, Q_X, y0, Q_W, h, "Inputs");
     for (int k = 0; k < a->nvis; k++) {
          struct biz_input *in = &a->in[a->vis[k]];
          int y = top + k * (row_h + 12), sel = k == a->sel, cur = !strcmp(in->id, a->cur);
          if (c) {
               if (sel)
                    focus_fill(c, rx, y, rw, row_h, 24);
               else if (cur)
                    gfx_rrect(c, rx, y, rw, row_h, 24, K_CARD_HI);
               dot(c, rx + 36, y + row_h / 2, 14, in->signal ? K_GOOD : (sel ? 0x90ffffff : K_OFF));
          } else {
               text_v(a, a->f_bold, rx + 64, y, row_h, K_TEXT, input_label(a, a->vis[k]), -1);
          }
          if (cur)
               badge(a, c, rx + rw - 28, y + (row_h - 36) / 2, "NOW",
                     sel ? 0x40ffffff : 0x333ddc97, sel ? K_TEXT : K_GOOD);
          else if (!c && !in->signal)
               text_v(a, a->f_small, rx + rw - 28, y, row_h, sel ? 0xe6ffffff : K_TEXT3, "No signal", 1);
     }
     static const char *const hh[] = { "OK", "Switch", "BACK", "Close" };
     hints(a, c, Q_X + Q_W / 2, y0 + h - 76, hh, 2);
}

/* ---- right sheet: Picture, Rename */
#define R_W 760
static int r_x(struct app *a) { return a->W - 40 - R_W; }

static void slider(cv_t *c, int x, int y, int w, int v, int focus)
{
     v = v < 0 ? 0 : v > 100 ? 100 : v;
     int fw = w * v / 100;
     gfx_rrect(c, x, y, w, 8, 4, K_TRACK);
     if (fw > 0)
          gfx_rrect_hgrad(c, x, y, fw < 8 ? 8 : fw, 8, 4, K_ACCENT_L, K_ACCENT_R);
     if (focus) {
          gfx_shadow(c, x + fw - 13, y - 9, 26, 26, 13, 10, 0x80000000);
          gfx_rrect(c, x + fw - 13, y - 9, 26, 26, 13, 0xffffffff);
     }
}

static const char *choice_name(const struct setting_row *r, int v)
{
     static char buf[24];
     for (int i = 0; i < r->nopts; i++)
          if (r->vals[i] == v)
               return r->names[i];
     snprintf(buf, sizeof(buf), "%d", v);     /* a value we have no name for */
     return buf;
}

static void draw_settings(struct app *a, cv_t *c)
{
     const struct page *p = &pages[a->page];
     int x0 = r_x(a), rx = x0 + 28, rw = R_W - 56, row_h = 118;
     /* sheet sized to its rows, centred vertically */
     int h = 150 + a->nrows_vis * (row_h + 8) + 120;
     if (h > a->H - 80) h = a->H - 80;
     int y0 = (a->H - h) / 2, top = y0 + 150;

     sheet(a, c, x0, y0, R_W, h, p->title);
     for (int j = 0; j < a->nrows_vis; j++) {
          const struct setting_row *r = &p->rows[a->vis_row[j]];
          int v = a->val[a->vis_row[j]];
          int y = top + j * (row_h + 8), sel = j == a->sel;
          if (c && sel) {
               /* the whole row is focus: its highlight, the slider knob */
               rect_add(&g_focus, rx, y, rx + rw, y + row_h);
               gfx_rrect(c, rx, y, rw, row_h, 24, K_CARD_HI);
          }
          if (r->kind == R_CHOICE) {
               const char *m = a->val_known ? choice_name(r, v) : "...";
               int pw = 320, px = rx + rw - 28 - pw, ph = 56, py = y + (row_h - ph) / 2;
               if (c) {
                    if (sel) focus_fill(c, px, py, pw, ph, ph / 2);
                    else gfx_rrect(c, px, py, pw, ph, ph / 2, K_CARD);
               } else {
                    text_v(a, a->f_item, rx + 32, y, row_h, sel ? K_TEXT : K_TEXT2, r->name, -1);
                    text_v(a, a->f_bold, px + pw / 2, py, ph, K_TEXT, m, 0);
                    if (sel) {
                         text_v(a, a->f_bold, px + 26, py, ph, 0xccffffff, "<", 0);
                         text_v(a, a->f_bold, px + pw - 26, py, ph, 0xccffffff, ">", 0);
                    }
               }
          } else {
               char val[24];
               snprintf(val, sizeof(val), a->val_known ? "%d" : "...", v);
               if (c)
                    slider(c, rx + 32, y + 82, rw - 64, a->val_known ? v : 0, sel);
               else {
                    text_v(a, a->f_item, rx + 32, y + 12, 56, sel ? K_TEXT : K_TEXT2, r->name, -1);
                    text_v(a, a->f_bold, rx + rw - 32, y + 12, 56, K_TEXT, val, 1);
               }
          }
     }
     static const char *const hh[] = { "LEFT/RIGHT", "Change", "BACK", "Back" };
     hints(a, c, x0 + R_W / 2, y0 + h - 76, hh, 2);
}

#define LIST_ROW_H 92

static void draw_list(struct app *a, cv_t *c, const char *title, int n,
                      const char *(*label)(struct app *, int), const char *(*sub)(struct app *, int))
{
     int x0 = r_x(a), y0 = 40, h = a->H - 80, rx = x0 + 28, rw = R_W - 56, top = y0 + 150;

     /* rows that fit between the title and the hints */
     int rows = (h - 150 - 120) / (LIST_ROW_H + 10);
     sheet(a, c, x0, y0, R_W, h, title);
     if (a->sel < a->top) a->top = a->sel;
     if (a->sel >= a->top + rows) a->top = a->sel - rows + 1;
     if (c && n > rows) {
          /* scroll indicator on the right edge */
          int th = h - 150 - 120, bh = th * rows / n, by = top + th * a->top / n;
          gfx_rrect(c, x0 + R_W - 14, top, 4, th, 2, K_TRACK);
          gfx_rrect(c, x0 + R_W - 14, by, 4, bh, 2, 0xb0ffffff);
     }
     for (int i = a->top; i < n && i < a->top + rows; i++) {
          int y = top + (i - a->top) * (LIST_ROW_H + 10), sel = i == a->sel;
          if (c) {
               if (sel) focus_fill(c, rx, y, rw, LIST_ROW_H, 24);
          } else {
               const char *s2 = sub ? sub(a, i) : NULL;
               text_v(a, a->f_bold, rx + 36, y, LIST_ROW_H, K_TEXT, label(a, i), -1);
               if (s2)
                    text_v(a, a->f_small, rx + rw - 36, y, LIST_ROW_H, sel ? 0xe6ffffff : K_TEXT3, s2, 1);
          }
     }
     static const char *const hh[] = { "OK", "Select", "BACK", "Back" };
     hints(a, c, x0 + R_W / 2, y0 + h - 76, hh, 2);
}

static const char *rename_input_label(struct app *a, int i) { return input_label(a, a->vis[i]); }
static const char *rename_input_sub(struct app *a, int i)   { return a->in[a->vis[i]].id; }

/* the current name is a custom one: neither the id nor a preset */
static int custom_name(struct app *a)
{
     const char *cur = input_label(a, a->rename_idx);
     if (!strcmp(cur, a->in[a->rename_idx].id))
          return 0;
     for (int i = 0; i < N_PRESETS; i++)
          if (name_presets[i] && name_presets[i] != KB_ENTRY && !strcmp(cur, name_presets[i]))
               return 0;
     return 1;
}

static const char *preset_sub(struct app *a, int i)
{
     const char *cur = input_label(a, a->rename_idx);
     if (name_presets[i] == KB_ENTRY)
          return custom_name(a) ? cur : NULL;
     const char *name = name_presets[i] ? name_presets[i] : a->in[a->rename_idx].id;
     return !strcmp(cur, name) ? "Current" : NULL;
}

static const char *preset_label(struct app *a, int i)
{
     static char buf[64];
     if (name_presets[i] == KB_ENTRY)
          return "Custom name\xe2\x80\xa6";
     if (name_presets[i])
          return name_presets[i];
     snprintf(buf, sizeof(buf), "Default (%s)", a->in[a->rename_idx].id);
     return buf;
}

/* ---- on-screen keyboard */
#define KB_GAP 10
#define KB_KH  84

/* the key under (row, col): a character, or for the last row an action */
static int kb_action_at(int col)
{
     for (int i = KB_NACT - 1; i >= 0; i--)
          if (col >= kb_act_col[i])
               return i;
     return 0;
}

static char kb_char(struct app *a, int row, int col)
{
     char ch = kb_chars[row][col];
     return a->kb_shift && ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch;
}

static void draw_keyboard(struct app *a, cv_t *c)
{
     int x0 = r_x(a), y0 = 40, h = a->H - 80, rx = x0 + 28, rw = R_W - 56;
     int kw = (rw - (KB_COLS - 1) * KB_GAP) / KB_COLS;
     char title[96], count[16];

     snprintf(title, sizeof(title), "Name %s", a->in[a->rename_idx].id);
     sheet(a, c, x0, y0, R_W, h, title);

     /* the text field, with a caret after the text */
     int fy = y0 + 150, fh = 100;
     int tw = text_width(a->f_bold, a->kb_text);
     if (c) {
          rect_add(&g_aux, rx - 2, fy - 2, rx + rw + 2, fy + fh + 2);
          gfx_rrect(c, rx, fy, rw, fh, 24, K_CARD);
          gfx_rrect_outline(c, rx, fy, rw, fh, 24, 2, K_ACCENT_L);
          gfx_rrect(c, rx + 36 + tw + 4, fy + 26, 4, fh - 52, 2, K_ACCENT_R);
     } else {
          if (a->kb_text[0])
               text_v(a, a->f_bold, rx + 36, fy, fh, K_TEXT, a->kb_text, -1);
          else
               text_v(a, a->f_item, rx + 48, fy, fh, K_TEXT3, a->in[a->rename_idx].id, -1);
          snprintf(count, sizeof(count), "%d/%d", (int)strlen(a->kb_text), KB_MAX);
          text_v(a, a->f_small, rx + rw - 32, fy, fh, K_TEXT3, count, 1);
     }

     /* the keys */
     int gy = fy + fh + 48;
     for (int row = 0; row < KB_ROWS; row++) {
          int y = gy + row * (KB_KH + KB_GAP);
          if (row < 4) {
               for (int col = 0; col < KB_COLS; col++) {
                    int x = rx + col * (kw + KB_GAP);
                    int f = row == a->kb_row && col == a->kb_col;
                    if (c) {
                         if (f) focus_fill(c, x, y, kw, KB_KH, 16);
                         else gfx_rrect(c, x, y, kw, KB_KH, 16, K_CARD);
                    } else {
                         char s[2] = { kb_char(a, row, col), 0 };
                         text_v(a, a->f_bold, x + kw / 2, y, KB_KH, K_TEXT, s, 0);
                    }
               }
          } else {
               int cur = kb_action_at(a->kb_col);
               for (int i = 0; i < KB_NACT; i++) {
                    int x = rx + kb_act_col[i] * (kw + KB_GAP);
                    int w = kb_act_w[i] * kw + (kb_act_w[i] - 1) * KB_GAP;
                    int f = row == a->kb_row && i == cur;
                    int on = i == KB_SHIFT && a->kb_shift;
                    if (c) {
                         if (f) focus_fill(c, x, y, w, KB_KH, 16);
                         else gfx_rrect(c, x, y, w, KB_KH, 16, on ? 0xff2c3a66 : K_CARD);
                    } else {
                         text_v(a, a->f_small, x + w / 2, y, KB_KH,
                                on && !f ? K_ACCENT_L : K_TEXT, kb_act_name[i], 0);
                    }
               }
          }
     }
     static const char *const hh[] = { "OK", "Type", "0-9", "Digits", "BACK", "Cancel" };
     hints(a, c, x0 + R_W / 2, y0 + h - 76, hh, 3);
}

/* ---- HUDs */
static void draw_toast(struct app *a, cv_t *c)
{
     int w = text_width(a->f_bold, a->toast) + 120, h = 92, x = (a->W - w) / 2, y = 56;
     if (c) {
          gfx_shadow(c, x, y, w, h, h / 2, 24, 0x90000000);
          gfx_rrect(c, x, y, w, h, h / 2, K_PANEL);
          gfx_rrect_outline(c, x, y, w, h, h / 2, 1, K_PANEL_EDGE);
          dot(c, x + 44, y + h / 2, 14, K_ACCENT_R);
     } else {
          text_v(a, a->f_bold, x + 72, y, h, K_TEXT, a->toast, -1);
     }
}

static void draw_volume(struct app *a, cv_t *c)
{
     int w = 680, h = 104, x = (a->W - w) / 2, y = a->H - 200;
     int v = a->vol_shown < 0 ? 0 : a->vol_shown > 100 ? 100 : a->vol_shown;
     int bx = x + 190, bw = w - 190 - 56;
     if (c) {
          rect_add(&g_hud, x - 24, y - 24, x + w + 24, y + h + 24);   /* + shadow */
          gfx_shadow(c, x, y, w, h, h / 2, 24, 0x90000000);
          gfx_rrect(c, x, y, w, h, h / 2, K_PANEL);
          gfx_rrect_outline(c, x, y, w, h, h / 2, 1, K_PANEL_EDGE);
          gfx_rrect(c, bx, y + h / 2 - 5, bw, 10, 5, K_TRACK);
          if (!a->mute && v > 0)
               gfx_rrect_hgrad(c, bx, y + h / 2 - 5, bw * v / 100 < 10 ? 10 : bw * v / 100, 10, 5,
                               K_ACCENT_L, K_ACCENT_R);
     } else {
          char label[16];
          snprintf(label, sizeof(label), a->mute ? "Muted" : "%d", v);
          text_v(a, a->f_tiny, x + 56, y + 14, 34, K_TEXT3, "VOLUME", -1);
          text_v(a, a->mute ? a->f_item : a->f_head, x + 56, y + 40, 54, K_TEXT, label, -1);
     }
}

/* "<input> . No signal", small, bottom left */
static void draw_nosig(struct app *a, cv_t *c)
{
     int w = text_width(a->f_item, a->nosig_text) + 100, h = 72, x = 72, y = a->H - 152;
     if (c) {
          rect_add(&g_hud, x - 24, y - 24, x + w + 24, y + h + 24);   /* + shadow */
          gfx_shadow(c, x, y, w, h, h / 2, 24, 0x90000000);
          gfx_rrect(c, x, y, w, h, h / 2, K_PANEL);
          gfx_rrect_outline(c, x, y, w, h, h / 2, 1, K_PANEL_EDGE);
          dot(c, x + 38, y + h / 2, 12, K_OFF);
     } else {
          text_v(a, a->f_item, x + 62, y, h, K_TEXT2, a->nosig_text, -1);
     }
}

static void draw_screens(struct app *a, cv_t *c, double now)
{
     switch (a->screen) {
     case S_HOME:         draw_home(a, c); break;
     case S_QUICK:        draw_quick(a, c); break;
     case S_SETTINGS:     draw_settings(a, c); break;
     case S_RENAME_INPUT: draw_list(a, c, "Rename inputs", a->nvis, rename_input_label, rename_input_sub); break;
     case S_RENAME_NAME: {
          char t[96];
          snprintf(t, sizeof(t), "Rename %s", input_label(a, a->rename_idx));
          draw_list(a, c, t, N_PRESETS, preset_label, preset_sub);
          break;
     }
     case S_KEYBOARD:     draw_keyboard(a, c); break;
     case S_NONE:         break;
     }
     if (a->nosig_shown && a->screen == S_NONE)
          draw_nosig(a, c);
     if (now < a->toast_until)
          draw_toast(a, c);
     if (now < a->vol_until)
          draw_volume(a, c);
}

static void redraw(struct app *a, double now)
{
     void *ptr = NULL;
     int pitch = 0;
     double t0 = vibe_now_s();

     /* Shapes are drawn in ordinary RAM, then copied to the surface in one
      * sequential write pass. Blending reads every pixel, and the locked
      * surface is display memory, slow to read: drawing the keyboard there
      * took ~260 ms, in RAM ~60 ms (native/tools/gfx_bench.c). */
     if (!a->back)
          a->back = malloc((size_t)a->W * a->H * 4);

     /*
      * Partial frame (dirty == 2: a focus move, typing, or a HUD change):
      * the RAM buffer holds the whole previous frame, so only the old focus
      * and the new one (and the text field when typing, and the HUDs' old
      * and new places) are drawn again. A measuring pass
      * with an empty clip finds the new focus without drawing; a list that
      * scrolled (top moved) needs the full frame. The window is double
      * buffered, so the surface gets this frame's region and the last one's.
      */
     struct rect full = { 0, 0, a->W, a->H }, region = full;
     int partial = a->dirty == 2 && a->have_frame && a->back &&
                   (int)a->screen == a->last_screen;
     if (partial) {
          struct canvas m = { a->back, a->W, a->W, a->H, 1, 0, 0, 0, 0 };
          int top = a->top;
          g_focus = g_aux = g_hud = (struct rect){ 0, 0, 0, 0 };
          draw_screens(a, &m, now);
          if (a->top != top) {
               partial = 0;
          } else {
               region = a->last_focus;
               rect_add(&region, g_focus.x0, g_focus.y0, g_focus.x1, g_focus.y1);
               if (a->want_aux)
                    rect_add(&region, g_aux.x0, g_aux.y0, g_aux.x1, g_aux.y1);
               /* the HUDs: old and new place, so one that appears, changes
                * or vanishes (new rect empty) is covered */
               if (a->want_hud || memcmp(&g_hud, &a->last_hud, sizeof(g_hud))) {
                    rect_add(&region, a->last_hud.x0, a->last_hud.y0, a->last_hud.x1, a->last_hud.y1);
                    rect_add(&region, g_hud.x0, g_hud.y0, g_hud.x1, g_hud.y1);
               }
               if (region.x0 < 0) region.x0 = 0;
               if (region.y0 < 0) region.y0 = 0;
               if (region.x1 > a->W) region.x1 = a->W;
               if (region.y1 > a->H) region.y1 = a->H;
               if (rect_empty(region))
                    partial = 0;
          }
     }
     if (!partial)
          region = full;
     /* what the surface needs: this frame's region and the previous one */
     struct rect copy = region;
     if (partial)
          rect_add(&copy, a->last_region.x0, a->last_region.y0, a->last_region.x1, a->last_region.y1);

     g_focus = g_aux = g_hud = (struct rect){ 0, 0, 0, 0 };
     if (a->back && a->s->Lock(a->s, DSLF_WRITE, &ptr, &pitch) == DFB_OK && ptr) {
          struct canvas c = { a->back, a->W, a->W, a->H, partial,
                              region.x0, region.y0, region.x1, region.y1 };
          gfx_clear(&c);                        /* transparent: the picture shows */
          draw_screens(a, &c, now);
          size_t bytes = (size_t)(copy.x1 - copy.x0) * 4;
          for (int y = copy.y0; y < copy.y1; y++)
               memcpy((char *)ptr + (size_t)y * pitch + (size_t)copy.x0 * 4,
                      a->back + (size_t)y * a->W + copy.x0, bytes);
          a->s->Unlock(a->s);
     } else {
          a->s->Clear(a->s, 0, 0, 0, 0);
          partial = 0;
          copy = region = full;
     }
     double t1 = vibe_now_s();
     /* text only where pixels were just replaced: elsewhere it is already
      * there, and drawing it again would blend it twice */
     DFBRegion clip = { copy.x0, copy.y0, copy.x1 - 1, copy.y1 - 1 };
     a->s->SetClip(a->s, partial ? &clip : NULL);
     a->s->SetDrawingFlags(a->s, DSDRAW_BLEND);
     draw_screens(a, NULL, now);
     a->s->SetClip(a->s, NULL);
     double t2 = vibe_now_s();
     a->last_focus = g_focus;
     a->last_hud = g_hud;
     a->last_region = region;
     a->last_screen = a->screen;
     a->have_frame = a->back != NULL;
     a->want_aux = a->want_hud = 0;
     if (shot_flag) {
          /* raw dump: "W H\n" then W*H ARGB words, little endian */
          FILE *f = fopen("/tmp/vibe-shot.raw", "wb");
          if (f && a->s->Lock(a->s, DSLF_READ | DSLF_WRITE, &ptr, &pitch) == DFB_OK) {
               fprintf(f, "%d %d\n", a->W, a->H);
               for (int y = 0; y < a->H; y++)
                    fwrite((char *)ptr + (size_t)y * pitch, 4, a->W, f);
               a->s->Unlock(a->s);
               printf("screenshot written\n");
          }
          if (f) fclose(f);
          shot_flag = 0;
     }
     a->s->Flip(a->s, NULL, DSFLIP_WAITFORSYNC);
     a->dirty = 0;
     if (getenv("VIBE_TIMING"))
          printf("frame %.0f ms (shapes %.0f, text %.0f, flip %.0f) %s %dx%d\n",
                 (vibe_now_s() - t0) * 1000, (t1 - t0) * 1000, (t2 - t1) * 1000,
                 (vibe_now_s() - t2) * 1000, partial ? "partial" : "full",
                 copy.x1 - copy.x0, copy.y1 - copy.y0);
}

/* --------------------------------------------------------------- model */

/* only a HUD changed (volume, no-signal notice): a partial redraw of its old
 * and new place (a full one stays full) */
static void mark_hud(struct app *a)
{
     a->want_hud = 1;
     if (!a->dirty)
          a->dirty = 2;
}

static void poll_inputs(struct app *a)
{
     struct biz_input in[BIZ_MAX_INPUTS];
     char cur[32] = "";
     /* zeroed: the memcmp below compares whole structs, and stack garbage
      * after the strings made every poll look like a change (a full
      * redraw every second while a menu was open) */
     memset(in, 0, sizeof(in));
     int n = biz_inputs(in, BIZ_MAX_INPUTS);

     biz_current_input(cur, sizeof(cur));
     if (n > 0 && (n != a->nin || memcmp(in, a->in, n * sizeof(in[0])))) {
          memcpy(a->in, in, n * sizeof(in[0]));
          a->nin = n;
          a->nvis = 0;
          for (int i = 0; i < n; i++)
               if (strcmp(in[i].id, "TV"))      /* the tuner is not used */
                    a->vis[a->nvis++] = i;
          /* never clear a redraw already asked for (a volume key earlier
           * in the same loop) */
          if (a->screen != S_NONE)
               a->dirty = 1;
     }
     if (cur[0] && strcmp(cur, a->cur)) {
          printf("current input: %s\n", cur);
          snprintf(a->cur, sizeof(a->cur), "%s", cur);
          if (a->screen != S_NONE)
               a->dirty = 1;
     }
}

/*
 * The "No signal" notice, checked every loop (no model reads: poll_inputs
 * keeps in[] fresh). An input switch takes a moment to lock, so the
 * signal must be missing for 3 s on the same input before it shows.
 */
static void check_signal(struct app *a, double now)
{
     int i;
     for (i = 0; i < a->nin; i++)
          if (!strcmp(a->in[i].id, a->cur))
               break;
     int lost = i < a->nin && strcmp(a->in[i].id, "TV") && a->in[i].signal != 1;
     if (lost && (!a->nosig_lost || strcmp(a->nosig_cur, a->cur))) {
          a->nosig_since = now;
          snprintf(a->nosig_cur, sizeof(a->nosig_cur), "%s", a->cur);
          printf("no signal on %s\n", a->cur);
     } else if (!lost && a->nosig_lost) {
          printf("signal on %s after %.1f s\n", a->nosig_cur, now - a->nosig_since);
     }
     a->nosig_lost = lost;

     int show = lost && a->screen == S_NONE && now - a->nosig_since >= 3.0;
     char text[96] = "";
     if (show)
          snprintf(text, sizeof(text), "%s \xc2\xb7 No signal", input_label(a, i));
     if (show != a->nosig_shown || (show && strcmp(text, a->nosig_text))) {
          printf("no-signal notice: %s\n", show ? text : "hidden");
          a->nosig_shown = show;
          snprintf(a->nosig_text, sizeof(a->nosig_text), "%s", text);
          mark_hud(a);
     }
}


static int read_bar_expected(void)
{
     int v = 0;
     FILE *f = fopen(STATE_BAR, "r");
     if (f) {
          if (fscanf(f, "%d", &v) != 1)
               v = 0;
          fclose(f);
     }
     return v == 1;
}

static void set_bar_expected(struct app *a, int v, const char *why)
{
     if (a->bar_expected == v)
          return;
     a->bar_expected = v;
     printf("soundbar expected: %d (%s)\n", v, why);
     FILE *f = fopen(STATE_BAR ".new", "w");
     if (!f) {
          perror("vibe-ui: " STATE_BAR ".new");
          return;
     }
     fprintf(f, "%d\n", v);
     if (fclose(f) == 0)
          rename(STATE_BAR ".new", STATE_BAR);
}

/* every 2 s */
static void poll_arc(struct app *a, double now)
{
     int spk = 0, dev = 0;
     biz_get_int(BIZ_KEY_SPEAKER, &spk);
     biz_get_int(BIZ_KEY_ARC_DEVICE, &dev);
     int arc = spk == 2 && dev == 1;
     if (arc != a->arc) {
          printf("output: %s at %.1f\n", arc ? "soundbar (ARC)" : "TV", now);
          a->arc = arc;
          if (arc)
               a->arc_since = now;
     }
     if (arc) {
          set_bar_expected(a, 1, "soundbar connected");
          a->bar_gone_polls = 0;
     } else if (dev == 1 && spk != 2) {
          /* a soundbar is there and the owner chose another output */
          set_bar_expected(a, 0, "other output chosen");
     } else if (dev == 0 && now > 120) {
          /* after boot, a minute with no ARC device: unplugged */
          if (++a->bar_gone_polls >= 30)
               set_bar_expected(a, 0, "no ARC device for a minute");
     } else {
          a->bar_gone_polls = 0;
     }
}

static void poll_volume(struct app *a, double now)
{
     int vol = a->vol, mute = a->mute;

     biz_get_int(BIZ_KEY_VOLUME, &vol);
     biz_get_int(BIZ_KEY_MUTE, &mute);
     if (a->vol_known && (vol != a->vol || mute != a->mute)) {
          printf("volume %d mute %d t=%.2f\n", vol, mute, now);
          a->vol_until = now + 2.5;
          mark_hud(a);
     }
     a->vol = vol;
     a->mute = mute;
     a->vol_known = 1;
     a->vol_shown = vol;
}

/* read every row of the open page, and which rows this TV supports */
static void poll_page(struct app *a)
{
     const struct page *p = &pages[a->page];
     int v[MAX_ROWS], vis[MAX_ROWS], n = 0;

     for (int i = 0; i < p->nrows; i++) {
          const struct setting_row *r = &p->rows[i];
          int ok = 1;
          v[i] = 0;
          biz_get_int(r->key, &v[i]);
          if (r->needs) {
               int s = 0;
               biz_get_int(r->needs, &s);
               ok = s == 1;
          }
          if (ok)
               vis[n++] = i;
     }
     if (!a->val_known || n != a->nrows_vis || memcmp(v, a->val, p->nrows * sizeof(int)) ||
         memcmp(vis, a->vis_row, n * sizeof(int))) {
          memcpy(a->val, v, p->nrows * sizeof(int));
          memcpy(a->vis_row, vis, n * sizeof(int));
          a->nrows_vis = n;
          if (a->sel >= n)
               a->sel = n ? n - 1 : 0;
          a->val_known = 1;
          a->dirty = a->screen == S_SETTINGS;
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
     } else if (s == S_SETTINGS) {
          /* never draw a page with the previous page's row list: read the
           * new page now, before the first frame */
          a->sel = 0;
          a->val_known = 0;
          a->nrows_vis = 0;
          poll_page(a);
          a->next_pic = now + 1.0;
     } else if (s == S_RENAME_INPUT) {
          a->sel = 0;
     } else if (s == S_RENAME_NAME) {
          a->sel = 0;
     } else if (s == S_KEYBOARD) {
          /* start from the current name if it is a custom one */
          a->kb_text[0] = 0;
          if (custom_name(a))
               snprintf(a->kb_text, sizeof(a->kb_text), "%s", input_label(a, a->rename_idx));
          a->kb_shift = !a->kb_text[0];
          a->kb_row = 1;
          a->kb_col = 0;
     }
}

static void toast(struct app *a, const char *msg, double now)
{
     snprintf(a->toast, sizeof(a->toast), "%s", msg);
     a->toast_until = now + 3;
     a->dirty = 1;
}

/* ---- sleep timer */
static void sleep_arm(struct app *a, int minutes, double now)
{
     a->sleep_warned = 0;
     if (minutes > 0) {
          a->sleep_deadline = now + minutes * 60.0;
          printf("sleep timer: off at %.0f (in %d min)\n", a->sleep_deadline, minutes);
     } else {
          if (a->sleep_deadline)
               printf("sleep timer: disarmed\n");
          a->sleep_deadline = 0;
     }
}

/* cancelled by a key in the last minute, or by standby: Off in the model too */
static void sleep_cancel(struct app *a, const char *why, double now, int say)
{
     printf("sleep timer: cancelled (%s)\n", why);
     sleep_arm(a, 0, now);
     biz_set_int(BIZ_KEY_SLEEP_TIMER, 0);
     a->val_known = 0;                  /* the Sleep page shows the new value */
     if (say)
          toast(a, "Sleep timer off", now);
     else
          a->toast_until = 0;
     a->dirty = 1;
}

static void sleep_tick(struct app *a, double now)
{
     if (!a->sleep_deadline)
          return;
     double left = a->sleep_deadline - now;
     if (left <= 0) {
          printf("sleep timer: switching off\n");
          a->sleep_deadline = 0;
          a->sleep_warned = 0;
          a->screen = S_NONE;
          a->toast_until = 0;
          a->dirty = 1;
          int r = biz_switch_off_tv(BIZ_POWER_OFF_SLEEP_TIMER);
          printf("switch.off.tv [%d] -> r=%d\n", BIZ_POWER_OFF_SLEEP_TIMER, r);
          biz_set_int(BIZ_KEY_SLEEP_TIMER, 0);
          return;
     }
     if (left > 60)
          return;
     if (!a->sleep_warned) {
          a->sleep_warned = 1;
          a->next_sleep_tick = 0;
          printf("sleep timer: last minute\n");
     }
     if (now >= a->next_sleep_tick) {
          snprintf(a->toast, sizeof(a->toast), "Turning off in %d s \xc2\xb7 any key cancels",
                   (int)(left + 0.999));
          a->toast_until = now + 1.5;
          a->next_sleep_tick = now + 1.0;
          a->dirty = 1;
     }
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

/* only the focus moved: a partial redraw (a full one stays full) */
static void mark_focus(struct app *a)
{
     if (!a->dirty)
          a->dirty = 2;
}

static void list_move(struct app *a, int n, int d)
{
     int s = a->sel + d;
     if (s >= 0 && s < n) {
          a->sel = s;
          mark_focus(a);                /* redraw() goes full if the list scrolls */
     }
}

static void key_home(struct app *a, int k, double now)
{
     int ncol = a->row == 0 ? a->nvis : N_SETTINGS;
     switch (k) {
     case DIKS_CURSOR_LEFT:  if (a->col > 0) { a->col--; mark_focus(a); } break;
     case DIKS_CURSOR_RIGHT: if (a->col + 1 < ncol) { a->col++; mark_focus(a); } break;
     case DIKS_CURSOR_UP:
          if (a->row > 0) { a->row = 0; if (a->col >= a->nvis) a->col = a->nvis - 1; mark_focus(a); }
          break;
     case DIKS_CURSOR_DOWN:
          if (a->row < 1) { a->row = 1; if (a->col >= N_SETTINGS) a->col = N_SETTINGS - 1; mark_focus(a); }
          break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (a->row == 0 && a->col < a->nvis) {
               switch_to(a, a->vis[a->col], now);
               a->screen = S_NONE;
               a->dirty = 1;
          } else if (a->row == 1) {
               if (a->col < N_PAGES) {
                    a->page = a->col;
                    a->val_known = 0;
                    open_screen(a, S_SETTINGS, now);
               } else {
                    open_screen(a, S_RENAME_INPUT, now);
               }
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
          if (a->nvis) { a->sel = (a->sel + 1) % a->nvis; mark_focus(a); }
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

static void key_settings(struct app *a, int k, int repeat, double now)
{
     const struct page *p = &pages[a->page];
     int d = 0;
     switch (k) {
     case DIKS_CURSOR_UP:    list_move(a, a->nrows_vis, -1); return;
     case DIKS_CURSOR_DOWN:  list_move(a, a->nrows_vis, +1); return;
     case DIKS_CURSOR_LEFT:  d = -1; break;
     case DIKS_CURSOR_RIGHT: d = +1; break;
     case DIKS_BACK:
          open_screen(a, S_HOME, now); a->row = 1; a->col = a->page;
          return;
     case DIKS_EXIT:
          a->screen = S_NONE; a->dirty = 1;
          return;
     default:
          return;
     }
     if (!a->val_known || a->sel >= a->nrows_vis)
          return;
     int ri = a->vis_row[a->sel];
     const struct setting_row *r = &p->rows[ri];
     int v, rc;
     if (r->kind == R_CHOICE) {
          if (repeat)
               return;                  /* one step per press */
          int i = 0;
          for (int j = 0; j < r->nopts; j++)
               if (r->vals[j] == a->val[ri])
                    i = j;
          i = (i + d + r->nopts) % r->nopts;
          v = r->vals[i];
     } else {
          v = a->val[ri] + d * (repeat ? 2 : 1);
          v = v < 0 ? 0 : v > 100 ? 100 : v;
     }
     rc = r->set ? r->set(v) : biz_set_int(r->key, v);
     if (!strcmp(r->key, BIZ_KEY_SLEEP_TIMER))
          sleep_arm(a, v, now);
     printf("%s: %s = %d -> r=%d\n", p->title, r->name, v, rc);
     a->val[ri] = v;
     /* a change can move other rows (picture mode moves the sliders); the
      * poll that follows redraws in full if they did */
     a->next_pic = now + (r->kind == R_CHOICE ? 0.6 : 1.0);
     mark_focus(a);                     /* the value sits in the focused row */
}

static void do_rename(struct app *a, const char *name, double now)
{
     const char *id = a->in[a->rename_idx].id;
     int r = biz_rename_input(id, name);
     char msg[64];
     printf("rename %s -> '%s' r=%d\n", id, name, r);
     snprintf(msg, sizeof(msg), "%s: %s", id, name);
     toast(a, msg, now);
     a->next_inputs = now + 0.5;
     open_screen(a, S_RENAME_INPUT, now);
}

static void kb_type(struct app *a, char ch)
{
     size_t n = strlen(a->kb_text);
     if (n >= KB_MAX)
          return;
     a->kb_text[n] = ch;
     a->kb_text[n + 1] = 0;
     if (ch >= 'A' && ch <= 'Z')
          a->kb_shift = 0;              /* shift is for one letter */
}

static void key_keyboard(struct app *a, int k, double now)
{
     int last = KB_ROWS - 1;
     int shift0 = a->kb_shift;
     size_t len0 = strlen(a->kb_text);
     switch (k) {
     case DIKS_CURSOR_UP:
          if (a->kb_row > 0) {
               /* from the action row, land on the action's first column */
               if (a->kb_row == last)
                    a->kb_col = kb_act_col[kb_action_at(a->kb_col)];
               a->kb_row--;
          }
          break;
     case DIKS_CURSOR_DOWN:
          if (a->kb_row < last) a->kb_row++;
          break;
     case DIKS_CURSOR_LEFT:
          if (a->kb_row == last) {
               int i = kb_action_at(a->kb_col);
               if (i > 0) a->kb_col = kb_act_col[i - 1];
          } else if (a->kb_col > 0) {
               a->kb_col--;
          }
          break;
     case DIKS_CURSOR_RIGHT:
          if (a->kb_row == last) {
               int i = kb_action_at(a->kb_col);
               if (i < KB_NACT - 1) a->kb_col = kb_act_col[i + 1];
          } else if (a->kb_col < KB_COLS - 1) {
               a->kb_col++;
          }
          break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (a->kb_row < last) {
               kb_type(a, kb_char(a, a->kb_row, a->kb_col));
               break;
          }
          switch (kb_action_at(a->kb_col)) {
          case KB_SHIFT: a->kb_shift = !a->kb_shift; break;
          case KB_SPACE:
               if (a->kb_text[0]) kb_type(a, ' ');
               break;
          case KB_DELETE: {
               size_t n = strlen(a->kb_text);
               /* drop one UTF-8 character (a prefilled name may have any) */
               while (n > 0 && ((unsigned char)a->kb_text[--n] & 0xc0) == 0x80)
                    ;
               a->kb_text[n] = 0;
               if (!n) a->kb_shift = 1;
               break;
          }
          case KB_DONE: {
               size_t n = strlen(a->kb_text);
               while (n > 0 && a->kb_text[n - 1] == ' ')
                    a->kb_text[--n] = 0;
               /* nothing typed: back to the input's own name */
               do_rename(a, n ? a->kb_text : a->in[a->rename_idx].id, now);
               return;
          }
          }
          break;
     case DIKS_0: case DIKS_1: case DIKS_2: case DIKS_3: case DIKS_4:
     case DIKS_5: case DIKS_6: case DIKS_7: case DIKS_8: case DIKS_9:
          kb_type(a, '0' + (k - DIKS_0));
          break;
     case DIKS_BACK:
          open_screen(a, S_RENAME_NAME, now);
          a->sel = 1;                   /* back on "Custom name..." */
          return;
     case DIKS_EXIT:
          a->screen = S_NONE;
          break;
     default:
          return;
     }
     if (a->screen != S_KEYBOARD || a->kb_shift != shift0) {
          a->dirty = 1;                 /* Shift relabels every key */
     } else {
          mark_focus(a);
          if (strlen(a->kb_text) != len0)
               a->want_aux = 1;         /* and the text field */
     }
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
          } else if (name_presets[a->sel] == KB_ENTRY) {
               open_screen(a, S_KEYBOARD, now);
          } else {
               const char *id = a->in[a->rename_idx].id;
               do_rename(a, name_presets[a->sel] ? name_presets[a->sel] : id, now);
          }
          break;
     case DIKS_BACK:
          if (a->screen == S_RENAME_NAME) open_screen(a, S_RENAME_INPUT, now);
          else { open_screen(a, S_HOME, now); a->row = 1; a->col = N_PAGES; }
          break;
     case DIKS_EXIT:
          a->screen = S_NONE; a->dirty = 1;
          break;
     }
}

static void on_key(struct app *a, int k, int repeat, double now)
{
     /* in the last minute of the sleep timer any key (POWER aside: the
      * system acts on it) cancels it, and does nothing else */
     if (a->sleep_warned && k != DIKS_POWER && k != DIKS_POWER2) {
          sleep_cancel(a, "key", now, 1);
          return;
     }
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
          if (!a->arc && a->bar_expected) {
               /* the soundbar is not found yet (after a cold boot ~30 s,
                * after standby a moment): the key would only change the TV
                * speakers' volume, and is not forwarded to the soundbar */
               printf("volume key dropped: soundbar not connected yet\n");
               toast(a, "Sound system connecting\xe2\x80\xa6", now);
               return;
          }
          if (a->arc) {
               /* the system sends the key to the soundbar; show what it
                * reports, polling fast for a moment */
               a->follow_until = now + 1.5;
               a->next_vol = now + 0.1;
               r = 0;
               /* found early in a boot: the TV's CEC queue is still long,
                * presses reach the soundbar late (and cannot be held back:
                * systemserver queues them before we see them) */
               if (a->arc_since < 150 && now - a->arc_since < 45)
                    toast(a, "Sound system starting, volume may lag", now);
          } else if (k == DIKS_MUTE) {
               a->mute = !a->mute;
               r = biz_set_int(BIZ_KEY_MUTE, a->mute);
          } else {
               int d = k == DIKS_VOLUME_UP ? 1 : -1;
               if (a->mute) {               /* volume keys unmute */
                    a->mute = 0;
                    biz_set_int(BIZ_KEY_MUTE, 0);
               }
               int v = a->vol + d;
               a->vol = v < 0 ? 0 : v > 100 ? 100 : v;
               a->vol_shown = a->vol;
               r = biz_set_int(BIZ_KEY_VOLUME, a->vol);
          }
          printf("volume key: shown %d mute %d (r=%d)\n", a->vol_shown, a->mute, r);
          a->vol_until = now + 2.5;
          if (!a->arc)
               a->next_vol = now + 0.5;
          mark_hud(a);
          return;
     }
     case DIKS_HOME:
          if (a->screen == S_NONE) {
               /* focus starts on the input being shown */
               open_screen(a, S_HOME, now);
               a->row = 0;
               a->col = 0;
               for (int k = 0; k < a->nvis; k++)
                    if (!strcmp(a->in[a->vis[k]].id, a->cur))
                         a->col = k;
          }
          else { a->screen = S_NONE; a->dirty = 1; }
          return;
     case DIKS_MENU:
          if (a->screen == S_SETTINGS) { a->screen = S_NONE; a->dirty = 1; }
          else { a->page = PAGE_PICTURE; a->val_known = 0; open_screen(a, S_SETTINGS, now); }
          return;
     case KEY_INPUT:
          if (a->screen != S_QUICK) { open_screen(a, S_QUICK, now); return; }
          break;
     }
     if (repeat && a->screen != S_SETTINGS &&
         k != DIKS_CURSOR_UP && k != DIKS_CURSOR_DOWN &&
         k != DIKS_CURSOR_LEFT && k != DIKS_CURSOR_RIGHT)
          return;
     switch (a->screen) {
     case S_HOME:         key_home(a, k, now); break;
     case S_QUICK:        key_quick(a, k, now); break;
     case S_SETTINGS:     key_settings(a, k, repeat, now); break;
     case S_RENAME_INPUT:
     case S_RENAME_NAME:  key_rename(a, k, now); break;
     case S_KEYBOARD:     key_keyboard(a, k, now); break;
     case S_NONE:         break;
     }
}

/* ------------------------------------------------------------ dev hook */
/*
 * Development, only while the root-owned flag /var/local/vibe/dev exists
 * (checked at start): lines in /tmp/vibe-cmd (created by root, mode 666) are
 * executed and the file emptied:  "key 0xf005"  acts as that remote key,
 * "shot" writes the next frame to /tmp/vibe-shot.raw.
 */
static void dev_commands(struct app *a, double now)
{
     static double next;
     static int enabled = -1;
     char line[128];
     FILE *f;

     if (enabled < 0) {
          enabled = access("/var/local/vibe/dev", F_OK) == 0;
          printf("dev hook %s\n", enabled ? "on" : "off");
     }
     if (!enabled || now < next)
          return;
     next = now + 0.3;
     f = fopen("/tmp/vibe-cmd", "r+");
     if (!f)
          return;
     while (fgets(line, sizeof(line), f)) {
          unsigned k;
          if (sscanf(line, "key %x", &k) == 1) {
               printf("dev: key 0x%x\n", k);
               on_key(a, (int)k, 0, now);
          } else if (!strncmp(line, "shot", 4)) {
               shot_flag = 1;
          }
     }
     if (ftruncate(fileno(f), 0) != 0)
          printf("dev: cannot empty /tmp/vibe-cmd\n");
     fclose(f);
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

     if (font(a, FONT_BOLD, 60, &a->f_title) || font(a, FONT_BOLD, 48, &a->f_head) ||
         font(a, FONT_REG, 34, &a->f_item) || font(a, FONT_BOLD, 38, &a->f_bold) ||
         font(a, FONT_REG, 28, &a->f_small) || font(a, FONT_BOLD, 21, &a->f_tiny))
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
     if (a->f_tiny) a->f_tiny->Release(a->f_tiny);
     if (a->f_head) a->f_head->Release(a->f_head);
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
          else if (!strncmp(argv[i], "--gam-name=", 11)) { a->gam = 1; a->gam_name = argv[i] + 11; }
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
     sa.sa_handler = on_usr1;
     sigaction(SIGUSR1, &sa, NULL);
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
     a->bar_expected = read_bar_expected();
     printf("soundbar expected: %d (remembered)\n", a->bar_expected);
     poll_arc(a, t0);
     {
          /* a timer set before we started: count from now */
          int st = 0;
          if (biz_get_int(BIZ_KEY_SLEEP_TIMER, &st) == 1 && st > 0)
               sleep_arm(a, st, t0);
     }
     poll_volume(a, t0);
     printf("model: %d inputs, current %s, volume %d, mute %d (%.0f ms)\n",
            a->nin, a->cur, a->vol, a->mute, (vibe_now_s() - t0) * 1000);

     if (a->gam) {
          const char *gn = a->gam_name ? a->gam_name : "vibe";
          if (gam_open(gn, gam_on_sync, gam_on_async) == 0) {
               char buf[128];
               int r = gam_send_cmd("am", "started", gn);
               printf("gam: client '%s' up, started -> r=%d\n", gn, r);
               gam_focus_process(buf, sizeof(buf));
               printf("gam: focus process now '%s'\n", buf);
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
     double refocus_after = 0;
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
                    /* we own every key: if another window (the stock engine
                     * starting after us) takes focus, take it back, at most
                     * once a second */
                    if (we->type == DWET_LOSTFOCUS && now >= refocus_after) {
                         refocus_after = now + 1.0;
                         a->win->RaiseToTop(a->win);
                         printf("RequestFocus again: %s\n",
                                DirectFBErrorString(a->win->RequestFocus(a->win)));
                    }
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

          if (now >= a->next_arc) {
               poll_arc(a, now);
               a->next_arc = now + 2.0;
               /* standby (POWER, or our own switch-off) ends the timer */
               int scr = 1;
               if (a->sleep_deadline && biz_get_int(BIZ_KEY_SCREEN_STATE, &scr) == 1 && scr == 0)
                    sleep_cancel(a, "standby", now, 0);
          }
          sleep_tick(a, now);
          if (now >= a->next_vol) {
               poll_volume(a, now);
               if (now >= a->next_vol)
                    a->next_vol = now + (now < a->follow_until ? 0.1 : 0.4);
          }
          if (now >= a->next_inputs) {
               poll_inputs(a);
               /* faster while a menu is open, or the input has no signal
                * (the notice should go as soon as it returns) */
               a->next_inputs = now + (a->screen != S_NONE || a->nosig_lost ? 1.0 : 3.0);
          }
          check_signal(a, now);
          if (a->screen == S_SETTINGS && now >= a->next_pic) {
               poll_page(a);
               a->next_pic = now + 1.0;
          }
          /* overlays that just expired need one more redraw to vanish */
          if (a->toast_until != last_toast_end && now >= a->toast_until) {
               last_toast_end = a->toast_until;
               a->dirty = 1;
          }
          if (a->vol_until != last_vol_end && now >= a->vol_until) {
               last_vol_end = a->vol_until;
               mark_hud(a);
          }
          dev_commands(a, now);
          if (shot_flag)
               mark_focus(a);       /* a partial frame: the shot shows what
                                     * partial redraws left on screen */
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
