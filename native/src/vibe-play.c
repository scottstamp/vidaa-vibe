/*
 * vibe-play: plays one network video through the TV's hardware player, for
 * vibe-ui's Jellyfin client. A child process, so a crash in the vendor
 * media stack cannot take the UI down. It opens no DirectFB and never
 * touches the volume.
 *
 * Player: Hisense's HMP API in /usr/local/lib/libhmplayer.so (dlopen), the
 * one the stock browser's media backend uses for URL playback. Call order
 * and signatures were recovered from that backend
 * (docs/findings/07-jellyfin.md, "Playback path"); the parts marked UNSURE
 * below are guesses to check on the first run.
 *
 * Protocol, one line each way (the URL carries a token, so it comes over
 * stdin, never argv):
 *   in:  open URL [START_MS]   (first line)
 *        pause | resume | seek MS | stop | rect X Y W H Z | pts
 *   out: state opening|prepared|playing|paused|stopped
 *        pos MS RAW0 RAW1 RAW2   (every second while playing; MS is our
 *                                 estimate, RAW* what HMPGetPts gave)
 *        event N EXTRA           (every player callback, raw)
 *        ended | error TEXT | exit
 * stdin closing means stop.
 *
 * usage: vibe-play [--user=UID:GID[:G,...]] [--uri-word=N] [--mime=TYPE]
 *                  [--mime-word=N] [--rect=X,Y,W,H,Z] [--app-type=N]
 *                  [--kind=N] [--no-start-seek] [--probe]
 *                  [--av-request] [--media-status] [--dmp-path] [--no-aout-init]
 * --probe: load the library and resolve the symbols, then exit (no player
 * is made, nothing shows)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "biz.h"
#include "common.h"

/* ---- the HMP ABI (UNSURE where marked) ---------------------------------- */

typedef void *hmp_t;

struct hmp_create {          /* HMPCreate's param: 4 ints */
     int backend_id;          /* the browser passes its own id; UNSURE */
     int app_type;            /* 3 in the browser */
     int data_type;           /* 3 in the browser */
     int kind;                /* 32 = URL player (16 = MSE) */
};
struct hmp_uri { uintptr_t w[12]; };   /* by value; URL in w[0], MIME type in w[4] (seen in the vendor log) */
struct hmp_rect { int v[5]; };         /* by value; field order UNSURE */
struct hmp_pts { uint64_t pts; int64_t b; int c; int pad[8]; };  /* UNSURE */

typedef void (*hmp_cb)(hmp_t h, int ev, void *extra);

static struct {
     void *lib;
     int (*Init)(void);
     int (*Create)(struct hmp_create *, hmp_t *);
     int (*RegisterCallback)(hmp_t, hmp_cb);
     int (*UnRegisterCallback)(hmp_t);
     int (*SetURI)(hmp_t, struct hmp_uri);
     int (*Prepare)(hmp_t);
     int (*Start)(hmp_t);
     int (*Pause)(hmp_t);
     int (*Resume)(hmp_t);
     int (*Seek)(hmp_t, uint64_t ms);
     int (*Stop)(hmp_t);
     int (*Destroy)(hmp_t);
     int (*GetPts)(hmp_t, struct hmp_pts *);
     int (*SetDisplayRect)(hmp_t, struct hmp_rect, struct hmp_rect, unsigned);
     int (*SetComponentMuteState)(hmp_t, int, int);
     int (*SetSubtitleShowState)(hmp_t, int);
} H;

/* ---- output ------------------------------------------------------------- */

static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
/* the protocol stream: our stdout as we got it. fd 1 itself is pointed at
 * stderr, because the vendor libraries printf to stdout */
static FILE *proto;

static void say(const char *fmt, ...)
{
     va_list ap;
     FILE *f = proto ? proto : stdout;
     pthread_mutex_lock(&out_lock);
     va_start(ap, fmt);
     vfprintf(f, fmt, ap);
     va_end(ap);
     fputc('\n', f);
     fflush(f);
     pthread_mutex_unlock(&out_lock);
}

