/*
 * vibe - first native client for the 58A51HUV's DirectFB world.
 *
 * The stock UI (Cobalt, process hisenseUI) is one DirectFB 1.4.2 slave in
 * fusion world 0, beside systemserver, tvmain, app_man and the browser.
 * systemserver loads the input drivers (IR, keypad, loopback), so keys
 * reach whichever window has focus. vibe joins the same world as one more
 * slave. It never becomes master: session 0 and force-slave are set before
 * DirectFBCreate, so a missing world means a wait, not a fresh hardware init.
 *
 *   vibe probe              read-only: version, screens, layers and their
 *                           configuration, input devices, windows by id.
 *                           Draws nothing.
 *   vibe hello [opts]       one window, text, a key log. Exits on BACK or
 *                           EXIT, on SIGTERM/SIGINT, or after --timeout.
 *   vibe keys [opts]        as hello, but never exits on a key, and also
 *                           logs every key DirectFB sees from any device
 *                           (a global input event buffer), to tell keys that
 *                           reach our window from keys taken before it.
 *
 * Options (before the mode):
 *   --user=UID:GID[:G1,G2,...]   drop to this identity before touching
 *                                DirectFB (the launcher passes UhisUI's)
 *   --layer=N                    layer for hello (default: primary)
 *   --timeout=S                  hello exits after S seconds (default 20)
 *   --font=PATH                  TTF for hello
 *
 * A hard watchdog (alarm) kills the process if DirectFB hangs, so a stuck
 * window cannot outlive timeout + 10 s.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#include <directfb.h>
#include <directfb_version.h>

/* exported by libdirectfb; lets the probe report the real library version */
extern const unsigned int directfb_major_version;
extern const unsigned int directfb_minor_version;
extern const unsigned int directfb_micro_version;

static const char *opt_user;
static int opt_layer = DLID_PRIMARY;
static int opt_timeout = 20;
static const char *opt_font = "/opt/font/Cobalt_fonts/Roboto-Regular.ttf";

static volatile sig_atomic_t stop_flag;

/*
 * The library copies its own sizeof() into these. The vendor build changed
 * interface layouts (see tools/mstar_headers.py); its data structs have not
 * been measured, so every buffer the library fills gets slack.
 */
/* PADDED() is in common.h */


static void on_signal(int sig) { (void)sig; stop_flag = 1; }

#define CHECK(what, call)                                                    \
     do {                                                                    \
          DFBResult r_ = (call);                                             \
          if (r_ != DFB_OK) {                                                \
               fprintf(stderr, "vibe: %s failed: %s (%d)\n", what,           \
                       DirectFBErrorString(r_), (int)r_);                    \
               goto fail;                                                    \
          }                                                                  \
     } while (0)

/* ------------------------------------------------------------------ probe */

static const char *fmt_name(DFBSurfacePixelFormat f)
{
     switch (f) {
     case DSPF_ARGB:     return "ARGB";
     case DSPF_RGB32:    return "RGB32";
     case DSPF_RGB16:    return "RGB16";
     case DSPF_ARGB4444: return "ARGB4444";
     case DSPF_ARGB1555: return "ARGB1555";
     case DSPF_LUT8:     return "LUT8";
     case DSPF_YUY2:     return "YUY2";
     case DSPF_UYVY:     return "UYVY";
     case DSPF_NV12:     return "NV12";
     case DSPF_A8:       return "A8";
     default:            return "?";
     }
}

static DFBEnumerationResult screen_cb(DFBScreenID id, DFBScreenDescription d, void *ctx)
{
     (void)ctx;
     printf("screen %u: name='%.32s' caps=0x%x mixers=%d encoders=%d outputs=%d\n",
            id, d.name, d.caps, d.mixers, d.encoders, d.outputs);
     return DFENUM_OK;
}

/*
 * Enumeration callbacks get the description struct by value. If the vendor
 * struct is bigger than upstream's, callbackdata lands somewhere else on the
 * stack, so callbacks never use their ctx argument: they use g_dfb.
 */
static IDirectFB *g_dfb;

