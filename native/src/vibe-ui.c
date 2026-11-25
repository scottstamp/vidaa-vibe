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
     IDirectFBFont         *f_title, *f_head, *f_item, *f_bold, *f_small, *f_tiny;
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

/* the focus look: gradient pill with a soft glow */
static void focus_fill(cv_t *c, int x, int y, int w, int h, int r)
{
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

static const char *const settings_cards[][2] = {
     { "Picture", "Mode, backlight, colour" },
     { "Rename inputs", "Name your devices" },
};
#define N_SETTINGS 2

static void home_card(struct app *a, cv_t *c, int x, int y, int focus, const char *title,
                      const char *sub, int dot_state /* -1 none, 0 off, 1 on */, int now)
{
     if (c) {
          if (focus)
               focus_fill(c, x, y, CARD_W, CARD_H, CARD_R);
          else {
               gfx_rrect(c, x, y, CARD_W, CARD_H, CARD_R, K_CARD);
               gfx_rrect_outline(c, x, y, CARD_W, CARD_H, CARD_R, 1, K_PANEL_EDGE);
          }
          if (dot_state >= 0)
               dot(c, x + 40, y + 52, 14, dot_state ? K_GOOD : (focus ? 0x90ffffff : K_OFF));
     }
     if (now)
          badge(a, c, x + CARD_W - 24, y + 34, "NOW", focus ? 0x40ffffff : 0x333ddc97,
                focus ? K_TEXT : K_GOOD);
     if (!c) {
          if (sub)
               text_v(a, a->f_small, dot_state >= 0 ? x + 60 : x + 32, y + 34, 36,
                      focus ? 0xe6ffffff : K_TEXT2, sub, -1);
          text_v(a, a->f_bold, x + 32, y + CARD_H - 96, 64, K_TEXT, title, -1);
     }
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
          home_card(a, c, HOME_X + k * (CARD_W + CARD_GAP), 646, a->row == 1 && a->col == k,
                    settings_cards[k][0], settings_cards[k][1], -1, 0);

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

static void draw_picture(struct app *a, cv_t *c)
{
     int x0 = r_x(a), y0 = 40, h = a->H - 80, rx = x0 + 28, rw = R_W - 56;
     int top = y0 + 150, row_h = 118;

     sheet(a, c, x0, y0, R_W, h, "Picture");
     for (int i = 0; i < N_PIC; i++) {
          int y = top + i * (row_h + 8), sel = i == a->sel;
          char val[48];
          if (c && sel)
               gfx_rrect(c, rx, y, rw, row_h, 24, K_CARD_HI);
          if (i == 0) {
               const char *m = a->pic_known && a->pic[0] >= 0 && a->pic[0] < N_MODES
                               ? picture_modes[a->pic[0]] : "...";
               int pw = 300, px = rx + rw - 28 - pw, ph = 56, py = y + (row_h - ph) / 2;
               if (c) {
                    if (sel) focus_fill(c, px, py, pw, ph, ph / 2);
                    else gfx_rrect(c, px, py, pw, ph, ph / 2, K_CARD);
               } else {
                    text_v(a, a->f_item, rx + 32, y, row_h, K_TEXT, pic_rows[0].name, -1);
                    text_v(a, a->f_bold, px + pw / 2, py, ph, K_TEXT, m, 0);
                    if (sel) {
                         text_v(a, a->f_bold, px + 26, py, ph, 0xccffffff, "<", 0);
                         text_v(a, a->f_bold, px + pw - 26, py, ph, 0xccffffff, ">", 0);
                    }
               }
               continue;
          }
          snprintf(val, sizeof(val), a->pic_known ? "%d" : "...", a->pic[i]);
          if (c)
               slider(c, rx + 32, y + 82, rw - 64, a->pic_known ? a->pic[i] : 0, sel);
          else {
               text_v(a, a->f_item, rx + 32, y + 12, 56, sel ? K_TEXT : K_TEXT2, pic_rows[i].name, -1);
               text_v(a, a->f_bold, rx + rw - 32, y + 12, 56, K_TEXT, val, 1);
          }
     }
     static const char *const hh[] = { "LEFT/RIGHT", "Adjust", "BACK", "Back" };
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

static const char *preset_sub(struct app *a, int i)
{
     const char *cur = input_label(a, a->rename_idx);
     const char *name = name_presets[i] ? name_presets[i] : a->in[a->rename_idx].id;
     return !strcmp(cur, name) ? "Current" : NULL;
}

static const char *preset_label(struct app *a, int i)
{
     static char buf[64];
     if (name_presets[i])
          return name_presets[i];
     snprintf(buf, sizeof(buf), "Default (%s)", a->in[a->rename_idx].id);
     return buf;
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
     int v = a->vol < 0 ? 0 : a->vol > 100 ? 100 : a->vol;
     int bx = x + 190, bw = w - 190 - 56;
     if (c) {
          gfx_shadow(c, x, y, w, h, h / 2, 24, 0x90000000);
          gfx_rrect(c, x, y, w, h, h / 2, K_PANEL);
          gfx_rrect_outline(c, x, y, w, h, h / 2, 1, K_PANEL_EDGE);
          gfx_rrect(c, bx, y + h / 2 - 5, bw, 10, 5, K_TRACK);
          if (!a->mute && v > 0)
               gfx_rrect_hgrad(c, bx, y + h / 2 - 5, bw * v / 100 < 10 ? 10 : bw * v / 100, 10, 5,
                               K_ACCENT_L, K_ACCENT_R);
     } else {
          char label[16];
          snprintf(label, sizeof(label), a->mute ? "Muted" : "%d", a->vol);
          text_v(a, a->f_tiny, x + 56, y + 14, 34, K_TEXT3, "VOLUME", -1);
          text_v(a, a->mute ? a->f_item : a->f_head, x + 56, y + 40, 54, K_TEXT, label, -1);
     }
}

static void draw_screens(struct app *a, cv_t *c, double now)
{
     switch (a->screen) {
     case S_HOME:         draw_home(a, c); break;
     case S_QUICK:        draw_quick(a, c); break;
     case S_PICTURE:      draw_picture(a, c); break;
     case S_RENAME_INPUT: draw_list(a, c, "Rename inputs", a->nvis, rename_input_label, rename_input_sub); break;
     case S_RENAME_NAME: {
          char t[96];
          snprintf(t, sizeof(t), "Rename %s", input_label(a, a->rename_idx));
          draw_list(a, c, t, N_PRESETS, preset_label, preset_sub);
          break;
     }
     case S_NONE:         break;
     }
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

     if (a->s->Lock(a->s, DSLF_WRITE, &ptr, &pitch) == DFB_OK && ptr) {
          struct canvas c = { ptr, pitch / 4, a->W, a->H };
          gfx_clear(&c);                        /* transparent: the picture shows */
          draw_screens(a, &c, now);
          a->s->Unlock(a->s);
     } else {
          a->s->Clear(a->s, 0, 0, 0, 0);
     }
     a->s->SetDrawingFlags(a->s, DSDRAW_BLEND);
     draw_screens(a, NULL, now);
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
          printf("frame %.0f ms\n", (vibe_now_s() - t0) * 1000);
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
     poll_volume(a, t0);
     poll_picture(a);
     printf("model: %d inputs, current %s, volume %d, mute %d, picture mode %d (%.0f ms)\n",
            a->nin, a->cur, a->vol, a->mute, a->pic[0], (vibe_now_s() - t0) * 1000);

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
          dev_commands(a, now);
          if (shot_flag)
               a->dirty = 1;
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