/* ---- state -------------------------------------------------------------- */

static hmp_t player;
static int av_request, media_status, dmp_path, model_ok = -1;
static char prev_input[64];
static int aout_init = 1, data_type = 3, do_init, uri_word = 0, mime_word = 4, app_type = 3, kind = 32, start_seek = 1;
static const char *mime;
static struct hmp_rect rect = { { 0, 0, 1920, 1080, 0 } };
static char url[4096];
static uint64_t start_ms;

static pthread_mutex_t st_lock = PTHREAD_MUTEX_INITIALIZER;
static int playing, paused, started;
/* our own clock: position = base + (now - since) while playing */
static double pos_base_ms, pos_since;

static double now_ms(void) { return vibe_now_s() * 1000.0; }

static double position(void)
{
     pthread_mutex_lock(&st_lock);
     double p = pos_base_ms + (playing && !paused ? now_ms() - pos_since : 0);
     pthread_mutex_unlock(&st_lock);
     return p;
}

static void clock_set(double ms, int run)
{
     pthread_mutex_lock(&st_lock);
     pos_base_ms = ms;
     pos_since = now_ms();
     paused = !run;
     pthread_mutex_unlock(&st_lock);
}

static void on_event(hmp_t h, int ev, void *extra)
{
     say("event %d %ld", ev, (long)(intptr_t)extra);
     uintptr_t x = (uintptr_t)extra;
     if (getenv("VIBE_PLAY_DUMP") && x > 0x10000 && x < 0xc0000000u) {
          const uint32_t *w = extra;
          say("event-data %08x %08x %08x %08x %08x %08x %08x %08x",
              w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
          for (int i = 0; i < 4; i++) {
               uintptr_t q = w[i];
               if (q > 0x10000 && q < 0xc0000000u) {
                    const char *c = (const char *)q;
                    char buf[64]; int n = 0;
                    while (n < 63 && c[n] >= 32 && c[n] < 127) { buf[n] = c[n]; n++; }
                    buf[n] = 0;
                    if (n >= 3) say("event-str[%d] %s", i, buf);
               }
          }
     }
}

static int load(void)
{
     /* libhmplayer needs dfbrc_client_* without naming the library that has
      * them (the browser's backend loads it first). Without it the dlopen
      * fails partway through relocation; a second dlopen then returns the
      * half-relocated library and its first PLT call jumps to garbage. */
     static const char *const pre[] = {
          "libdirectfb-1.4.so.0", "/usr/local/lib/libdfb_renderclient.so",
          /* libgsthsplayer (a GStreamer plugin) uses curl without linking it */
          "libcurl.so.4",
     };
     for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++)
          if (!dlopen(pre[i], RTLD_NOW | RTLD_GLOBAL)) {
               say("error dlopen: %s", dlerror());
               return -1;
          }
     /* the vendor debug-log library needs its one-time init before any
      * library logs (libhmplayer does, in its static constructors) */
     void *dbg = dlopen("libcsp-debug.so", RTLD_NOW | RTLD_GLOBAL);
     void (*dbg_init)(void) = dbg ? (void (*)(void))dlsym(dbg, "cspDebugInit") : NULL;
     if (dbg_init) dbg_init();
     else say("note no cspDebugInit");
     H.lib = dlopen("/usr/local/lib/libhmplayer.so", RTLD_NOW | RTLD_GLOBAL);
     if (!H.lib) { say("error dlopen: %s", dlerror()); return -1; }
     /* HUI_StreamInjectOpen opens an audio-output handle for every stream
      * (audio and video) and fails if this process never initialised the
      * MI audio-output module; nothing on the HMP path does (tvmain does it
      * in its own process). Zeroed 1-byte params, as tvmain passes; 0 or 2
      * (already initialised) is success. --no-aout-init skips it. */
     if (aout_init) {
          void *mi = dlopen("libmi.so", RTLD_NOW | RTLD_GLOBAL);
          /* every MI module's Init fails ("_mi_hm_Open: Not inited", 4)
           * until the process has attached to MI: MI_SYS_Init(NULL) waits
           * for the system (mi_daemon_sys brings it up) and sets up this
           * process's handle manager. It is what libHui's
           * Mif_Board_Start_Board does; 0 or 2 is success. */
          int (*sys)(void *) = mi ? (int (*)(void *))dlsym(mi, "MI_SYS_Init") : NULL;
          if (sys) say("note MI_SYS_Init -> %d", sys(NULL));
          else say("note no MI_SYS_Init");
          int (*aout)(void *) = mi ? (int (*)(void *))dlsym(mi, "MI_AOUT_Init") : NULL;
          if (aout) {
               uint8_t a = 0;
               say("note MI_AOUT_Init -> %d", aout(&a));
          } else say("note no MI_AOUT_Init");
     }
#define SYM(f) if (!(*(void **)&H.f = dlsym(H.lib, "HMP" #f))) { say("error no HMP%s", #f); return -1; }
     SYM(Init); SYM(Create); SYM(RegisterCallback); SYM(UnRegisterCallback); SYM(SetURI);
     SYM(Prepare); SYM(Start); SYM(Pause); SYM(Resume); SYM(Seek); SYM(Stop); SYM(Destroy);
     SYM(GetPts); SYM(SetDisplayRect); SYM(SetComponentMuteState); SYM(SetSubtitleShowState);
#undef SYM
     return 0;
}