static DFBEnumerationResult layer_cb(DFBDisplayLayerID id, DFBDisplayLayerDescription d, void *ctx)
{
     IDirectFB *dfb = g_dfb;
     IDirectFBDisplayLayer *layer = NULL;
     PADDED(DFBDisplayLayerConfig, cfgp);
     DFBDisplayLayerConfig *cfgv = &cfgp.v;
     DFBResult r;
     int level = 0x7fffffff;

     printf("layer %u: name='%.32s' type=0x%x caps=0x%x level=%d regions=%d sources=%d\n",
            id, d.name, d.type, d.caps, d.level, d.regions, d.sources);

     r = dfb->GetDisplayLayer(dfb, id, &layer);
     if (r != DFB_OK) {
          printf("  GetDisplayLayer: %s\n", DirectFBErrorString(r));
          return DFENUM_OK;
     }
     memset(&cfgp, 0, sizeof(cfgp));
     r = layer->GetConfiguration(layer, cfgv);
     if (r == DFB_OK)
          printf("  config: flags=0x%x %dx%d format=%s(0x%x) buffermode=0x%x options=0x%x\n",
                 cfgv->flags, cfgv->width, cfgv->height, fmt_name(cfgv->pixelformat),
                 (unsigned)cfgv->pixelformat, cfgv->buffermode, cfgv->options);
     else
          printf("  GetConfiguration: %s\n", DirectFBErrorString(r));

     r = layer->GetLevel(layer, &level);
     printf("  level: %s %d\n", r == DFB_OK ? "ok" : DirectFBErrorString(r), level);

     /* Windows by id. On this build GetWindow answers OK with the same data
      * for every id, so this lists nothing real; kept to a few ids. */
     for (DFBWindowID wid = 0; wid < 3; wid++) {
          IDirectFBWindow *w = NULL;
          int x = 0, y = 0, ww = 0, wh = 0;
          u8 op = 0;
          if (layer->GetWindow(layer, wid, &w) != DFB_OK || !w)
               continue;
          w->GetPosition(w, &x, &y);
          w->GetSize(w, &ww, &wh);
          w->GetOpacity(w, &op);
          printf("  window %u: %d,%d %dx%d opacity=%u\n", wid, x, y, ww, wh, op);
          w->Release(w);
     }
     layer->Release(layer);
     return DFENUM_OK;
}

static DFBEnumerationResult input_cb(DFBInputDeviceID id, DFBInputDeviceDescription d, void *ctx)
{
     (void)ctx;
     printf("input %u: name='%.32s' vendor='%.40s' type=0x%x caps=0x%x min_keycode=%d max_keycode=%d\n",
            id, d.name, d.vendor, d.type, d.caps, d.min_keycode, d.max_keycode);
     return DFENUM_OK;
}

static int do_probe(IDirectFB *dfb)
{
     printf("library %u.%u.%u, headers %d.%d.%d\n",
            directfb_major_version, directfb_minor_version, directfb_micro_version,
            DIRECTFB_MAJOR_VERSION, DIRECTFB_MINOR_VERSION, DIRECTFB_MICRO_VERSION);
     DFBResult r;
     r = dfb->EnumScreens(dfb, screen_cb, NULL);
     printf("EnumScreens: %s\n", DirectFBErrorString(r));
     r = dfb->EnumDisplayLayers(dfb, layer_cb, NULL);
     printf("EnumDisplayLayers: %s\n", DirectFBErrorString(r));
     /* enumeration may be filtered for slaves; ask for each id directly */
     for (int id = 0; id < 4; id++) {
          PADDED(DFBDisplayLayerDescription, dp);
          IDirectFBDisplayLayer *l = NULL;
          r = dfb->GetDisplayLayer(dfb, id, &l);
          printf("GetDisplayLayer(%d): %s\n", id, DirectFBErrorString(r));
          if (r != DFB_OK)
               continue;
          memset(&dp, 0, sizeof(dp));
          r = l->GetDescription(l, &dp.v);
          l->Release(l);
          if (r == DFB_OK)
               layer_cb(id, dp.v, NULL);
          else
               printf("  GetDescription: %s\n", DirectFBErrorString(r));
     }
     r = dfb->EnumInputDevices(dfb, input_cb, NULL);
     printf("EnumInputDevices: %s\n", DirectFBErrorString(r));
     return 0;
}

/* ------------------------------------------------------------------ hello */

#define LOG_LINES 14

static int opt_keys;   /* "keys" mode: log every key, never exit on one */

