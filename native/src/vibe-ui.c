/*
 * vibe-ui - the native UI for the 58A51HUV. First skeleton.
 *
 * One process, two vendor services:
 *   - DirectFB (world 0, slave): one full-screen ARGB window that keeps
 *     focus. Where nothing is drawn it is fully transparent, so the HDMI
 *     picture underneath stays visible.
 *   - the kilby model (biz.c): input list, current input, volume, mute, and
 *     the source.switch action.
 *
 * Keys (DirectFB symbols measured with `vibe keys`, see the findings):
 *   HOME, MENU, INPUT   open the input panel (INPUT again moves down)
 *   UP / DOWN           move in the panel
 *   OK                  switch to the selected input, close the panel
 *   BACK / EXIT         close the panel
 *   POWER               standby, the stock UI's own call (SwitchOffTv)
 *   VOLUME / MUTE       we set the model value (kilby.sl2.i32.sound.main.*)
 *                       and show our indicator
 * Key repeats that the IR device re-injects with key_code -1 are dropped.
 *
 * Meant to run with the stock engine booted from slot B (native/slotb: an
 * empty page), so nothing else draws or acts on keys. With the stock UI
 * in slot A, its overlays appear above ours and volume keys act twice.
 *
 * usage: vibe-ui [--user=UID:GID[:G,...]] [--timeout=S] [--layer=N]
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

#define FONT_REG  "/opt/font/Cobalt_fonts/Roboto-Regular.ttf"
#define FONT_BOLD "/opt/font/Cobalt_fonts/Roboto-Bold.ttf"

/* remote keys without a DirectFB name (vibe keys, 2026-11-25) */
#define KEY_INPUT  0xf068

static volatile sig_atomic_t stop_flag;
static void on_signal(int sig) { (void)sig; stop_flag = 1; }

struct app {
     IDirectFB             *dfb;
     IDirectFBDisplayLayer *layer;
     IDirectFBWindow       *win;
     IDirectFBSurface      *s;
     IDirectFBEventBuffer  *events;
     IDirectFBFont         *f_title, *f_item, *f_small;
     int W, H;

     struct biz_input in[BIZ_MAX_INPUTS];
     int  nin;
     char cur[32];
     int  vol, mute, vol_known;