/* HMPStart blocks until the stream runs (it waits for prepare, codec
 * discovery, and opens the decoders): its own thread */
/* The stock browser's media backend (libhisense-uva-backend,
 * HiBackendStreamingBackendImpl::proccessPlay -> UVABackendTvAccess::
 * pauseDtvAVSync()) asks tvmain for the A/V resource right before HMPStart:
 * kilby.sl2.action.source.switch ["ss_access_<n>", "2" (REQUEST_RESOURCE),
 * "hbbtv", "3"], all strings, blocking until tvmain answers (up to 5 s).
 * MediaBackend::play() first sets the HbbTV stream-media status to 1; the
 * backend sets it back to 0 when the source is released. It never sends a
 * release (reason 3). Both are opt-in (--av-request, --media-status) until
 * a run on the screen shows what they do. */
#define KEY_STREAM_MEDIA_STATUS "kilby.sl2.i32.videoplayer.enum.hbbtv.stream.media.status"

static int model(void)
{
     if (model_ok < 0) model_ok = biz_open() == 0;
     return model_ok;
}

static void request_av(void)
{
     if (media_status && model())
          say("note media.status := 1 -> %d", biz_set_int(KEY_STREAM_MEDIA_STATUS, 1));
     if (av_request && model()) {
          const char *a[4] = { "ss_access_1", "2", "hbbtv", "3" };
          say("note source.switch request -> %d", biz_action(BIZ_ACTION_SWITCH, 4, a));
     }
}

/* HMPSetDisplayRect takes panel pixels (3840x2160 here: a 1920x1080 rect
 * filled the top-left quarter); our rects are in UI pixels, 1920x1080 */
static int panel_w = 3840, panel_h = 2160;

static void read_panel(void)
{
     static char s[BIZ_STR_MAX];
     int w, h;
     if (model() && biz_get_str("kilby.sl2.str.video.panel.resolution.info", s) == 1 &&
         sscanf(s, "%d*%d", &w, &h) == 2 && w >= 1280 && h >= 720) {
          panel_w = w; panel_h = h;
     }
     say("note panel %dx%d", panel_w, panel_h);
}

static struct hmp_rect to_panel(struct hmp_rect r)
{
     r.v[0] = r.v[0] * panel_w / 1920; r.v[2] = r.v[2] * panel_w / 1920;
     r.v[1] = r.v[1] * panel_h / 1080; r.v[3] = r.v[3] * panel_h / 1080;
     return r;
}