static const char *key_name(DFBInputDeviceKeySymbol k)
{
     switch (k) {
     case DIKS_CURSOR_UP:     return "UP";
     case DIKS_CURSOR_DOWN:   return "DOWN";
     case DIKS_CURSOR_LEFT:   return "LEFT";
     case DIKS_CURSOR_RIGHT:  return "RIGHT";
     case DIKS_RETURN:        return "RETURN";
     case DIKS_OK:            return "OK";
     case DIKS_SELECT:        return "SELECT";
     case DIKS_BACK:          return "BACK";
     case DIKS_EXIT:          return "EXIT";
     case DIKS_ESCAPE:        return "ESCAPE";
     case DIKS_MENU:          return "MENU";
     case DIKS_HOME:          return "HOME";
     case DIKS_POWER:         return "POWER";
     case DIKS_POWER2:        return "POWER2";
     case DIKS_VOLUME_UP:     return "VOLUME_UP";
     case DIKS_VOLUME_DOWN:   return "VOLUME_DOWN";
     case DIKS_MUTE:          return "MUTE";
     case DIKS_CHANNEL_UP:    return "CHANNEL_UP";
     case DIKS_CHANNEL_DOWN:  return "CHANNEL_DOWN";
     case DIKS_INFO:          return "INFO";
     case DIKS_TV:            return "TV";
     case DIKS_PLAYPAUSE:     return "PLAYPAUSE";
     case DIKS_PLAY:          return "PLAY";
     case DIKS_PAUSE:         return "PAUSE";
     case DIKS_STOP:          return "STOP";
     case DIKS_REWIND:        return "REWIND";
     case DIKS_FASTFORWARD:   return "FASTFORWARD";
     case DIKS_RED:           return "RED";
     case DIKS_GREEN:         return "GREEN";
     case DIKS_YELLOW:        return "YELLOW";
     case DIKS_BLUE:          return "BLUE";
     default:                 break;
     }
     if (k >= DIKS_0 && k <= DIKS_9) return "DIGIT";
     return "?";
}

static void log_add(char log[LOG_LINES][96], int *nlog, const char *line)
{
     if (*nlog == LOG_LINES) {
          memmove(log[0], log[1], sizeof(log[0]) * (LOG_LINES - 1));
          (*nlog)--;
     }
     snprintf(log[(*nlog)++], sizeof(log[0]), "%s", line);
}

static void draw(IDirectFBSurface *s, IDirectFBFont *font, int w, int h,
                 char log[LOG_LINES][96], int nlog, double left)
{
     int fh = 0;
     char buf[96];

     font->GetHeight(font, &fh);
     s->SetDrawingFlags(s, DSDRAW_NOFX);
     s->Clear(s, 0x10, 0x14, 0x1c, 0xff);

     /* header bar */
     s->SetColor(s, 0x2a, 0x6f, 0xdb, 0xff);
     s->FillRectangle(s, 0, 0, w, fh * 2);

     s->SetFont(s, font);
     s->SetColor(s, 0xff, 0xff, 0xff, 0xff);
     s->DrawString(s, opt_keys ? "vidaa-vibe native: key test (press every remote key once)"
                               : "vidaa-vibe native: hello from DirectFB",
                   -1, fh / 2, fh / 2, DSTF_TOPLEFT);

     if (opt_keys)
          snprintf(buf, sizeof(buf), "WIN = reached this window, DEV = seen by DirectFB   closes in %.0f s", left);
     else
          snprintf(buf, sizeof(buf), "surface %dx%d   closes in %.0f s, or press BACK / EXIT", w, h, left);
     s->SetColor(s, 0xc8, 0xd0, 0xe0, 0xff);
     s->DrawString(s, buf, -1, fh / 2, fh * 3, DSTF_TOPLEFT);

     for (int i = 0; i < nlog; i++)
          s->DrawString(s, log[i], -1, fh / 2, fh * (5 + i), DSTF_TOPLEFT);

     s->Flip(s, NULL, DSFLIP_WAITFORSYNC);
}