     int    panel, sel;
     char   toast[64];
     double toast_until, vol_until;
     double next_inputs, next_vol;
     int    dirty;
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

static int input_index(struct app *a, const char *id)
{
     for (int i = 0; i < a->nin; i++)
          if (!strcmp(a->in[i].id, id))
               return i;
     return -1;
}

static void draw_panel(struct app *a)
{
     const int pw = 640, x0 = 64, row = 112, top = 220;

     fill(a, 0, 0, pw, a->H, 0xec101622);
     fill(a, pw, 0, 4, a->H, 0xff2a6fdb);
     text(a, a->f_title, x0, 96, 0xffffffff, "Inputs", DSTF_TOPLEFT);

     for (int i = 0; i < a->nin; i++) {
          struct biz_input *in = &a->in[i];
          int y = top + i * row;
          int cur = !strcmp(in->id, a->cur);

          if (i == a->sel)
               fill(a, 32, y, pw - 64, row - 16, 0xff2a6fdb);
          else if (cur)
               fill(a, 32, y, pw - 64, row - 16, 0xff1d2a40);

          text(a, a->f_item, x0, y + 22, 0xffffffff, in->label, DSTF_TOPLEFT);
          {
               char sub[64];
               snprintf(sub, sizeof(sub), "%s%s", in->signal ? "signal" : "no signal",
                        cur ? "   \xc2\xb7   now showing" : "");
               text(a, a->f_small, x0, y + 66, in->signal ? 0xff9ff0b0 : 0xffa0a8b8,
                    sub, DSTF_TOPLEFT);
          }
     }
     text(a, a->f_small, x0, a->H - 90, 0xffa0a8b8, "OK  switch        BACK  close",
          DSTF_TOPLEFT);
}

static void draw_toast(struct app *a)
{
     int w = 480, h = 120, x = 64, y = 64;
     fill(a, x, y, w, h, 0xe0101622);
     fill(a, x, y, 8, h, 0xff2a6fdb);
     text(a, a->f_title, x + 40, y + h / 2, 0xffffffff, a->toast, DSTF_LEFT);
}

static void draw_volume(struct app *a)
{
     int w = 760, h = 120, x = (a->W - w) / 2, y = a->H - 220;
     char label[32];
     int bar = w - 260, v = a->vol < 0 ? 0 : a->vol > 100 ? 100 : a->vol;

     fill(a, x, y, w, h, 0xe0101622);
     if (a->mute)
          snprintf(label, sizeof(label), "Muted");
     else
          snprintf(label, sizeof(label), "%d", a->vol);
     text(a, a->f_title, x + 40, y + h / 2, 0xffffffff, label, DSTF_LEFT);
     fill(a, x + 220, y + h / 2 - 6, bar, 12, 0xff3a4458);
     if (!a->mute)
          fill(a, x + 220, y + h / 2 - 6, bar * v / 100, 12, 0xff2a6fdb);
}

static void redraw(struct app *a, double now)
{
     a->s->Clear(a->s, 0, 0, 0, 0);    /* transparent: the picture shows */
     if (a->panel)
          draw_panel(a);
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
          if (a->sel >= n)
               a->sel = n - 1;
          a->dirty = a->panel;
     }
     if (cur[0] && strcmp(cur, a->cur)) {
          printf("current input: %s\n", cur);
          snprintf(a->cur, sizeof(a->cur), "%s", cur);
          a->dirty = a->panel;
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

/* ---------------------------------------------------------------- keys */

static void open_panel(struct app *a)
{
     int i = input_index(a, a->cur);
     a->panel = 1;
     a->sel = i >= 0 ? i : 0;
     a->next_inputs = 0;    /* refresh signal state now */
     a->dirty = 1;
}

static void on_key(struct app *a, DFBInputDeviceKeySymbol k, double now)
{
     switch ((int)k) {   /* includes vendor symbols outside the enum */
     case DIKS_HOME:
     case DIKS_MENU:
          if (a->panel) { a->panel = 0; a->dirty = 1; }
          else open_panel(a);
          return;
     case DIKS_POWER:
     case DIKS_POWER2: {
          /* what the stock UI's power-off does: modeljs.system.SwitchOffTv([1])
           * = action "kilby.action.system.switch.off.tv" ["1"] (read in
           * libcobalt). Not under the sl2 prefix. */
          /* DISABLED: switch.off.tv enters a fast standby (Linux keeps
           * running, screen state 0) that nothing in our setup wakes from:
           * POWER then reaches us again, and with the stock page blanked the
           * system does not wake on its own (2026-11-25). Until the wake
           * path is mapped, POWER is only logged. */
          /* Stock: POWER is handled natively (app_man -> apm_bond), not by
           * the UI, once the UI has reported gui.loop.finished = 1 (set in
           * main). We leave it to the system. */
          printf("power key: left to the system\n");
          (void)BIZ_ACTION_POWER_OFF;
          return;
     }
     case KEY_INPUT:
          if (!a->panel) open_panel(a);
          else if (a->nin) { a->sel = (a->sel + 1) % a->nin; a->dirty = 1; }
          return;
     case DIKS_VOLUME_UP:
     case DIKS_VOLUME_DOWN:
     case DIKS_MUTE: {
          /* With the stock UI blanked nobody else acts on these: the stock
           * page used to call modeljs.sound.setMainVolume. We set the model
           * value ourselves (setIntValue measured to take effect). */
          int r;
          if (k == DIKS_MUTE) {
               a->mute = !a->mute;
               r = biz_set_int(BIZ_KEY_MUTE, a->mute);
          } else {
               int v = a->vol + (k == DIKS_VOLUME_UP ? 1 : -1);
               a->vol = v < 0 ? 0 : v > 100 ? 100 : v;
               if (a->mute) {               /* volume keys unmute, like most TVs */
                    a->mute = 0;
                    biz_set_int(BIZ_KEY_MUTE, 0);
               }
               r = biz_set_int(BIZ_KEY_VOLUME, a->vol);
          }
          printf("volume %d mute %d (set r=%d)\n", a->vol, a->mute, r);
          a->vol_until = now + 2.5;
          a->next_vol = now + 0.5;   /* re-read soon to confirm */
          a->dirty = 1;
          return;
     }
     default:
          break;
     }
     if (!a->panel)
          return;
     switch (k) {
     case DIKS_CURSOR_UP:
          if (a->sel > 0) { a->sel--; a->dirty = 1; }
          break;
     case DIKS_CURSOR_DOWN:
          if (a->sel + 1 < a->nin) { a->sel++; a->dirty = 1; }
          break;
     case DIKS_OK:
     case DIKS_RETURN:
     case DIKS_SELECT:
          if (a->sel >= 0 && a->sel < a->nin) {
               struct biz_input *in = &a->in[a->sel];
               if (strcmp(in->id, a->cur)) {
                    int r = biz_switch_input(in->id);
                    printf("switch to %s -> r=%d\n", in->id, r);
                    a->next_inputs = now + 0.5;
               }
               snprintf(a->toast, sizeof(a->toast), "%s", in->label);
               a->toast_until = now + 3;
          }
          a->panel = 0;
          a->dirty = 1;
          break;
     case DIKS_BACK:
     case DIKS_EXIT:
          a->panel = 0;
          a->dirty = 1;
          break;
     default:
          break;
     }
}

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
     if (a->W < 640 || a->H < 360 || a->W > 4096 || a->H > 4096) {
          fprintf(stderr, "vibe-ui: layer size %dx%d looks wrong\n", a->W, a->H);
          return -1;
     }

     memset(&wd, 0, sizeof(wd));
     wd.v.flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
                  DWDESC_CAPS | DWDESC_PIXELFORMAT | DWDESC_STACKING;
     wd.v.posx = 0;
     wd.v.posy = 0;
     wd.v.width = a->W;
     wd.v.height = a->H;
     wd.v.caps = DWCAPS_ALPHACHANNEL | DWCAPS_DOUBLEBUFFER;
     wd.v.pixelformat = DSPF_ARGB;
     wd.v.stacking = DWSC_UPPER;
     CHECK("CreateWindow", a->layer->CreateWindow(a->layer, &wd.v, &a->win));
     CHECK("GetSurface", a->win->GetSurface(a->win, &a->s));
     CHECK("CreateEventBuffer", a->win->CreateEventBuffer(a->win, &a->events));

     if (font(a, FONT_BOLD, 64, &a->f_title) || font(a, FONT_REG, 44, &a->f_item) ||
         font(a, FONT_REG, 30, &a->f_small))
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
          else {
               fprintf(stderr, "usage: vibe-ui [--user=UID:GID[:G,...]] [--timeout=S] [--layer=N]\n");
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
      * its init (uiLoadFinish(1)); without it POWER did nothing */
     {
          int v = -1, r;
          biz_get_int(BIZ_KEY_GUI_LOOP_FINISHED, &v);
          r = biz_set_int(BIZ_KEY_GUI_LOOP_FINISHED, 1);
          printf("gui.loop.finished was %d, set 1 -> r=%d\n", v, r);
     }
     poll_inputs(a);
     poll_volume(a, t0);
     printf("model: %d inputs, current %s, volume %d, mute %d (%.0f ms)\n",
            a->nin, a->cur, a->vol, a->mute, (vibe_now_s() - t0) * 1000);

     if (setup(a, layer)) {
          teardown(a);
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
               printf("key 0x%x code %d\n", we->key_symbol, we->key_code);
               on_key(a, we->key_symbol, now);
          }

          if (now >= a->next_vol) {
               poll_volume(a, now);
               a->next_vol = now + 0.4;
          }
          if (now >= a->next_inputs) {
               poll_inputs(a);
               a->next_inputs = now + (a->panel ? 1.0 : 3.0);
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
     biz_close();
     alarm(0);
     printf("vibe-ui: exit\n");
     return 0;
}