/* The main video/audio path belongs to tvmain and follows the "source".
 * The stock UI's media pages (DLNA, PVR media, sound test) take it with
 * kilby.sl2.action.source.switch [ts, 9 (SWITCH_SOURCE), "DMP"]: systemserver's
 * APM starts its virtual DmpProxy (no process forked), which has tvmain
 * build the app path external://plain/dmp (static analysis,
 * research/jellyfin/apm-path-re.md). Back: the same action with the input
 * that was showing, as vibe-ui switches inputs. --dmp-path. */
static void switch_source(const char *id)
{
     char ts[32];
     snprintf(ts, sizeof ts, "%lld", (long long)now_ms());
     const char *a[3] = { ts, "9", id };
     say("note source.switch %s -> %d", id, biz_action(BIZ_ACTION_SWITCH, 3, a));
}

static void take_path(void)
{
     static char cur[BIZ_STR_MAX];
     if (!dmp_path || !model()) return;
     if (biz_get_str(BIZ_KEY_INPUT_CURRENT, cur) == 1)
          snprintf(prev_input, sizeof prev_input, "%s", cur);
     say("note input before: %s", prev_input);
     switch_source("DMP");
     for (int i = 0; i < 25; i++) {          /* up to 5 s: log what it becomes */
          usleep(200000);
          if (biz_get_str(BIZ_KEY_INPUT_CURRENT, cur) == 1 && strcmp(cur, prev_input)) break;
     }
     say("note input now: %s", cur);
}

static void give_back_path(void)
{
     if (!dmp_path || !model() || !prev_input[0] || !strcmp(prev_input, "DMP")) return;
     switch_source(prev_input);
     prev_input[0] = 0;                     /* once */
}

static void *start_thread(void *arg)
{
     request_av();
     say("trace start");
     int r = H.Start(player);
     if (r != 0) { say("error HMPStart %d", r); return NULL; }
     pthread_mutex_lock(&st_lock);
     started = playing = 1;
     pthread_mutex_unlock(&st_lock);
     clock_set(0, 1);
     /* the inject handle exists only once HMPStart has opened the decoders */
     struct hmp_rect pr = to_panel(rect);
     r = H.SetDisplayRect(player, pr, pr, 0);
     say("rect %d -> %d", pr.v[2], r);
     if (r == 0) H.SetComponentMuteState(player, 2, 0);   /* unmute video (UNSURE: 2) */
     if (start_ms && start_seek) {
          r = H.Seek(player, start_ms);
          say("seek %llu -> %d", (unsigned long long)start_ms, r);
          if (r == 0) clock_set((double)start_ms, 1);
     }
     say("state playing");
     return NULL;
}

static int open_url(void)
{
     struct hmp_create p = { 0, app_type, data_type, kind };
     int r;
     /* HMPInit only logs (the real init is lazy, in HMPCreate), and it
      * crashed here outside the browser: skipped unless --init */
     if (do_init) {
          say("trace init");
          if ((r = H.Init()) != 0) say("note HMPInit %d", r);
     }
     take_path();
     say("trace create");
     if ((r = H.Create(&p, &player)) != 0 || !player) { say("error HMPCreate %d", r); return -1; }
     say("trace callback");
     H.RegisterCallback(player, on_event);
     struct hmp_uri u;
     memset(&u, 0, sizeof u);
     u.w[uri_word] = (uintptr_t)url;
     if (mime && mime_word >= 0 && mime_word < 12) u.w[mime_word] = (uintptr_t)mime;
     say("trace seturi");
     if ((r = H.SetURI(player, u)) != 0) { say("error HMPSetURI %d", r); return -1; }
     say("trace prepare");
     if ((r = H.Prepare(player)) != 0) { say("error HMPPrepare %d", r); return -1; }
     say("state prepared");
     H.SetSubtitleShowState(player, 0);          /* we draw subtitles */
     pthread_t t;
     pthread_create(&t, NULL, start_thread, NULL);
     pthread_detach(t);
     return 0;
}