static int do_hello(IDirectFB *dfb)
{
     IDirectFBDisplayLayer *layer = NULL;
     IDirectFBWindow *win = NULL;
     IDirectFBSurface *surf = NULL;
     IDirectFBEventBuffer *events = NULL;
     IDirectFBEventBuffer *devents = NULL;   /* keys mode: all key devices */
     IDirectFBFont *font = NULL;
     PADDED(DFBDisplayLayerConfig, cfgp);
     DFBDisplayLayerConfig cfg;
     PADDED(DFBWindowDescription, wdp);
     PADDED(DFBFontDescription, fdp);
     DFBWindowDescription *wd = &wdp.v;
     DFBFontDescription *fd = &fdp.v;
     char log[LOG_LINES][96];
     int nlog = 0, w, h, ret = 1;
     double deadline = vibe_now_s() + opt_timeout, last_left = -1;

     CHECK("GetDisplayLayer", dfb->GetDisplayLayer(dfb, opt_layer, &layer));
     memset(&cfgp, 0, sizeof(cfgp));
     CHECK("GetConfiguration", layer->GetConfiguration(layer, &cfgp.v));
     cfg = cfgp.v;
     printf("layer %d is %dx%d %s\n", opt_layer, cfg.width, cfg.height, fmt_name(cfg.pixelformat));
     if (cfg.width < 64 || cfg.height < 64 || cfg.width > 4096 || cfg.height > 4096) {
          fprintf(stderr, "vibe: layer size looks wrong, not creating a window\n");
          goto fail;
     }

     /* inset window: the stock UI stays visible around it */
     w = cfg.width * 3 / 4;
     h = cfg.height * 3 / 4;

     memset(&wdp, 0, sizeof(wdp));
     wd->flags = DWDESC_POSX | DWDESC_POSY | DWDESC_WIDTH | DWDESC_HEIGHT |
                 DWDESC_CAPS | DWDESC_PIXELFORMAT | DWDESC_STACKING;
     wd->posx = (cfg.width - w) / 2;
     wd->posy = (cfg.height - h) / 2;
     wd->width = w;
     wd->height = h;
     wd->caps = DWCAPS_DOUBLEBUFFER;
     wd->pixelformat = DSPF_ARGB;
     wd->stacking = DWSC_UPPER;
     CHECK("CreateWindow", layer->CreateWindow(layer, wd, &win));
     CHECK("Window GetSurface", win->GetSurface(win, &surf));
     CHECK("Window CreateEventBuffer", win->CreateEventBuffer(win, &events));

     memset(&fdp, 0, sizeof(fdp));
     fd->flags = DFDESC_HEIGHT;
     fd->height = h / 20 > 12 ? h / 20 : 12;
     CHECK("CreateFont", dfb->CreateFont(dfb, opt_font, fd, &font));

     if (opt_keys) {
          /* global: deliver device events even when another window has focus */
          DFBResult r = dfb->CreateInputEventBuffer(dfb, DICAPS_KEYS, DFB_TRUE, &devents);
          printf("CreateInputEventBuffer: %s\n", DirectFBErrorString(r));
          if (r != DFB_OK)
               devents = NULL;
     }

     draw(surf, font, w, h, log, nlog, opt_timeout);
     CHECK("SetOpacity", win->SetOpacity(win, 0xff));
     win->RaiseToTop(win);
     {
          DFBResult r = win->RequestFocus(win);
          printf("RequestFocus: %s\n", DirectFBErrorString(r));
     }
     fflush(stdout);

     while (!stop_flag) {
          double left = deadline - vibe_now_s();
          PADDED(DFBEvent, evp);
          DFBWindowEvent ev;
          int dirty = 0;

          if (left <= 0) {
               printf("timeout\n");
               break;
          }
          events->WaitForEventWithTimeout(events, 0, devents ? 50 : 200);
          while (events->GetEvent(events, &evp.v) == DFB_OK) {
               ev = evp.v.window;
               if (evp.v.clazz != DFEC_WINDOW)
                    continue;
               if (ev.type == DWET_GOTFOCUS || ev.type == DWET_LOSTFOCUS) {
                    printf("%s\n", ev.type == DWET_GOTFOCUS ? "got focus" : "lost focus");
                    log_add(log, &nlog, ev.type == DWET_GOTFOCUS ? "-- got focus" : "-- LOST FOCUS");
                    dirty = 1;
                    continue;
               }
               if (ev.type != DWET_KEYDOWN && ev.type != DWET_KEYUP)
                    continue;
               printf("WIN %s %-12s symbol=0x%x id=0x%x code=%d mods=0x%x\n",
                      ev.type == DWET_KEYDOWN ? "down" : "up  ", key_name(ev.key_symbol),
                      ev.key_symbol, ev.key_id, ev.key_code, ev.modifiers);
               if (ev.type != DWET_KEYDOWN)
                    continue;
               {
                    char line[96];
                    snprintf(line, sizeof(line), "WIN %-12s symbol=0x%04x code=%d",
                             key_name(ev.key_symbol), ev.key_symbol, ev.key_code);
                    log_add(log, &nlog, line);
               }
               dirty = 1;
               if (!opt_keys && (ev.key_symbol == DIKS_BACK || ev.key_symbol == DIKS_EXIT ||
                                 ev.key_symbol == DIKS_ESCAPE))
                    stop_flag = 1;
          }
          while (devents && devents->GetEvent(devents, &evp.v) == DFB_OK) {
               DFBInputEvent ie = evp.v.input;
               char line[96];
               if (evp.v.clazz != DFEC_INPUT)
                    continue;
               if (ie.type != DIET_KEYPRESS && ie.type != DIET_KEYRELEASE)
                    continue;
               printf("DEV %s %-12s symbol=0x%x id=0x%x code=%d device=%u\n",
                      ie.type == DIET_KEYPRESS ? "down" : "up  ", key_name(ie.key_symbol),
                      ie.key_symbol, ie.key_id, ie.key_code, ie.device_id);
               if (ie.type != DIET_KEYPRESS)
                    continue;
               snprintf(line, sizeof(line), "DEV %-12s symbol=0x%04x code=%d dev=%u",
                        key_name(ie.key_symbol), ie.key_symbol, ie.key_code, ie.device_id);
               log_add(log, &nlog, line);
               dirty = 1;
          }
          if ((int)left != (int)last_left) {
               last_left = left;
               dirty = 1;
          }
          if (dirty && !stop_flag)
               draw(surf, font, w, h, log, nlog, left);
     }
     ret = 0;

fail:
     if (win)
          win->SetOpacity(win, 0);
     if (font) font->Release(font);
     if (devents) devents->Release(devents);
     if (events) events->Release(events);
     if (surf) surf->Release(surf);
     if (win) {
          win->Destroy(win);
          win->Release(win);
     }
     if (layer) layer->Release(layer);
     return ret;
}

