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
 * --probe: load the library and resolve the symbols, then exit (no player
 * is made, nothing shows)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

/* ---- the HMP ABI (UNSURE where marked) ---------------------------------- */

typedef void *hmp_t;

struct hmp_create {          /* HMPCreate's param: 4 ints */
     int backend_id;          /* the browser passes its own id; UNSURE */
     int app_type;            /* 3 in the browser */
     int data_type;           /* 3 in the browser */
     int kind;                /* 32 = URL player (16 = MSE) */
};
struct hmp_uri { uintptr_t w[12]; };   /* by value; the URL in w[4] (UNSURE) */
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

static void say(const char *fmt, ...)
{
     va_list ap;
     pthread_mutex_lock(&out_lock);
     va_start(ap, fmt);
     vfprintf(stdout, fmt, ap);
     va_end(ap);
     fputc('\n', stdout);
     fflush(stdout);
     pthread_mutex_unlock(&out_lock);
}

/* ---- state -------------------------------------------------------------- */

static hmp_t player;
static int uri_word = 4, mime_word = -1, app_type = 3, kind = 32, start_seek = 1;
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
}

static int load(void)
{
     H.lib = dlopen("/usr/local/lib/libhmplayer.so", RTLD_NOW | RTLD_GLOBAL);
     if (!H.lib) H.lib = dlopen("libhmplayer.so", RTLD_NOW | RTLD_GLOBAL);
     if (!H.lib) { say("error dlopen: %s", dlerror()); return -1; }
#define SYM(f) if (!(*(void **)&H.f = dlsym(H.lib, "HMP" #f))) { say("error no HMP%s", #f); return -1; }
     SYM(Init); SYM(Create); SYM(RegisterCallback); SYM(UnRegisterCallback); SYM(SetURI);
     SYM(Prepare); SYM(Start); SYM(Pause); SYM(Resume); SYM(Seek); SYM(Stop); SYM(Destroy);
     SYM(GetPts); SYM(SetDisplayRect); SYM(SetComponentMuteState); SYM(SetSubtitleShowState);
#undef SYM
     return 0;
}

/* HMPStart blocks until the stream runs (it waits for prepare, codec
 * discovery, and opens the decoders): its own thread */
static void *start_thread(void *arg)
{
     int r = H.Start(player);
     if (r != 0) { say("error HMPStart %d", r); return NULL; }
     pthread_mutex_lock(&st_lock);
     started = playing = 1;
     pthread_mutex_unlock(&st_lock);
     clock_set(0, 1);
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
     struct hmp_create p = { 0, app_type, 3, kind };
     int r;
     if ((r = H.Init()) != 0) say("note HMPInit %d", r);
     if ((r = H.Create(&p, &player)) != 0 || !player) { say("error HMPCreate %d", r); return -1; }
     H.RegisterCallback(player, on_event);
     struct hmp_uri u;
     memset(&u, 0, sizeof u);
     u.w[uri_word] = (uintptr_t)url;
     if (mime && mime_word >= 0 && mime_word < 12) u.w[mime_word] = (uintptr_t)mime;
     if ((r = H.SetURI(player, u)) != 0) { say("error HMPSetURI %d", r); return -1; }
     if ((r = H.Prepare(player)) != 0) { say("error HMPPrepare %d", r); return -1; }
     say("state prepared");
     H.SetSubtitleShowState(player, 0);          /* we draw subtitles */
     r = H.SetDisplayRect(player, rect, rect, 0);
     say("rect %d -> %d", rect.v[2], r);
     if (r == 0) H.SetComponentMuteState(player, 2, 0);   /* unmute video (UNSURE: 2) */
     pthread_t t;
     pthread_create(&t, NULL, start_thread, NULL);
     pthread_detach(t);
     return 0;
}

static void stop_player(void)
{
     if (!player) return;
     H.Stop(player);
     H.UnRegisterCallback(player);
     H.Destroy(player);
     player = NULL;
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
     setenv("GST_REGISTRY", "/tmp/vibe-play-gstreg.bin", 1);
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
          else if (!strcmp(a, "--no-start-seek")) start_seek = 0;
          else if (!strcmp(a, "--probe")) probe = 1;
          else if (!strncmp(a, "--rect=", 7))
               sscanf(a + 7, "%d,%d,%d,%d,%d", &rect.v[0], &rect.v[1], &rect.v[2], &rect.v[3], &rect.v[4]);
          else { fprintf(stderr, "vibe-play: unknown option %s\n", a); return 2; }
     }
     if (uri_word < 0 || uri_word > 11) uri_word = 4;
     if (user && vibe_drop_privileges(user) != 0) return 1;
     signal(SIGPIPE, SIG_IGN);
     signal(SIGTERM, on_term);
     signal(SIGINT, on_term);
     setvbuf(stdin, NULL, _IOLBF, 0);
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