static void stop_player(void)
{
     if (!player) { give_back_path(); return; }
     H.Stop(player);
     H.UnRegisterCallback(player);
     H.Destroy(player);
     player = NULL;
     if (media_status && model())
          say("note media.status := 0 -> %d", biz_set_int(KEY_STREAM_MEDIA_STATUS, 0));
     give_back_path();
     say("state stopped");
}

static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }

static void *ticker(void *arg)
{
     while (!quit) {
          sleep(1);
          if (!player || !started || paused) continue;
          struct hmp_pts pt;
          memset(&pt, 0, sizeof pt);
          int r = H.GetPts(player, &pt);
          say("pos %.0f %llu %lld %d%s", position(), (unsigned long long)pt.pts, (long long)pt.b,
              pt.c, r ? " (GetPts failed)" : "");
     }
     return NULL;
}

/* the vendor libraries find their own dependencies through the loader's
 * path, read once at exec: set it up and exec ourselves again */
static void ensure_env(char **argv)
{
     if (getenv("VIBE_PLAY_ENV")) return;
     setenv("VIBE_PLAY_ENV", "1", 1);
     setenv("LD_LIBRARY_PATH",
            "/vendor/lib/utopia/mi_ipc_client:/vendor/lib/utopia:/vendor/lib/appipc:/basic/lib:"
            "/vendor/lib:/vendor/lib/WVCENC:/vendor/lib/playready:/vendor/lib/directfb:/opt/mmlib:"
            "/usr/local/lib:/lib:/opt/libs", 1);
     setenv("GST_PLUGIN_PATH_1_0", "/opt/mmlib/", 1);
     setenv("GST_PLUGIN_PATH", "/opt/mmlib/gstreamer-1.0", 1);
     setenv("GST_PLUGIN_SCANNER", "/opt/mmlib/bin/gst-plugin-scanner", 1);
     /* private registry and cache: never the browser's, never root-owned
      * files in a place the UI user needs */
     setenv("GST_REGISTRY", "/tmp/vibe-play-gstreg2.bin", 1);
     /* scan plugins in this process, where libcurl is loaded: the separate
      * scanner could not load libgsthsplayer and the registry kept that */
     setenv("GST_REGISTRY_FORK", "no", 1);
     setenv("XDG_CACHE_HOME", "/tmp/vibe-play-cache", 1);
     execv("/proc/self/exe", argv);
     perror("vibe-play: re-exec");      /* go on without */
}