/* ------------------------------------------------------------------- main */

static void usage(void)
{
     fprintf(stderr,
             "usage: vibe [--user=UID:GID[:G,...]] [--layer=N] [--timeout=S] [--font=PATH] probe|hello|keys\n");
}

int main(int argc, char **argv)
{
     IDirectFB *dfb = NULL;
     const char *mode = NULL;
     int ret = 1;
     struct sigaction sa;

     for (int i = 1; i < argc; i++) {
          const char *a = argv[i];
          if (!strncmp(a, "--user=", 7))          opt_user = a + 7;
          else if (!strncmp(a, "--layer=", 8))    opt_layer = atoi(a + 8);
          else if (!strncmp(a, "--timeout=", 10)) opt_timeout = atoi(a + 10);
          else if (!strncmp(a, "--font=", 7))     opt_font = a + 7;
          else if (a[0] != '-' && !mode)          mode = a;
          else { usage(); return 2; }
     }
     if (mode && !strcmp(mode, "keys")) {
          opt_keys = 1;
          mode = "hello";
     }
     if (!mode || (strcmp(mode, "probe") && strcmp(mode, "hello"))) {
          usage();
          return 2;
     }
     if (opt_timeout < 1 || opt_timeout > 600)
          opt_timeout = 20;

     setvbuf(stdout, NULL, _IOLBF, 0);

     memset(&sa, 0, sizeof(sa));
     sa.sa_handler = on_signal;
     sigaction(SIGTERM, &sa, NULL);
     sigaction(SIGINT, &sa, NULL);
     sigaction(SIGHUP, &sa, NULL);
     /* last resort: default SIGALRM action kills us if DirectFB wedges */
     alarm((unsigned)opt_timeout + 10);

     if (opt_user && vibe_drop_privileges(opt_user))
          return 1;
     printf("vibe: uid=%d gid=%d mode=%s\n", (int)getuid(), (int)getgid(), mode);

     {
          /* DirectFBInit wants argc/argv; give it none of ours */
          int dargc = 1;
          char *dargv_store[] = { argv[0], NULL };
          char **dargv = dargv_store;
          CHECK("DirectFBInit", DirectFBInit(&dargc, &dargv));
     }
     /* join world 0 as a slave, never start a new master */
     CHECK("SetOption session", DirectFBSetOption("session", "0"));
     CHECK("SetOption force-slave", DirectFBSetOption("force-slave", NULL));
     CHECK("SetOption no-banner", DirectFBSetOption("no-banner", NULL));
     CHECK("DirectFBCreate", DirectFBCreate(&dfb));

     g_dfb = dfb;
     ret = !strcmp(mode, "probe") ? do_probe(dfb) : do_hello(dfb);

fail:
     if (dfb)
          dfb->Release(dfb);
     alarm(0);
     printf("vibe: exit %d\n", ret);
     return ret;
}