int main(int argc, char **argv)
{
     const char *user = NULL;
     int probe = 0;
     ensure_env(argv);
     for (int i = 1; i < argc; i++) {
          const char *a = argv[i];
          if (!strncmp(a, "--user=", 7)) user = a + 7;
          else if (!strncmp(a, "--uri-word=", 11)) uri_word = atoi(a + 11);
          else if (!strncmp(a, "--mime=", 7)) mime = a + 7;
          else if (!strncmp(a, "--mime-word=", 12)) mime_word = atoi(a + 12);
          else if (!strncmp(a, "--app-type=", 11)) app_type = atoi(a + 11);
          else if (!strncmp(a, "--kind=", 7)) kind = atoi(a + 7);
          else if (!strncmp(a, "--data-type=", 12)) data_type = atoi(a + 12);
          else if (!strcmp(a, "--no-start-seek")) start_seek = 0;
          else if (!strcmp(a, "--probe")) probe = 1;
          else if (!strcmp(a, "--init")) do_init = 1;
          else if (!strcmp(a, "--no-aout-init")) aout_init = 0;
          else if (!strcmp(a, "--av-request")) av_request = 1;
          else if (!strcmp(a, "--media-status")) media_status = 1;
          else if (!strcmp(a, "--dmp-path")) dmp_path = 1;
          else if (!strncmp(a, "--rect=", 7))
               sscanf(a + 7, "%d,%d,%d,%d,%d", &rect.v[0], &rect.v[1], &rect.v[2], &rect.v[3], &rect.v[4]);
          else { fprintf(stderr, "vibe-play: unknown option %s\n", a); return 2; }
     }
     if (uri_word < 0 || uri_word > 11) uri_word = 0;
     /* debug: the vendor libraries log to the fifo /var/log/console; give
      * this process alone a private mount table with a file over it */
     const char *vlog = getenv("VIBE_PLAY_VENDOR_LOG");
     if (vlog) {
          int fd = open(vlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fd >= 0) { if (user) { uid_t u = (uid_t)atoi(user); if (fchown(fd, u, u)) {} } close(fd); }
          if (unshare(CLONE_NEWNS) != 0 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
              mount(vlog, "/var/log/console", NULL, MS_BIND, NULL) != 0)
               fprintf(stderr, "vibe-play: vendor log: %s\n", strerror(errno));
     }
     if (user && vibe_drop_privileges(user) != 0) return 1;
     signal(SIGPIPE, SIG_IGN);
     signal(SIGTERM, on_term);
     signal(SIGINT, on_term);
     setvbuf(stdin, NULL, _IOLBF, 0);
     {
          int pfd = dup(1);
          if (pfd >= 0 && (proto = fdopen(pfd, "w")) != NULL) {
               fcntl(pfd, F_SETFD, FD_CLOEXEC);
               dup2(2, 1);
          }
     }
     if (probe) {
          int r = load();
          say(r == 0 ? "probe ok" : "probe failed");
          return r ? 1 : 0;
     }

     char line[4200];
     if (!fgets(line, sizeof line, stdin) || strncmp(line, "open ", 5) != 0) {
          say("error expected: open URL [START_MS]");
          return 1;
     }
     line[strcspn(line, "\r\n")] = 0;
     char *sp = strchr(line + 5, ' ');
     if (sp) { *sp = 0; start_ms = strtoull(sp + 1, NULL, 10); }
     snprintf(url, sizeof url, "%s", line + 5);
     say("state opening");
     read_panel();
     if (load() != 0 || open_url() != 0) { stop_player(); say("exit"); return 1; }
     pthread_t tk;
     pthread_create(&tk, NULL, ticker, NULL);
     pthread_detach(tk);

     while (!quit && fgets(line, sizeof line, stdin)) {
          line[strcspn(line, "\r\n")] = 0;
          int r;
          if (!strcmp(line, "pause")) {
               r = H.Pause(player);
               if (r == 0) { clock_set(position(), 0); say("state paused"); }
               else say("error pause %d", r);
          } else if (!strcmp(line, "resume")) {
               r = H.Resume(player);
               if (r == 0) { clock_set(position(), 1); say("state playing"); }
               else say("error resume %d", r);
          } else if (!strncmp(line, "seek ", 5)) {
               uint64_t ms = strtoull(line + 5, NULL, 10);
               r = H.Seek(player, ms);
               if (r == 0) clock_set((double)ms, !paused);
               say("seek %llu -> %d", (unsigned long long)ms, r);
          } else if (!strncmp(line, "rect ", 5)) {
               struct hmp_rect rc;
               if (sscanf(line + 5, "%d %d %d %d %d", &rc.v[0], &rc.v[1], &rc.v[2], &rc.v[3], &rc.v[4]) == 5) {
                    rc = to_panel(rc);
                    r = H.SetDisplayRect(player, rc, rc, 0);
                    say("rect -> %d", r);
               }
          } else if (!strcmp(line, "pts")) {
               struct hmp_pts pt;
               memset(&pt, 0, sizeof pt);
               r = H.GetPts(player, &pt);
               say("pts %d %llu %lld %d", r, (unsigned long long)pt.pts, (long long)pt.b, pt.c);
          } else if (!strcmp(line, "stop")) {
               break;
          }
     }
     quit = 1;
     stop_player();
     say("exit");
     return 0;
}
