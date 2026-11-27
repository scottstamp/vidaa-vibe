/*
 * The YouTube panel (see ytui.h). Home is shelves of 16:9 cards under a tab
 * bar (Home, Search, History); a channel is a grid of cards.
 *
 * Unlike the Jellyfin panel there is no detail page (owner's choice): OK on
 * a card plays the video at once (from where it was left, if it was), and
 * the details live in the player's overlay, shown by any navigation key:
 * title, channel, views, age, SponsorBlock, the progress bar with the
 * segments on it, and a row of actions (Channel, Follow, From the start,
 * Description) reached with DOWN.
 *
 * Screens: HOME <-> CHANNEL; either -> PLAYER, which returns where it came
 * from. BACK on Home's tab bar (first tab) closes the panel.
 */
#define _GNU_SOURCE
#include "ytui.h"
#include "yt.h"
#include "biz.h"
#include "jfimg.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SWITCH_OFF "/var/local/vibe/yt.off"
#define PLAYER_BIN "/var/local/vibe/vibe-play"

/* palette: as vibe-ui.c / jfui.c */
#define K_TEXT      0xfff0f3fa
#define K_TEXT2     0xff98a1b4
#define K_TEXT3     0xff6b7488
#define K_CARD      0xeb1c2130
#define K_PANEL_EDGE 0x26ffffff
#define K_TRACK     0x38ffffff
#define K_RED       0xffff3040         /* progress */
#define K_SKIP      0xff00d26a         /* SponsorBlock's green: segments we skip */
#define K_SEG       0xb0ffd000         /* segments we only mark */

/* ------------------------------------------------------------ the worker */

enum job_kind { JB_HOME, JB_SEARCH, JB_HISTORY, JB_CHANNEL, JB_OPEN, JB_REPLAY, JB_PROGRESS, JB_FOLLOW,
                JB_ACCOUNT, JB_SIGNIN, JB_POLL, JB_SIGNOUT, JB_SETTINGS,
                JB_RSTART, JB_RSTATUS, JB_RSTATE, JB_REMOTE /* a phone's command (remote thread) */,
                JB_RELATED, JB_COMMENTS, JB_AUTOPLAY };

struct job {
     enum job_kind kind;
     int  gen, flag, n;
     int  count;                   /* JB_SEARCH: how many results */
     char id[32];
     char arg[200];
     double pos, dur;
};

struct result {
     enum job_kind kind;
     int  gen, rc, flag, n;
     char err[200];
     char id[32];
     struct yt_home home;
     struct yt_list list;
     struct yt_channel ch;
     struct yt_video vid;
     struct yt_play play;
     struct yt_account acct;
     char code[32], url[128];      /* JB_SIGNIN; JB_RSTART: code */
     struct yt_cmd cmd;            /* JB_REMOTE */
     struct yt_comment *comm;      /* JB_COMMENTS: malloc'd */
     int  ncomm;
     char rconn[120];              /* JB_RSTATUS: the phones connected */
};

#define QMAX 32
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;
static struct job      jobs[QMAX];
static int             jq_head, jq_n;
static struct result  *results[QMAX];
static int             rq_head, rq_n;
static int             worker_up;

static void post(struct job j)
{
     pthread_mutex_lock(&q_lock);
     if (jq_n < QMAX) {
          jobs[(jq_head + jq_n) % QMAX] = j;
          jq_n++;
          pthread_cond_signal(&q_cond);
     }
     pthread_mutex_unlock(&q_lock);
}

static void result_free(struct result *r)
{
     yt_home_free(&r->home);
     yt_list_free(&r->list);
     yt_list_free(&r->ch.l);
     yt_video_free(&r->vid);
     free(r->comm);
     free(r);
}

static void deliver(struct result *r)
{
     pthread_mutex_lock(&q_lock);
     if (rq_n < QMAX) {
          results[(rq_head + rq_n) % QMAX] = r;
          rq_n++;
          r = NULL;
     }
     pthread_mutex_unlock(&q_lock);
     if (r) result_free(r);
}

static void *worker(void *arg)
{
     (void)arg;
     for (;;) {
          pthread_mutex_lock(&q_lock);
          while (!jq_n) pthread_cond_wait(&q_cond, &q_lock);
          struct job j = jobs[jq_head];
          jq_head = (jq_head + 1) % QMAX;
          jq_n--;
          pthread_mutex_unlock(&q_lock);

          struct result *r = calloc(1, sizeof *r);
          if (!r) continue;
          r->kind = j.kind;
          r->gen = j.gen;
          r->flag = j.flag;
          r->n = j.n;
          snprintf(r->id, sizeof r->id, "%s", j.id);
          switch (j.kind) {
          case JB_HOME:     r->rc = yt_home(&r->home); break;
          case JB_SEARCH:   r->rc = yt_search(j.arg, j.count ? j.count : 30, &r->list); break;
          case JB_HISTORY:  r->rc = yt_history(60, &r->list); break;
          case JB_CHANNEL:  r->rc = yt_channel(j.id, 60, &r->ch); break;
          case JB_OPEN:     /* the details (segments, position), then the stream */
               r->rc = yt_video(j.id, &r->vid);
               if (r->rc == 0) r->rc = yt_play(j.id, yt_maxh(), &r->play);
               break;
          case JB_REPLAY:   r->rc = yt_play(j.id, j.n, &r->play); break;    /* another quality */
          case JB_PROGRESS: r->rc = yt_progress(j.id, j.pos, j.dur); break;
          case JB_FOLLOW:   r->rc = yt_follow(j.id, j.flag); break;
          case JB_ACCOUNT:                              /* and the settings */
               r->rc = yt_account(&r->acct);
               if (r->rc == 0) { yt_settings(-1, &r->n); yt_autoplay(-1, &r->flag); }
               break;
          case JB_SETTINGS: r->rc = yt_settings(j.flag, &r->n); break;
          case JB_RSTART:
               r->rc = yt_remote_start(r->code, sizeof r->code);
               if (r->rc == 0) yt_remote_status(&r->n, r->rconn, sizeof r->rconn);
               break;
          case JB_RSTATUS:  r->rc = yt_remote_status(&r->n, r->rconn, sizeof r->rconn); break;
          case JB_RSTATE:   r->rc = yt_remote_state(j.id, j.pos, j.dur, j.arg); break;
          case JB_REMOTE:   break;              /* made by the remote thread, never queued */
          case JB_RELATED:  r->rc = yt_related(j.id, &r->list); break;
          case JB_COMMENTS:
               r->comm = calloc(40, sizeof *r->comm);
               r->rc = r->comm ? yt_comments(j.id, r->comm, 40, &r->ncomm) : -1;
               break;
          case JB_AUTOPLAY: r->rc = yt_autoplay(j.flag, &r->flag); break;
          case JB_SIGNIN:   r->rc = yt_signin_start(r->code, sizeof r->code, r->url, sizeof r->url); break;
          case JB_POLL:
               r->rc = yt_signin_poll(&r->flag, r->acct.name, sizeof r->acct.name);
               if (r->rc == 0 && r->flag < 0) snprintf(r->err, sizeof r->err, "%s", yt_error());
               break;
          case JB_SIGNOUT:  r->rc = yt_signout(); break;
          }
          if (r->rc != 0) snprintf(r->err, sizeof r->err, "%s", yt_error());
          deliver(r);
     }
     return NULL;
}

/* ------------------------------------------------------------- UI state */

enum scr { Y_HOME, Y_CHANNEL, Y_PLAYER };
enum { TAB_HOME, TAB_SEARCH, TAB_HISTORY, TAB_ACCOUNT, NTABS };
enum { A_CHANNEL, A_FOLLOW, A_UNFOLLOW, A_RESTART, A_DESC, A_SPEED, A_QUALITY, A_COMMENTS };

struct shelf {
     const char *title;
     struct yt_item *items;
     int  n, grid;
     int  sel, scroll;
};

struct button { char title[48]; int action; };

static struct {
     int  on, inited;
     struct jfui_host h;
     enum scr scr;
     int  gen, busy;
     char busy_msg[64];
     char err[200];
     double err_until;

     struct yt_home home;
     int  home_loaded, home_stale;
     struct yt_list srch, hist;
     char query[64];
     double query_at;
     int  srch_gen, srch_done;

     int  tab, tab_focus, kb_focus, kb_r, kb_c;
     struct shelf sh[YT_MAX_SHELVES + 1];
     int  nsh, cur, vtop;
     int  tsel[NTABS][YT_MAX_SHELVES + 1], tscroll[NTABS][YT_MAX_SHELVES + 1], tcur[NTABS], tvtop[NTABS];

     struct yt_channel ch;
     int  have_ch, ch_sel, ch_vtop;
     enum scr ch_back;

     /* the account tab: what the helper says; a sign-in in progress */
     struct yt_account acct;
     int  acct_known;
     int  set_sel;                 /* the Settings tab's focused button: 0 account, 1 DeArrow */
     int  dearrow;                 /* the helper's DeArrow setting */
     int  srch_count, srch_more;   /* results asked for; a "more" request out */

     /* phone casting: the helper's state; a pairing code shown; a phone's
      * "play" waiting for the panel to open; where it asked to start */
     volatile int remote_enabled;
     char rcode[32], rconn[120];
     char pend_id[16];
     double pend_pos, remote_start;
     int  want_open;
     const char *rstate_last;

     /* the player's extras: what YouTube suggests next (the "Up next" row,
      * autoplay), the comments panel, the countdown at the end */
     struct yt_list rel;
     char rel_for[16];             /* the video rel belongs to */
     int  rel_sel, rel_scroll;
     struct yt_comment *comm;
     int  ncomm, comm_top, comm_on, comm_loading;
     int  autoplay;
     int  upnext;                  /* the end: counting down to rel.items[0] */
     double upnext_until;
     char code[32], code_url[128];
     double next_poll, confirm_until;

     /* playback: what plays and its details (the overlay's) */
     enum scr play_back;          /* where the player returns */
     struct yt_item playing;      /* from the card at first, then the helper's */
     struct yt_video det;
     int  have_det;
     struct button btn[8];
     int  nbtn, bsel;
     int  osd_focus;              /* 0: the bar (seek, pause); 1: the actions */
     int  desc_on;                /* the description over the picture */
     int  speed, speed_set;       /* percent (100 = normal), kept across videos; applied once playing */
     int  ch_head;                /* the channel's Follow button has the focus */
     pid_t pid;
     char prev_input[64];
     int  to_fd, from_fd;
     char rbuf[2048];
     int  rlen;
     int  seg_done[YT_MAX_SEGS];
     int  live;
     double start_ms;
     int  pstate;                 /* 0 loading/opening, 1 playing, 2 paused */
     double pos_ms, dur_ms, pos_at, pos_moved;
     double osd_until, next_redraw, next_report;
     double seek_target_ms, seek_at;
     int  seek_pending;
     char toast[96];
     double toast_until;
     int  dirty;
} U;

static void set_err(const char *msg, double now)
{
     snprintf(U.err, sizeof U.err, "%s", msg);
     U.err_until = now + 6;
     U.dirty = 1;
     fprintf(stderr, "youtube: %s\n", msg);
}

static void busy(const char *msg)
{
     U.busy = 1;
     snprintf(U.busy_msg, sizeof U.busy_msg, "%s", msg);
     U.dirty = 1;
}

static void post_browse(struct job j, const char *msg)
{
     j.gen = ++U.gen;
     post(j);
     busy(msg);
}

/* phone casting: waits on the helper for the phone's commands (its own
 * thread, so the 25 s long-poll never holds up the worker); a command is
 * delivered as a JB_REMOTE result, applied by ytui_tick */
static void *remote_thread(void *arg)
{
     (void)arg;
     for (;;) {
          if (!U.remote_enabled) { sleep(3); continue; }
          struct result *r = calloc(1, sizeof *r);
          if (!r) { sleep(5); continue; }
          if (yt_remote_next(25, &r->cmd) != 0) { free(r); sleep(5); continue; }
          if (!strcmp(r->cmd.cmd, "none")) { free(r); continue; }
          r->kind = JB_REMOTE;
          deliver(r);
     }
     return NULL;
}

static int start_thread(void *(*fn)(void *))
{
     pthread_t t;
     pthread_attr_t at;
     pthread_attr_init(&at);
     pthread_attr_setstacksize(&at, 256 * 1024);
     int ok = pthread_create(&t, &at, fn, NULL) == 0;
     if (ok) pthread_detach(t);
     pthread_attr_destroy(&at);
     return ok;
}

/* the worker and the remote thread, once; what the helper's settings are */
static void ensure_started(void)
{
     if (U.inited) return;
     U.inited = 1;
     U.to_fd = U.from_fd = -1;
     U.tab_focus = 1;
     U.speed = 100;
     U.remote_start = -1;
     yt_config_load();
     worker_up = start_thread(worker);
     start_thread(remote_thread);
     if (worker_up) {
          struct job a = { .kind = JB_ACCOUNT }, s = { .kind = JB_RSTATUS };
          post(a);
          post(s);
     }
}

void ytui_start(void)
{
     if (ytui_available()) ensure_started();
}

int ytui_wants_open(void)
{
     int w = U.want_open;
     U.want_open = 0;
     return w;
}

int ytui_available(void) { return access(SWITCH_OFF, F_OK) != 0; }
int ytui_playing(void) { return U.on && U.scr == Y_PLAYER; }

/* ---------------------------------------------------------- formatting */

static void fmt_clock(double s, char *out, size_t n)
{
     long t = (long)s;
     if (t < 0) t = 0;
     if (t >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
     else snprintf(out, n, "%ld:%02ld", t / 60, t % 60);
}

static void fmt_views(long long v, char *out, size_t n)
{
     if (v >= 1000000000LL) snprintf(out, n, "%.1fB views", v / 1e9);
     else if (v >= 1000000) snprintf(out, n, "%.1fM views", v / 1e6);
     else if (v >= 1000) snprintf(out, n, "%.0fK views", v / 1e3);
     else if (v > 0) snprintf(out, n, "%lld views", v);
     else out[0] = 0;
}

/* days since 1970-01-01 of a civil date (H. Hinnant's algorithm): no
 * mktime/difftime (they gave nonsense here: "2147483647 years ago") */
static long days_civil(int y, int m, int d)
{
     y -= m <= 2;
     long era = (y >= 0 ? y : y - 399) / 400;
     long yoe = y - era * 400;
     long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
     long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
     return era * 146097 + doe - 719468;
}

/* "YYYY-MM-DD" -> "3 years ago" */
static void fmt_ago(const char *d, char *out, size_t n)
{
     out[0] = 0;
     int y, m, dd;
     if (strlen(d) != 10 || sscanf(d, "%d-%d-%d", &y, &m, &dd) != 3 || m < 1 || m > 12) return;
     long days = (long)(time(NULL) / 86400) - days_civil(y, m, dd);
     if (days < 0) return;
     if (days < 1) snprintf(out, n, "today");
     else if (days < 2) snprintf(out, n, "yesterday");
     else if (days < 14) snprintf(out, n, "%ld days ago", days);
     else if (days < 60) snprintf(out, n, "%ld weeks ago", days / 7);
     else if (days < 730) snprintf(out, n, "%ld months ago", days * 10 / 304);
     else snprintf(out, n, "%ld years ago", days * 100 / 36525);
}

#define DOT "  \xc2\xb7  "

static void join(char *out, size_t n, const char *part)
{
     if (!part[0]) return;
     size_t l = strlen(out);
     snprintf(out + l, n - l, "%s%s", l ? DOT : "", part);
}

static void card_sub(const struct yt_item *it, char *out, size_t n)
{
     char v[32], a[32];
     snprintf(out, n, "%s", it->channel);
     if (it->info[0]) { join(out, n, it->info); return; }    /* YouTube's own wording */
     fmt_views(it->views, v, sizeof v);
     fmt_ago(it->published, a, sizeof a);
     join(out, n, v);
     join(out, n, a);
}

/* ------------------------------------------------------------ card screens */

#define C_X0     96
#define C_TOP    110
#define C_BOT    (1080 - 44)
#define L_W      352
#define L_H      198
#define C_GAP    32
#define C_RAD    14
#define SH_TITLE 64
#define C_TEXT   78
#define SH_GAP   24
#define SR_X     640

static int cards_x0(void) { return U.scr == Y_HOME && U.tab == TAB_SEARCH ? SR_X : C_X0; }
static int per_line(void) { return (1920 - cards_x0() - C_X0 + C_GAP) / (L_W + C_GAP); }
static int shelf_h(void) { return SH_TITLE + L_H + C_TEXT + SH_GAP; }
static int grid_line_h(void) { return L_H + C_TEXT + 16; }
static int grid_top(void) { return U.scr == Y_HOME && U.tab == TAB_SEARCH ? C_TOP + 96 : C_TOP; }
static int grid_lines_shown(void)
{
     int n = (C_BOT - grid_top()) / grid_line_h();
     return n < 1 ? 1 : n;
}

static void cards_follow(void)
{
     if (!U.nsh) return;
     struct shelf *h = &U.sh[U.cur];
     if (h->grid) {
          int pl = per_line(), line = h->sel / pl, shown = grid_lines_shown();
          if (line < U.vtop) U.vtop = line;
          if (line >= U.vtop + shown) U.vtop = line - shown + 1;
          return;
     }
     int full = per_line();
     if (h->sel < h->scroll) h->scroll = h->sel;
     if (h->sel >= h->scroll + full) h->scroll = h->sel - full + 1;
     if (U.cur < U.vtop) U.vtop = U.cur;
     while ((U.cur - U.vtop + 1) * shelf_h() - SH_GAP > C_BOT - C_TOP && U.vtop < U.cur) U.vtop++;
}

static void add_shelf(const char *title, struct yt_item *items, int n, int grid)
{
     if (U.nsh >= YT_MAX_SHELVES + 1 || n <= 0) return;
     U.sh[U.nsh++] = (struct shelf){ title, items, n, grid, 0, 0 };
}

static void save_cards(void)
{
     if (U.scr == Y_HOME) {
          for (int i = 0; i < U.nsh; i++) {
               U.tsel[U.tab][i] = U.sh[i].sel;
               U.tscroll[U.tab][i] = U.sh[i].scroll;
          }
          U.tcur[U.tab] = U.cur;
          U.tvtop[U.tab] = U.vtop;
     } else if (U.scr == Y_CHANNEL && U.nsh) {
          U.ch_sel = U.sh[0].sel;
          U.ch_vtop = U.vtop;
     }
}

static void build_cards(void)
{
     U.nsh = 0;
     if (U.scr == Y_HOME) {
          if (U.tab == TAB_HOME)
               for (int i = 0; i < U.home.n; i++)
                    add_shelf(U.home.sh[i].title, U.home.sh[i].l.items, U.home.sh[i].l.n, 0);
          else if (U.tab == TAB_SEARCH) add_shelf("Results", U.srch.items, U.srch.n, 1);
          else if (U.tab == TAB_HISTORY) add_shelf("Watched", U.hist.items, U.hist.n, 1);
          for (int i = 0; i < U.nsh; i++) {
               struct shelf *h = &U.sh[i];
               h->sel = U.tsel[U.tab][i] < h->n ? U.tsel[U.tab][i] : 0;
               h->scroll = U.tscroll[U.tab][i] <= h->sel ? U.tscroll[U.tab][i] : h->sel;
          }
          U.cur = U.tcur[U.tab] < U.nsh ? U.tcur[U.tab] : 0;
          U.vtop = U.tvtop[U.tab] <= U.cur ? U.tvtop[U.tab] : U.cur;
     } else if (U.scr == Y_CHANNEL && U.have_ch) {
          add_shelf(U.ch.title, U.ch.l.items, U.ch.l.n, 1);
          if (U.nsh) U.sh[0].sel = U.ch_sel < U.sh[0].n ? U.ch_sel : 0;
          U.cur = 0;
          U.vtop = U.ch_vtop;
     }
     cards_follow();
}

static void load_home(void)
{
     struct job j = { .kind = JB_HOME };
     post_browse(j, "Loading\xe2\x80\xa6");
     U.home_stale = 0;
}

static void go(enum scr s)
{
     U.scr = s;
     if (s != Y_PLAYER) build_cards();
     /* back on Home after a follow or a watch: its shelves changed */
     if (s == Y_HOME && U.tab == TAB_HOME && U.home_stale) load_home();
     U.dirty = 1;
}

static void open_channel(const char *cid)
{
     struct job j = { .kind = JB_CHANNEL };
     snprintf(j.id, sizeof j.id, "%s", cid);
     post_browse(j, "Loading the channel\xe2\x80\xa6");
}

/* ---- the overlay's actions */

static void add_btn(const char *t, int a)
{
     if (U.nbtn < 8) {
          snprintf(U.btn[U.nbtn].title, sizeof U.btn[U.nbtn].title, "%s", t);
          U.btn[U.nbtn++].action = a;
     }
}

static void build_buttons(void)
{
     U.nbtn = 0;
     const struct yt_item *it = &U.playing;
     if (it->channel_id[0]) {
          add_btn("Channel", A_CHANNEL);
          if (U.have_det) add_btn(U.det.followed ? "Unfollow" : "Follow", U.det.followed ? A_UNFOLLOW : A_FOLLOW);
     }
     if (!U.live) add_btn("From the start", A_RESTART);
     if (!U.live) {
          char t[32];
          if (U.speed % 100 == 0) snprintf(t, sizeof t, "Speed %d\xc3\x97", U.speed / 100);
          else if (U.speed % 25 == 0 && U.speed % 50) snprintf(t, sizeof t, "Speed %.2f\xc3\x97", U.speed / 100.0);
          else snprintf(t, sizeof t, "Speed %.1f\xc3\x97", U.speed / 100.0);
          add_btn(t, A_SPEED);
     }
     {
          char t[32];
          snprintf(t, sizeof t, "Up to %dp", yt_maxh());
          add_btn(t, A_QUALITY);
     }
     if (U.have_det && U.det.description) add_btn(U.desc_on ? "Hide description" : "Description", A_DESC);
     if (!U.live) add_btn(U.comm_on ? "Hide comments" : "Comments", A_COMMENTS);
     if (U.bsel >= U.nbtn) U.bsel = U.nbtn ? U.nbtn - 1 : 0;
}

/* ------------------------------------------------------------- playback */

static void player_send(const char *fmt, ...)
{
     if (U.to_fd < 0) return;
     char buf[2048];
     va_list ap;
     va_start(ap, fmt);
     int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
     va_end(ap);
     if (n < 0) return;
     if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
     buf[n++] = '\n';
     if (write(U.to_fd, buf, (size_t)n) != n)
          fprintf(stderr, "youtube: player write: %s\n", strerror(errno));
}

static int spawn_player(const char *url, const char *mime, double start_ms)
{
     char mime_arg[64];
     snprintf(mime_arg, sizeof mime_arg, "--mime=%s", mime);
     int in[2], out[2];
     if (pipe2(in, O_CLOEXEC) != 0) return -1;
     if (pipe2(out, O_CLOEXEC) != 0) { close(in[0]); close(in[1]); return -1; }
     if (biz_current_input(U.prev_input, sizeof U.prev_input) != 0) U.prev_input[0] = 0;
     pid_t pid = fork();
     if (pid < 0) {
          close(in[0]); close(in[1]); close(out[0]); close(out[1]);
          return -1;
     }
     if (pid == 0) {
          dup2(in[0], 0);
          dup2(out[1], 1);
          int lf = open("/tmp/vibe-play.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
          if (lf >= 0) dup2(lf, 2);
          for (int fd = 3; fd < 1024; fd++) close(fd);
          char *const argv[] = { (char *)"vibe-play", mime_arg, NULL };
          execv(PLAYER_BIN, argv);
          _exit(127);
     }
     close(in[0]);
     close(out[1]);
     U.pid = pid;
     U.to_fd = in[1];
     U.from_fd = out[0];
     fcntl(U.from_fd, F_SETFL, fcntl(U.from_fd, F_GETFL) | O_NONBLOCK);
     U.rlen = 0;
     player_send("open %s %.0f", url, start_ms);
     return 0;
}

/* as jfui.c: wait for the child; if it died, rebuild the picture path */
static void player_reap(void)
{
     if (U.to_fd >= 0) { close(U.to_fd); U.to_fd = -1; }
     if (U.from_fd >= 0) { close(U.from_fd); U.from_fd = -1; }
     if (U.pid <= 0) return;
     int st = 0, clean = 0;
     for (int i = 0; i < 60; i++) {
          if (waitpid(U.pid, &st, WNOHANG) == U.pid) {
               U.pid = 0;
               clean = WIFEXITED(st) && WEXITSTATUS(st) == 0;
               break;
          }
          usleep(100000);
     }
     if (U.pid > 0) {
          kill(U.pid, SIGKILL);
          waitpid(U.pid, NULL, 0);
          U.pid = 0;
     }
     if (!clean && U.prev_input[0] && strcmp(U.prev_input, "DMP")) {
          fprintf(stderr, "youtube: player died (status 0x%x), %s -> DMP -> %s\n",
                  st, U.prev_input, U.prev_input);
          biz_switch_input(U.prev_input);
          sleep(2);
          biz_switch_input("DMP");
          sleep(3);
          biz_switch_input(U.prev_input);
     }
}

static double cur_pos(double now)
{
     if (U.seek_pending) return U.seek_target_ms;
     return U.pos_ms + (U.pstate == 1 && U.pos_at > 0 ? (now - U.pos_at) * 1000.0 * U.speed / 100.0 : 0);
}

static void report(double now)
{
     if (U.live || !U.playing.id[0]) return;
     struct job j = { .kind = JB_PROGRESS, .pos = U.pos_ms / 1000.0, .dur = U.dur_ms / 1000.0 };
     snprintf(j.id, sizeof j.id, "%s", U.playing.id);
     post(j);
     U.next_report = now + 10;
}

/* a phone linked: tell it what plays (on every change, and with report) */
static void rstate(const char *state)
{
     if (!U.remote_enabled || !U.playing.id[0]) return;
     struct job j = { .kind = JB_RSTATE, .pos = U.pos_ms / 1000.0, .dur = U.dur_ms / 1000.0 };
     snprintf(j.id, sizeof j.id, "%s", U.playing.id);
     snprintf(j.arg, sizeof j.arg, "%s", state);
     post(j);
     U.rstate_last = state;
}

/* the position shown on the card we came from, as it is now */
static void update_cards_position(double pos_s)
{
     struct yt_list *lists[2 + YT_MAX_SHELVES];
     int nl = 0;
     lists[nl++] = &U.srch;
     lists[nl++] = &U.hist;
     for (int i = 0; i < U.home.n; i++) lists[nl++] = &U.home.sh[i].l;
     for (int k = 0; k < nl; k++)
          for (int i = 0; i < lists[k]->n; i++)
               if (!strcmp(lists[k]->items[i].id, U.playing.id)) lists[k]->items[i].position = pos_s;
     for (int i = 0; U.have_ch && i < U.ch.l.n; i++)
          if (!strcmp(U.ch.l.items[i].id, U.playing.id)) U.ch.l.items[i].position = pos_s;
}

static void stop_playback(double now, int ended)
{
     if (U.scr != Y_PLAYER && !U.pid) return;
     U.gen++;                       /* a stream still being fetched is not wanted */
     U.busy = 0;
     if (ended && U.dur_ms > 0) U.pos_ms = U.dur_ms;
     if (U.pstate) {
          report(now);
          update_cards_position(ended ? 0 : U.pos_ms / 1000.0);
     }
     /* the end, autoplay on, suggestions known: count down to the next one,
      * staying on the DMP path ("stop keep") */
     int next = ended && U.autoplay && U.rel.n > 0 && !strcmp(U.rel_for, U.playing.id);
     if (!next) rstate("stopped");
     player_send(next ? "stop keep" : "stop");
     player_reap();
     U.home_stale = 1;             /* Continue watching changed */
     U.comm_on = 0;
     if (next) {
          U.upnext = 1;
          U.upnext_until = now + 8;
          U.pstate = 0;
          U.osd_until = 0;
          U.dirty = 1;
          return;
     }
     go(U.play_back);
}

/* the countdown cancelled: back to the input the player had replaced */
static void upnext_cancel(void)
{
     U.upnext = 0;
     rstate("stopped");
     if (U.prev_input[0] && strcmp(U.prev_input, "DMP")) biz_switch_input(U.prev_input);
     go(U.play_back);
}

/* OK on a card: the player screen at once (loading), the helper's details
 * and stream next (JB_OPEN). Also for a phone's "play" while something
 * plays: that one stops first (staying on the DMP path) */
static void play_item(const struct yt_item *it)
{
     if (U.pid) {
          if (U.pstate) report(0);
          player_send("stop keep");
          player_reap();
     }
     if (U.scr != Y_PLAYER) {            /* from a list: where to come back to */
          save_cards();
          U.play_back = U.scr;
     }
     U.upnext = 0;
     U.comm_on = 0;
     free(U.comm);
     U.comm = NULL;
     U.ncomm = 0;
     U.playing = *it;
     yt_video_free(&U.det);
     memset(&U.det, 0, sizeof U.det);
     U.have_det = 0;
     U.live = it->live;
     U.pstate = 0;
     U.pos_ms = U.start_ms = 0;
     U.dur_ms = it->duration * 1000.0;
     U.osd_until = 0;
     U.osd_focus = 0;
     U.desc_on = 0;
     U.bsel = 0;
     U.toast_until = 0;
     memset(U.seg_done, 0, sizeof U.seg_done);
     U.speed_set = 0;
     build_buttons();
     struct job j = { .kind = JB_OPEN };
     snprintf(j.id, sizeof j.id, "%s", it->id);
     post_browse(j, "");
     U.scr = Y_PLAYER;
     U.dirty = 1;
}

static void on_player_line(char *l, double now)
{
     if (!strncmp(l, "state ", 6)) {
          if (!strcmp(l + 6, "playing")) {
               U.pstate = 1;
               if (!U.speed_set && U.speed != 100) player_send("speed %d", U.speed);
               U.speed_set = 1;
               rstate("playing");
          }
          else if (!strcmp(l + 6, "paused")) { U.pstate = 2; rstate("paused"); }
          U.dirty = 1;
     } else if (!strncmp(l, "pos ", 4)) {
          double p = atof(l + 4);
          if (!U.seek_pending) {
               if (p != U.pos_ms) U.pos_moved = now;
               U.pos_ms = p;
               U.pos_at = now;
          }
          if (U.osd_until > now) U.dirty = 1;
     } else if (!strncmp(l, "error ", 6)) {
          set_err(l + 6, now);
     } else if (!strcmp(l, "ended")) {
          stop_playback(now, 1);
     }
     fprintf(stderr, "vibe-play: %s\n", l);
}

static void read_player(double now)
{
     if (U.from_fd < 0) return;
     for (;;) {
          ssize_t n = read(U.from_fd, U.rbuf + U.rlen, sizeof U.rbuf - 1 - (size_t)U.rlen);
          if (n == 0) {
               if (U.scr == Y_PLAYER) {
                    if (U.pstate == 0) set_err("The player stopped before the video started", now);
                    stop_playback(now, 0);
               }
               return;
          }
          if (n < 0) return;
          U.rlen += (int)n;
          U.rbuf[U.rlen] = 0;
          char *nl;
          while ((nl = strchr(U.rbuf, '\n'))) {
               *nl = 0;
               on_player_line(U.rbuf, now);
               if (U.from_fd < 0) return;
               int rest = U.rlen - (int)(nl + 1 - U.rbuf);
               memmove(U.rbuf, nl + 1, (size_t)rest);
               U.rlen = rest;
               U.rbuf[U.rlen] = 0;
          }
          if (U.rlen >= (int)sizeof U.rbuf - 1) U.rlen = 0;
     }
}

/* SponsorBlock: inside a segment we skip -> to its end, once per entry */
static void sponsor_tick(double now)
{
     if (U.pstate != 1 || U.seek_pending || !U.have_det) return;
     double p = cur_pos(now) / 1000.0;
     for (int i = 0; i < U.det.nsegs; i++) {
          struct yt_seg *g = &U.det.segs[i];
          if (!g->skip) continue;
          if (p < g->start - 1.0) { U.seg_done[i] = 0; continue; }   /* sought back before it */
          if (U.seg_done[i] || p < g->start || p >= g->end - 0.5) continue;
          U.seg_done[i] = 1;
          double to = g->end;
          if (U.dur_ms > 0 && to * 1000.0 > U.dur_ms - 1500) to = U.dur_ms / 1000.0 - 1.5;
          player_send("seek %.0f", to * 1000.0);
          U.pos_ms = to * 1000.0;
          U.pos_at = now;
          snprintf(U.toast, sizeof U.toast, "Skipped: %s", yt_cat_name(g->category));
          U.toast_until = now + 3;
          U.dirty = 1;
          fprintf(stderr, "youtube: skipped %s %.1f-%.1f\n", g->category, g->start, g->end);
          return;
     }
}

/* ---------------------------------------------------------------- open */

void ytui_open(const struct jfui_host *h, double now)
{
     U.h = *h;
     U.on = 1;
     U.dirty = 1;
     ensure_started();
     if (!worker_up) { set_err("could not start the network thread", now); return; }
     U.scr = Y_HOME;
     U.tab = TAB_HOME;
     U.tab_focus = 1;
     build_cards();
     if (!U.home_loaded || U.home_stale) load_home();
     if (U.pend_id[0]) {                              /* a phone asked to play: now */
          struct yt_item it;
          memset(&it, 0, sizeof it);
          snprintf(it.id, sizeof it.id, "%s", U.pend_id);
          U.remote_start = U.pend_pos;
          U.pend_id[0] = 0;
          play_item(&it);
     }
}

void ytui_close(double now)
{
     if (U.scr == Y_PLAYER || U.pid) stop_playback(now, 0);
     U.on = 0;
     U.gen++;
     U.busy = 0;
}

/* ---------------------------------------------------------------- tick */

static void apply(struct result *r, double now)
{
     int stale = r->gen && r->gen != U.gen;
     if (r->kind == JB_PROGRESS) return;
     if (r->kind == JB_SEARCH) {
          if (r->n != U.srch_gen) return;               /* an older query */
          int more = U.srch_more;
          U.srch_more = 0;
          if (r->rc != 0) { set_err(r->err, now); return; }
          int keep = more && U.scr == Y_HOME && U.tab == TAB_SEARCH && U.nsh ? U.sh[0].sel : -1;
          int vtop = U.vtop;
          yt_list_free(&U.srch);
          U.srch = r->list;
          r->list.items = NULL; r->list.n = 0;
          U.srch_done = 1;
          memset(U.tsel[TAB_SEARCH], 0, sizeof U.tsel[TAB_SEARCH]);
          memset(U.tscroll[TAB_SEARCH], 0, sizeof U.tscroll[TAB_SEARCH]);
          U.tcur[TAB_SEARCH] = U.tvtop[TAB_SEARCH] = 0;
          if (keep >= 0) {                              /* more of the same: stay put */
               U.tsel[TAB_SEARCH][0] = keep < U.srch.n ? keep : 0;
               U.tvtop[TAB_SEARCH] = vtop;
          }
          if (U.scr == Y_HOME && U.tab == TAB_SEARCH) build_cards();
          U.dirty = 1;
          return;
     }
     if (r->kind == JB_FOLLOW) {                       /* whatever screen we are on */
          if (r->rc != 0) { set_err(r->err, now); return; }
          if (U.have_det && !strcmp(U.det.it.channel_id, r->id)) { U.det.followed = r->flag; build_buttons(); }
          if (U.have_ch && !strcmp(U.ch.id, r->id)) U.ch.followed = r->flag;
          snprintf(U.toast, sizeof U.toast, r->flag ? "Following: new videos appear on Home" : "Unfollowed");
          U.toast_until = now + 3;
          U.home_stale = 1;
          U.dirty = 1;
          return;
     }
     switch (r->kind) {                                  /* the account tab (any screen) */
     case JB_ACCOUNT:
          if (r->rc == 0) { U.acct = r->acct; U.acct_known = 1; U.dearrow = r->n; U.autoplay = r->flag; }
          else set_err(r->err, now);
          U.dirty = 1;
          return;
     case JB_RSTART:
          U.busy = 0;
          if (r->rc != 0) { set_err(r->err, now); return; }
          snprintf(U.rcode, sizeof U.rcode, "%s", r->code);
          U.remote_enabled = r->n;
          snprintf(U.rconn, sizeof U.rconn, "%s", r->rconn);
          U.dirty = 1;
          return;
     case JB_RSTATUS:
          if (r->rc == 0) {
               U.remote_enabled = r->n;
               snprintf(U.rconn, sizeof U.rconn, "%s", r->rconn);
               U.dirty = 1;
          }
          return;
     case JB_RSTATE:
          return;
     case JB_REMOTE: {                                   /* the phone's command */
          const struct yt_cmd *m = &r->cmd;
          fprintf(stderr, "youtube: remote %s %s %.0f\n", m->cmd, m->id, m->pos);
          if (!strcmp(m->cmd, "play") && strlen(m->id) == 11) {
               U.rcode[0] = 0;                            /* linked: the code is done */
               if (U.on) {
                    struct yt_item it;
                    memset(&it, 0, sizeof it);
                    snprintf(it.id, sizeof it.id, "%s", m->id);
                    U.remote_start = m->pos > 0 ? m->pos : -1;
                    play_item(&it);
               } else {                                   /* vibe-ui opens the panel first */
                    snprintf(U.pend_id, sizeof U.pend_id, "%s", m->id);
                    U.pend_pos = m->pos > 0 ? m->pos : -1;
                    U.want_open = 1;
               }
          } else if (U.scr == Y_PLAYER && U.on) {
               if (!strcmp(m->cmd, "pause") && U.pstate == 1) player_send("pause");
               else if (!strcmp(m->cmd, "resume") && U.pstate == 2) player_send("resume");
               else if (!strcmp(m->cmd, "seek") && U.pstate && !U.live) {
                    U.seek_pending = 0;
                    player_send("seek %.0f", m->pos * 1000.0);
                    U.pos_ms = m->pos * 1000.0;
                    U.pos_at = now;
                    U.osd_until = now + 4;
                    rstate(U.pstate == 2 ? "paused" : "playing");
               } else if (!strcmp(m->cmd, "stop")) stop_playback(now, 0);
          }
          U.dirty = 1;
          return;
     }
     case JB_RELATED:
          if (r->rc != 0 || strcmp(r->id, U.playing.id)) return;   /* a hiccup or an old video */
          yt_list_free(&U.rel);
          U.rel = r->list;
          r->list.items = NULL; r->list.n = 0;
          snprintf(U.rel_for, sizeof U.rel_for, "%s", r->id);
          U.rel_sel = U.rel_scroll = 0;
          U.dirty = 1;
          return;
     case JB_COMMENTS:
          if (strcmp(r->id, U.playing.id)) return;
          U.comm_loading = 0;
          if (r->rc != 0) { set_err(r->err, now); U.comm_on = 0; return; }
          free(U.comm);
          U.comm = r->comm;
          U.ncomm = r->ncomm;
          r->comm = NULL;
          U.comm_top = 0;
          U.dirty = 1;
          return;
     case JB_AUTOPLAY:
          if (r->rc == 0) U.autoplay = r->flag;
          U.dirty = 1;
          return;
     case JB_SETTINGS:
          if (r->rc != 0) { set_err(r->err, now); return; }
          U.dearrow = r->n;
          U.home_stale = 1;                               /* titles and thumbnails change */
          snprintf(U.toast, sizeof U.toast, U.dearrow ? "DeArrow on: community titles and thumbnails"
                                                      : "DeArrow off: YouTube's own titles");
          U.toast_until = now + 3;
          U.dirty = 1;
          return;
     case JB_SIGNIN:
          U.busy = 0;
          if (r->rc != 0) { set_err(r->err, now); return; }
          snprintf(U.code, sizeof U.code, "%s", r->code);
          snprintf(U.code_url, sizeof U.code_url, "%s", r->url);
          U.next_poll = now + 5;
          U.dirty = 1;
          return;
     case JB_POLL:
          if (!U.code[0]) return;                         /* cancelled meanwhile */
          if (r->rc != 0) { set_err(r->err, now); return; }   /* the helper hiccuped: keep polling */
          if (r->flag > 0) {
               U.code[0] = 0;
               U.acct.signed_in = 1;
               snprintf(U.acct.name, sizeof U.acct.name, "%s", r->acct.name);
               snprintf(U.toast, sizeof U.toast, "Signed in%s%s", r->acct.name[0] ? " as " : "", r->acct.name);
               U.toast_until = now + 4;
               U.home_stale = 1;
          } else if (r->flag < 0) {
               U.code[0] = 0;
               set_err(r->err[0] ? r->err : "Sign-in did not complete", now);
          }
          U.dirty = 1;
          return;
     case JB_SIGNOUT:
          if (r->rc != 0) { set_err(r->err, now); return; }
          memset(&U.acct, 0, sizeof U.acct);
          U.acct_known = 1;
          U.home_stale = 1;
          snprintf(U.toast, sizeof U.toast, "Signed out");
          U.toast_until = now + 3;
          U.dirty = 1;
          return;
     default:
          break;
     }
     if (stale) return;
     U.busy = 0;
     U.dirty = 1;
     if (r->rc != 0) {
          set_err(r->err[0] ? r->err : "the helper did not answer", now);
          if (r->kind == JB_OPEN && U.scr == Y_PLAYER) go(U.play_back);
          return;
     }
     switch (r->kind) {
     case JB_HOME:
          yt_home_free(&U.home);
          U.home = r->home;
          memset(&r->home, 0, sizeof r->home);
          U.home_loaded = 1;
          if (U.scr == Y_HOME) build_cards();
          break;
     case JB_HISTORY:
          yt_list_free(&U.hist);
          U.hist = r->list;
          r->list.items = NULL; r->list.n = 0;
          if (U.scr == Y_HOME && U.tab == TAB_HISTORY) build_cards();
          break;
     case JB_CHANNEL:
          save_cards();
          if (U.scr != Y_CHANNEL) U.ch_back = U.scr == Y_PLAYER ? U.play_back : U.scr;
          yt_list_free(&U.ch.l);
          U.ch = r->ch;
          memset(&r->ch, 0, sizeof r->ch);
          U.have_ch = 1;
          U.ch_sel = U.ch_vtop = 0;
          U.ch_head = 0;
          go(Y_CHANNEL);
          break;
     case JB_OPEN: {
          if (U.scr != Y_PLAYER) break;
          yt_video_free(&U.det);
          U.det = r->vid;
          memset(&r->vid, 0, sizeof r->vid);
          U.have_det = 1;
          /* the helper's copy has more (views, date, position) */
          U.playing = U.det.it;
          U.live = r->play.live || U.det.it.live;
          U.dur_ms = r->play.duration > 0 ? r->play.duration * 1000.0 : U.det.it.duration * 1000.0;
          const struct yt_item *it = &U.det.it;
          int resume = !U.live && it->position > 30 && (it->duration <= 0 || it->position < it->duration - 30);
          U.start_ms = resume ? it->position * 1000.0 : 0;
          if (U.remote_start >= 0) {                  /* a phone said where */
               resume = 0;
               U.start_ms = U.live ? 0 : U.remote_start * 1000.0;
               U.remote_start = -1;
          }
          rstate("loading");
          if (strcmp(U.rel_for, U.playing.id)) {       /* the "Up next" row, autoplay */
               struct job rj = { .kind = JB_RELATED };
               snprintf(rj.id, sizeof rj.id, "%s", U.playing.id);
               post(rj);
          }
          U.pos_ms = U.start_ms;
          U.pos_at = 0;
          U.pos_moved = 0;
          U.seek_pending = 0;
          U.next_report = now + 10;
          build_buttons();
          if (resume) {
               char c[24];
               fmt_clock(it->position, c, sizeof c);
               snprintf(U.toast, sizeof U.toast, "Resuming from %s", c);
               U.toast_until = now + 4;
          }
          if (spawn_player(r->play.url, r->play.mime, U.start_ms) != 0) {
               set_err("could not start the player", now);
               go(U.play_back);
          }
          break;
     }
     case JB_REPLAY:
          if (U.scr != Y_PLAYER) break;
          U.pos_ms = U.start_ms;
          U.pos_at = 0;
          U.pos_moved = 0;
          U.seek_pending = 0;
          U.speed_set = 0;
          if (spawn_player(r->play.url, r->play.mime, U.start_ms) != 0) {
               set_err("could not start the player", now);
               go(U.play_back);
          }
          break;
     default:
          break;
     }
}

int ytui_tick(double now)
{
     if (!U.inited) return 0;
     for (;;) {
          struct result *r = NULL;
          pthread_mutex_lock(&q_lock);
          if (rq_n) { r = results[rq_head]; rq_head = (rq_head + 1) % QMAX; rq_n--; }
          pthread_mutex_unlock(&q_lock);
          if (!r) break;
          apply(r, now);
          result_free(r);
     }
     read_player(now);
     if (U.scr == Y_PLAYER && U.upnext) {             /* the countdown to the next video */
          if (!U.rel.n) upnext_cancel();
          else if (now >= U.upnext_until) {
               struct yt_item nx = U.rel.items[0];
               play_item(&nx);
          } else if (now >= U.next_redraw) {
               U.next_redraw = now + 1;
               U.dirty = 1;
          }
     }
     if (U.scr == Y_PLAYER && !U.upnext) {
          if (U.seek_pending && now >= U.seek_at) {
               U.seek_pending = 0;
               player_send("seek %.0f", U.seek_target_ms);
               U.pos_ms = U.seek_target_ms;
               U.pos_at = now;
               for (int i = 0; i < U.det.nsegs; i++)      /* skip again once landed before one */
                    if (U.seek_target_ms / 1000.0 < U.det.segs[i].start) U.seg_done[i] = 0;
          }
          sponsor_tick(now);
          if (U.pstate == 1 && now >= U.next_report) { report(now); rstate("playing"); }
          if (U.pstate == 1 && U.dur_ms > 0 && U.pos_ms > U.dur_ms + 3000) { stop_playback(now, 1); return U.on; }
          if (U.pstate == 1 && U.dur_ms > 0 && U.pos_ms > U.dur_ms - 15000 &&
              U.pos_moved > 0 && now - U.pos_moved > 6) { stop_playback(now, 1); return U.on; }
          if (U.osd_until && now >= U.osd_until) {
               U.osd_until = 0;
               U.osd_focus = 0;
               U.dirty = 1;
          } else if (U.osd_until > now && now >= U.next_redraw) {
               U.next_redraw = now + 1;
               U.dirty = 1;
          }
     }
     /* a sign-in code on show: ask the helper every 5 s whether it was entered */
     if (U.on && U.code[0] && now >= U.next_poll) {
          U.next_poll = now + 5;
          struct job j = { .kind = JB_POLL };
          post(j);
     }
     if (U.confirm_until && now >= U.confirm_until) { U.confirm_until = 0; U.dirty = 1; }
     if (U.toast_until && now >= U.toast_until) { U.toast_until = 0; U.dirty = 1; }
     if (U.err_until && now >= U.err_until) { U.err_until = 0; U.dirty = 1; }
     if (U.on && U.scr == Y_HOME && U.tab == TAB_SEARCH && U.query_at && now >= U.query_at) {
          U.query_at = 0;
          U.srch_gen++;
          if (strlen(U.query) >= 2) {
               U.srch_count = 30;
               U.srch_more = 0;
               struct job j = { .kind = JB_SEARCH, .n = U.srch_gen, .count = U.srch_count };
               snprintf(j.arg, sizeof j.arg, "%s", U.query);
               post(j);
               U.srch_done = 0;
          } else {
               yt_list_free(&U.srch);
               U.srch_done = 0;
               build_cards();
          }
          U.dirty = 1;
     }
     if (jfimg_changed() && (U.scr != Y_PLAYER || U.pstate == 0)) U.dirty = 1;
     int d = U.dirty;
     U.dirty = 0;
     return d && U.on;
}

/* ---------------------------------------------------------------- keys */

static void show_osd(double now)
{
     /* on the actions it stays up longer */
     U.osd_until = now + (U.osd_focus ? 20 : 8);
     U.dirty = 1;
}

static void seek_by(double d_ms, double now)
{
     if (U.live || U.pstate == 0) return;
     if (!U.seek_pending) U.seek_target_ms = U.pos_ms;
     U.seek_target_ms += d_ms;
     if (U.seek_target_ms < 0) U.seek_target_ms = 0;
     if (U.dur_ms > 0 && U.seek_target_ms > U.dur_ms - 2000) U.seek_target_ms = U.dur_ms - 2000;
     U.seek_pending = 1;
     U.seek_at = now + 0.7;
     show_osd(now);
}

static void osd_action(double now)
{
     if (U.bsel < 0 || U.bsel >= U.nbtn) return;
     switch (U.btn[U.bsel].action) {
     case A_CHANNEL: {
          char cid[32];
          snprintf(cid, sizeof cid, "%s", U.playing.channel_id);
          stop_playback(now, 0);
          open_channel(cid);
          break;
     }
     case A_FOLLOW: case A_UNFOLLOW: {
          struct job j = { .kind = JB_FOLLOW, .flag = U.btn[U.bsel].action == A_FOLLOW };
          snprintf(j.id, sizeof j.id, "%s", U.playing.channel_id);
          post(j);
          show_osd(now);
          break;
     }
     case A_RESTART:
          if (U.pstate == 0) break;
          U.seek_pending = 0;
          player_send("seek 0");
          U.pos_ms = 0;
          U.pos_at = now;
          memset(U.seg_done, 0, sizeof U.seg_done);
          U.osd_focus = 0;
          show_osd(now);
          break;
     case A_COMMENTS:
          U.comm_on = !U.comm_on;
          if (U.comm_on) {
               U.desc_on = 0;
               if (!U.ncomm && !U.comm_loading) {
                    U.comm_loading = 1;
                    struct job j = { .kind = JB_COMMENTS };
                    snprintf(j.id, sizeof j.id, "%s", U.playing.id);
                    post(j);
               }
          }
          build_buttons();
          show_osd(now);
          break;
     case A_DESC:
          U.desc_on = !U.desc_on;
          build_buttons();
          show_osd(now);
          break;
     case A_SPEED: {
          static const int steps[] = { 100, 125, 150, 175, 200, 75 };
          int n = sizeof steps / sizeof steps[0], i = 0;
          while (i < n && steps[i] != U.speed) i++;
          U.speed = steps[(i + 1) % n];
          if (U.pstate) player_send("speed %d", U.speed);
          U.pos_ms = cur_pos(now);
          U.pos_at = now;
          build_buttons();
          show_osd(now);
          break;
     }
     case A_QUALITY: {
          /* the cap cycles; the stream starts again where it is, in the new one */
          static const int caps[] = { 2160, 1440, 1080, 720, 480 };
          int n = sizeof caps / sizeof caps[0], i = 0;
          while (i < n && caps[i] != yt_maxh()) i++;
          int h = caps[(i + 1) % n];
          if (yt_set_maxh(h) != 0) fprintf(stderr, "youtube: could not save the quality\n");
          if (U.pstate == 0) { build_buttons(); show_osd(now); break; }
          U.start_ms = cur_pos(now);
          report(now);
          player_send("stop keep");
          player_reap();
          U.pstate = 0;
          snprintf(U.toast, sizeof U.toast, "Up to %dp", h);
          U.toast_until = now + 3;
          build_buttons();
          struct job j = { .kind = JB_REPLAY, .n = h };
          snprintf(j.id, sizeof j.id, "%s", U.playing.id);
          post_browse(j, "");
          break;
     }
     }
}

static void key_player(int k, int repeat, double now)
{
     int shown = U.osd_until > now;
     if (U.upnext) {                                   /* the countdown: OK now, BACK no */
          if (repeat) return;
          if (k == DIKS_OK || k == DIKS_RETURN || k == DIKS_SELECT || k == DIKS_PLAY) {
               struct yt_item nx = U.rel.items[0];
               play_item(&nx);
          } else if (k == DIKS_BACK || k == DIKS_EXIT || k == DIKS_STOP) {
               upnext_cancel();
          }
          return;
     }
     if (U.osd_focus == 2 && shown && U.rel.n) {       /* the "Up next" row */
          switch (k) {
          case DIKS_CURSOR_LEFT:  if (U.rel_sel > 0) U.rel_sel--; break;
          case DIKS_CURSOR_RIGHT: if (U.rel_sel < U.rel.n - 1) U.rel_sel++; break;
          case DIKS_CURSOR_UP:    U.osd_focus = 1; break;
          case DIKS_BACK: case DIKS_EXIT:
               if (repeat) return;
               U.osd_focus = 0;
               break;
          case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: {
               if (repeat) return;
               struct yt_item nx = U.rel.items[U.rel_sel];
               play_item(&nx);
               return;
          }
          default:
               break;
          }
          show_osd(now);
          return;
     }
     if (U.comm_on && !U.osd_focus && (k == DIKS_CURSOR_UP || k == DIKS_CURSOR_DOWN)) {
          if (k == DIKS_CURSOR_DOWN && U.comm_top < U.ncomm - 1) U.comm_top++;
          else if (k == DIKS_CURSOR_UP && U.comm_top > 0) U.comm_top--;
          else if (k == DIKS_CURSOR_DOWN && shown && U.nbtn) U.osd_focus = 1;   /* past the end: the actions */
          show_osd(now);
          return;
     }
     if (U.comm_on && (k == DIKS_BACK || k == DIKS_EXIT) && !U.osd_focus) {
          if (repeat) return;
          U.comm_on = 0;
          build_buttons();
          U.dirty = 1;
          return;
     }
     if (U.pstate == 0) {                              /* loading: only BACK */
          if ((k == DIKS_BACK || k == DIKS_EXIT || k == DIKS_STOP) && !repeat) stop_playback(now, 0);
          return;
     }
     if (U.osd_focus && shown) {                       /* on the actions */
          switch (k) {
          case DIKS_CURSOR_LEFT:  if (U.bsel > 0) U.bsel--; break;
          case DIKS_CURSOR_RIGHT: if (U.bsel < U.nbtn - 1) U.bsel++; break;
          case DIKS_CURSOR_DOWN:  if (U.rel.n && !strcmp(U.rel_for, U.playing.id)) U.osd_focus = 2; break;
          case DIKS_CURSOR_UP: case DIKS_BACK: case DIKS_EXIT:
               if (repeat) return;
               U.osd_focus = 0;
               break;
          case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
               if (repeat) return;
               osd_action(now);
               return;
          case DIKS_PLAYPAUSE: case DIKS_PAUSE: case DIKS_PLAY:
               break;                                  /* below */
          default:
               show_osd(now);
               return;
          }
          if (k != DIKS_PLAYPAUSE && k != DIKS_PAUSE && k != DIKS_PLAY) { show_osd(now); return; }
     }
     switch (k) {
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: case DIKS_PLAYPAUSE:
     case DIKS_PAUSE: case DIKS_PLAY:
          if (repeat) return;
          /* OK with the overlay hidden only shows it; with it up, pauses */
          if (!shown && (k == DIKS_OK || k == DIKS_RETURN || k == DIKS_SELECT)) { show_osd(now); return; }
          if (U.pstate == 1 && k != DIKS_PLAY) player_send("pause");
          else if (U.pstate == 2 && k != DIKS_PAUSE) player_send("resume");
          show_osd(now);
          break;
     case DIKS_CURSOR_LEFT: case DIKS_REWIND:
          if (!shown && k == DIKS_CURSOR_LEFT) { show_osd(now); return; }
          seek_by(repeat ? -30000 : -10000, now);
          break;
     case DIKS_CURSOR_RIGHT: case DIKS_FASTFORWARD:
          if (!shown && k == DIKS_CURSOR_RIGHT) { show_osd(now); return; }
          seek_by(repeat ? 30000 : 10000, now);
          break;
     case DIKS_CURSOR_DOWN:
          if (shown && U.nbtn) { U.osd_focus = 1; if (U.bsel >= U.nbtn) U.bsel = 0; }
          show_osd(now);
          break;
     case DIKS_CURSOR_UP: case DIKS_INFO:
          if (shown && !U.desc_on && k == DIKS_INFO) { U.osd_until = 0; U.dirty = 1; break; }
          show_osd(now);
          break;
     case DIKS_BACK: case DIKS_EXIT: case DIKS_STOP:
          if (repeat) return;
          if (U.desc_on && k != DIKS_STOP) { U.desc_on = 0; build_buttons(); U.dirty = 1; break; }
          if (shown && k != DIKS_STOP) { U.osd_until = 0; U.osd_focus = 0; U.dirty = 1; break; }
          stop_playback(now, 0);
          break;
     }
}

/* keyboard: as jfui.c */
static const char *const kb_rows[6] = { "abcdef", "ghijkl", "mnopqr", "stuvwx", "yz0123", "456789" };
static const char *const kb_acts[3] = { "Space", "Delete", "Clear" };
#define KB_X  C_X0
#define KB_KW 70
#define KB_KH 62
#define KB_G  8
#define KB_Y  (C_TOP + 96)

static void set_tab(int t)
{
     if (t == U.tab) return;
     save_cards();
     U.tab = t;
     if (t == TAB_HISTORY) {
          struct job j = { .kind = JB_HISTORY };
          post_browse(j, "Loading\xe2\x80\xa6");
     }
     if (t == TAB_HOME && U.home_stale) load_home();
     if (t == TAB_SEARCH) U.kb_focus = 1;
     if (t == TAB_ACCOUNT) {
          struct job j = { .kind = JB_ACCOUNT }, s = { .kind = JB_RSTATUS };
          post(j);
          post(s);
          U.confirm_until = 0;
     }
     build_cards();
     U.dirty = 1;
}

static int tab_key(int k, int repeat)
{
     switch (k) {
     case DIKS_CURSOR_LEFT:  if (U.tab > 0) set_tab(U.tab - 1); break;
     case DIKS_CURSOR_RIGHT: if (U.tab < NTABS - 1) set_tab(U.tab + 1); break;
     case DIKS_CURSOR_DOWN: case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (repeat) return 1;
          if (U.tab == TAB_SEARCH) { U.tab_focus = 0; U.kb_focus = 1; }
          else if (U.tab == TAB_ACCOUNT) U.tab_focus = 0;
          else if (U.nsh) U.tab_focus = 0;
          break;
     case DIKS_BACK: case DIKS_EXIT:
          if (repeat) return 1;
          if (U.tab != TAB_HOME) { set_tab(TAB_HOME); return 1; }
          return 0;                                  /* close the panel */
     default:
          return 1;
     }
     U.dirty = 1;
     return 1;
}

static void kb_type(int r, int c, double now)
{
     size_t l = strlen(U.query);
     if (r < 6) {
          if (l + 1 < sizeof U.query) { U.query[l] = kb_rows[r][c]; U.query[l + 1] = 0; }
     } else switch (c / 2) {
     case 0: if (l && l + 1 < sizeof U.query && U.query[l - 1] != ' ') { U.query[l] = ' '; U.query[l + 1] = 0; } break;
     case 1: if (l) U.query[l - 1] = 0; break;
     case 2: U.query[0] = 0; break;
     }
     U.query_at = now + 0.8;
}

static int kb_key(int k, int repeat, double now)
{
     switch (k) {
     case DIKS_CURSOR_UP:   if (U.kb_r == 0) U.tab_focus = 1; else U.kb_r--; break;
     case DIKS_CURSOR_DOWN: if (U.kb_r < 6) U.kb_r++; if (U.kb_r == 6) U.kb_c -= U.kb_c % 2; break;
     case DIKS_CURSOR_LEFT: if (U.kb_c > 0) U.kb_c -= U.kb_r == 6 ? 2 : 1; break;
     case DIKS_CURSOR_RIGHT:
          if (U.kb_c < (U.kb_r == 6 ? 4 : 5)) U.kb_c += U.kb_r == 6 ? 2 : 1;
          else if (U.nsh) U.kb_focus = 0;
          break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          kb_type(U.kb_r, U.kb_c, now);
          break;
     case DIKS_BACK: case DIKS_EXIT:
          if (repeat) return 1;
          set_tab(TAB_HOME);
          U.tab_focus = 1;
          break;
     default:
          return 1;
     }
     U.dirty = 1;
     return 1;
}

/* the Settings tab: three rows (account, phone casting, DeArrow), one button
 * each; UP/DOWN between them, OK presses */
#define SET_ROWS 4
static int account_key(int k, int repeat, double now)
{
     switch (k) {
     case DIKS_CURSOR_UP:
          if (U.set_sel > 0) U.set_sel--;
          else U.tab_focus = 1;
          U.confirm_until = 0;
          break;
     case DIKS_CURSOR_DOWN:
          if (U.set_sel < SET_ROWS - 1) U.set_sel++;
          U.confirm_until = 0;
          break;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT:
          if (repeat) return 1;
          if (U.set_sel == 2) {                          /* autoplay on/off */
               struct job j = { .kind = JB_AUTOPLAY, .flag = !U.autoplay };
               post(j);
               break;
          }
          if (U.set_sel == 3) {                          /* DeArrow on/off */
               struct job j = { .kind = JB_SETTINGS, .flag = !U.dearrow };
               post(j);
               break;
          }
          if (U.set_sel == 1) {                          /* a pairing code for a phone */
               if (U.rcode[0]) { U.rcode[0] = 0; break; }
               struct job j = { .kind = JB_RSTART };
               post_browse(j, "Asking YouTube for a TV code\xe2\x80\xa6");
               break;
          }
          if (U.code[0]) {                               /* Cancel */
               U.code[0] = 0;
          } else if (U.acct.signed_in) {
               if (U.confirm_until > now) {
                    U.confirm_until = 0;
                    struct job j = { .kind = JB_SIGNOUT };
                    post(j);
               } else {
                    U.confirm_until = now + 4;
               }
          } else if (!U.busy) {
               struct job j = { .kind = JB_SIGNIN };
               post_browse(j, "Asking YouTube for a code\xe2\x80\xa6");
          }
          break;
     case DIKS_BACK: case DIKS_EXIT:
          if (repeat) return 1;
          set_tab(TAB_HOME);
          U.tab_focus = 1;
          break;
     default:
          return 1;
     }
     U.dirty = 1;
     return 1;
}
static int card_key(int k, int repeat, double now)
{
     if (U.scr == Y_HOME) {
          if (U.tab_focus) return tab_key(k, repeat);
          if (U.tab == TAB_ACCOUNT) return account_key(k, repeat, now);
          if (U.tab == TAB_SEARCH && (U.kb_focus || !U.nsh)) return kb_key(k, repeat, now);
          if (U.tab != TAB_HOME && (k == DIKS_BACK || k == DIKS_EXIT)) {
               if (!repeat) { set_tab(TAB_HOME); U.tab_focus = 1; }
               return 1;
          }
          if (k == DIKS_CURSOR_UP && (!U.nsh || (U.cur == 0 && (!U.sh[0].grid || U.sh[0].sel < per_line())))) {
               U.tab_focus = 1; U.dirty = 1; return 1;
          }
          if (k == DIKS_CURSOR_LEFT && U.tab == TAB_SEARCH && U.nsh && U.sh[U.cur].sel % per_line() == 0) {
               U.kb_focus = 1; U.dirty = 1; return 1;
          }
     }
     if (U.scr == Y_CHANNEL && U.ch_head) {           /* the Follow button */
          switch (k) {
          case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: {
               if (repeat) return 1;
               struct job j = { .kind = JB_FOLLOW, .flag = !U.ch.followed };
               snprintf(j.id, sizeof j.id, "%s", U.ch.id);
               post(j);
               break;
          }
          case DIKS_CURSOR_DOWN: if (U.nsh) U.ch_head = 0; break;
          case DIKS_BACK: case DIKS_EXIT:
               if (repeat) return 1;
               U.ch_head = 0;
               save_cards();
               go(U.ch_back);
               return 1;
          }
          U.dirty = 1;
          return 1;
     }
     if (U.scr == Y_CHANNEL && k == DIKS_CURSOR_UP && (!U.nsh || U.sh[0].sel < per_line())) {
          U.ch_head = 1;
          U.dirty = 1;
          return 1;
     }
     if (!U.nsh) {
          if ((k == DIKS_BACK || k == DIKS_EXIT) && !repeat) {
               if (U.scr == Y_HOME) U.tab_focus = 1;
               else { save_cards(); go(U.ch_back); }
               U.dirty = 1;
          }
          return 1;
     }
     struct shelf *h = &U.sh[U.cur];
     int step = 0;
     switch (k) {
     case DIKS_CURSOR_LEFT:  if (h->sel > 0) h->sel--; break;
     case DIKS_CURSOR_RIGHT: if (h->sel < h->n - 1) h->sel++; break;
     case DIKS_CURSOR_UP:    step = -1; break;
     case DIKS_CURSOR_DOWN:  step = 1; break;
     case DIKS_PAGE_UP:      step = -3; break;
     case DIKS_PAGE_DOWN:    step = 3; break;
     case DIKS_BACK: case DIKS_EXIT:
          if (repeat) return 1;
          if (U.scr == Y_HOME) { U.tab_focus = 1; U.dirty = 1; return 1; }
          save_cards();
          go(U.ch_back);                             /* the channel -> where it was opened from */
          return 1;
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: case DIKS_PLAY: case DIKS_PLAYPAUSE:
          if (repeat || U.busy) return 1;
          if (h->sel < h->n) play_item(&h->items[h->sel]);
          return 1;
     default:
          return 1;
     }
     if (step && h->grid) {
          int pl = per_line(), t = h->sel + step * pl;
          if (t < 0) t = h->sel % pl;
          if (t > h->n - 1) t = (h->sel / pl + 1) * pl < h->n ? h->n - 1 : h->sel;
          h->sel = t;
     } else if (step) {
          int c = U.cur + (step < 0 ? -1 : 1);
          if (c >= 0 && c < U.nsh) U.cur = c;
     }
     cards_follow();
     /* search results: near the end, 30 more (up to 120) */
     if (U.scr == Y_HOME && U.tab == TAB_SEARCH && h->grid && !U.srch_more &&
         U.srch.n >= U.srch_count && U.srch_count < 120 && h->sel >= h->n - per_line() && U.query[0]) {
          U.srch_more = 1;
          U.srch_count += 30;
          struct job j = { .kind = JB_SEARCH, .n = U.srch_gen, .count = U.srch_count };
          snprintf(j.arg, sizeof j.arg, "%s", U.query);
          post(j);
          snprintf(U.toast, sizeof U.toast, "Loading more\xe2\x80\xa6");
          U.toast_until = now + 2;
     }
     U.dirty = 1;
     return 1;
}

int ytui_key(int k, int repeat, double now)
{
     if (U.scr == Y_PLAYER) { key_player(k, repeat, now); return 1; }
     return card_key(k, repeat, now);
}

/* ------------------------------------------------------------- drawing */

static void text(IDirectFBFont *f, int x, int y, int h, uint32_t col, const char *s, int align)
{
     U.h.text(U.h.ctx, f, x, y, h, col, s, align);
}

static void fit(IDirectFBFont *f, const char *s, int maxw, char *out, size_t n)
{
     snprintf(out, n, "%s", s);
     size_t len = strlen(out);
     for (;;) {
          int w = (int)len * 15;
          if (f) f->GetStringWidth(f, out, -1, &w);
          if (w <= maxw || len == 0 || n < 8) return;
          do len--; while (len > 0 && ((unsigned char)out[len] & 0xc0) == 0x80);
          while (len > 0 && out[len - 1] == ' ') len--;
          if (len + 4 > n) len = n - 4;
          memcpy(out + len, "\xe2\x80\xa6", 4);
     }
}

/* wrapped text, at most maxl lines (the last cut with an ellipsis); blank
 * lines become one paragraph gap. Returns the lines used. */
static int wrap(IDirectFBFont *f, const char *s, int x, int y, int line_h, int maxw, int maxl, uint32_t col)
{
     int lines = 0;
     while (*s && lines < maxl) {
          if (*s == '\n') {
               while (*s == '\n' || *s == ' ') s++;
               if (lines) lines++;
               continue;
          }
          char buf[512];
          int n = 0, last_space = -1, w = 0;
          for (;;) {
               if (!s[n] || s[n] == '\n' || n >= (int)sizeof buf - 8) break;
               int m = n + 1;
               while (((unsigned char)s[m] & 0xc0) == 0x80) m++;
               memcpy(buf, s, (size_t)m);
               buf[m] = 0;
               w = m * 15;
               if (f) f->GetStringWidth(f, buf, -1, &w);
               if (w > maxw) break;
               if (s[n] == ' ') last_space = n;
               n = m;
          }
          int cut = n;
          if (s[n] && s[n] != '\n' && last_space > 0) cut = last_space;
          if (cut <= 0) cut = n > 0 ? n : 1;
          snprintf(buf, sizeof buf, "%.*s", cut, s);
          if (lines == maxl - 1 && s[cut]) {
               char out[520];
               fit(f, buf, maxw - 30, out, sizeof out - 4);
               size_t l = strlen(out);
               if (l < 3 || strcmp(out + l - 3, "\xe2\x80\xa6")) snprintf(out + l, sizeof out - l, "\xe2\x80\xa6");
               text(f, x, y + lines * line_h, line_h, col, out, -1);
          } else {
               text(f, x, y + lines * line_h, line_h, col, buf, -1);
          }
          lines++;
          s += cut;
          while (*s == ' ') s++;
          if (*s == '\n' && s[1] != '\n') s++;
     }
     return lines;
}

static void draw_card(struct canvas *c, const struct yt_item *it, int x, int y, int foc)
{
     char url[256];
     yt_thumb_url(it->id, 'm', url, sizeof url);
     const struct jfimg *img = jfimg_get_url(url, L_W, L_H);
     char dur[24];
     if (it->live) snprintf(dur, sizeof dur, "LIVE");
     else if (it->duration > 0) fmt_clock(it->duration, dur, sizeof dur);
     else dur[0] = 0;
     int dw = (int)strlen(dur) * 15 + 20;
     if (U.h.f_tiny && dur[0]) { U.h.f_tiny->GetStringWidth(U.h.f_tiny, dur, -1, &dw); dw += 20; }
     if (c) {
          if (foc) gfx_rrect(c, x - 8, y - 8, L_W + 16, L_H + 16, C_RAD + 8, 0xffffffff);
          if (img) gfx_image(c, x, y, L_W, L_H, C_RAD, img->px, img->w, 255);
          else {
               gfx_rrect(c, x, y, L_W, L_H, C_RAD, 0xff1c2130);
               gfx_rrect_outline(c, x, y, L_W, L_H, C_RAD, 1, K_PANEL_EDGE);
          }
          if (dur[0]) gfx_rrect(c, x + L_W - dw - 10, y + L_H - 44, dw, 34, 8, it->live ? 0xe0d02020 : 0xd0000000);
          if (it->position > 0 && it->duration > 0) {
               int pw = (int)((L_W - 20) * (it->position / it->duration));
               if (pw > L_W - 20) pw = L_W - 20;
               gfx_rrect(c, x + 10, y + L_H - 6, L_W - 20, 4, 2, 0xa0000000);
               if (pw >= 4) gfx_rrect(c, x + 10, y + L_H - 6, pw, 4, 2, K_RED);
          }
          return;
     }
     char ft[240], sub[240];
     if (!img) {
          fit(U.h.f_small, it->title, L_W - 32, ft, sizeof ft);
          text(U.h.f_small, x + L_W / 2, y + L_H / 2 - 20, 40, K_TEXT2, ft, 0);
     }
     if (dur[0]) text(U.h.f_tiny, x + L_W - 10 - dw / 2, y + L_H - 44, 34, 0xffffffff, dur, 0);
     fit(U.h.f_small, it->title, L_W + 8, ft, sizeof ft);
     text(U.h.f_small, x, y + L_H + 8, 36, foc ? 0xffffffff : K_TEXT, ft, -1);
     card_sub(it, sub, sizeof sub);
     fit(U.h.f_tiny, sub, L_W + 8, ft, sizeof ft);
     text(U.h.f_tiny, x, y + L_H + 44, 30, K_TEXT2, ft, -1);
}

static void draw_cards(struct canvas *c)
{
     if (!U.nsh) return;
     cards_follow();
     int focus = !(U.scr == Y_HOME && (U.tab_focus || (U.tab == TAB_SEARCH && U.kb_focus))) &&
                 !(U.scr == Y_CHANNEL && U.ch_head);
     struct shelf *g = &U.sh[0];
     if (g->grid) {
          int pl = per_line(), lh = grid_line_h(), shown = grid_lines_shown(), top = grid_top();
          for (int l = U.vtop; l < U.vtop + shown; l++)
               for (int k = 0; k < pl; k++) {
                    int i = l * pl + k;
                    if (i >= g->n) break;
                    draw_card(c, &g->items[i], cards_x0() + k * (L_W + C_GAP), top + (l - U.vtop) * lh,
                              focus && i == g->sel);
               }
          return;
     }
     int y = C_TOP, x0 = cards_x0();
     for (int si = U.vtop; si < U.nsh; si++) {
          struct shelf *h = &U.sh[si];
          if (y + shelf_h() - SH_GAP > C_BOT) break;
          if (!c) text(U.h.f_bold, x0, y, SH_TITLE - 16, si == U.cur && focus ? K_TEXT : K_TEXT2, h->title, -1);
          int cy = y + SH_TITLE;
          for (int i = h->scroll; i < h->n; i++) {
               int x = x0 + (i - h->scroll) * (L_W + C_GAP);
               if (x >= 1920) break;
               draw_card(c, &h->items[i], x, cy, focus && si == U.cur && i == h->sel);
          }
          y += shelf_h();
     }
}

static void draw_tabs(struct canvas *c)
{
     static const char *const names[NTABS] = { "Home", "Search", "History", "Settings" };
     int W = U.h.W, w[NTABS], tw = 0, gap = 56;
     for (int t = 0; t < NTABS; t++) {
          w[t] = (int)strlen(names[t]) * 20;
          if (U.h.f_bold) U.h.f_bold->GetStringWidth(U.h.f_bold, names[t], -1, &w[t]);
          tw += w[t] + (t ? gap : 0);
     }
     int x = (W - tw) / 2;
     for (int t = 0; t < NTABS; t++) {
          int sel = t == U.tab, foc = sel && U.tab_focus;
          if (c) {
               if (foc) gfx_rrect(c, x - 24, 30, w[t] + 48, 58, 29, 0xffffffff);
               else if (sel) gfx_rrect(c, x, 82, w[t], 4, 2, K_RED);
          } else {
               text(U.h.f_bold, x, 24, 70, foc ? 0xff0b0e16 : sel ? K_TEXT : K_TEXT2, names[t], -1);
          }
          x += w[t] + gap;
     }
}

static void draw_search(struct canvas *c)
{
     int fw = 6 * KB_KW + 5 * KB_G;
     int kbf = U.kb_focus && !U.tab_focus;
     if (c) {
          gfx_rrect(c, KB_X, C_TOP, fw, 72, 16, K_CARD);
          gfx_rrect_outline(c, KB_X, C_TOP, fw, 72, 16, 1, K_PANEL_EDGE);
     } else {
          char q[80], ft[96];
          snprintf(q, sizeof q, "%s", U.query[0] ? U.query : "Search YouTube");
          fit(U.h.f_item, q, fw - 40, ft, sizeof ft);
          text(U.h.f_item, KB_X + 20, C_TOP, 72, U.query[0] ? K_TEXT : K_TEXT3, ft, -1);
     }
     for (int r = 0; r < 7; r++) {
          int ncol = r < 6 ? 6 : 3;
          for (int k = 0; k < ncol; k++) {
               int col = r < 6 ? k : k * 2;
               int kw = r < 6 ? KB_KW : 2 * KB_KW + KB_G;
               int kx = KB_X + col * (KB_KW + KB_G), ky = KB_Y + r * (KB_KH + KB_G);
               int f = kbf && U.kb_r == r && (r < 6 ? U.kb_c == col : U.kb_c / 2 == k);
               if (c) gfx_rrect(c, kx, ky, kw, KB_KH, 12, f ? 0xffffffff : 0x26ffffff);
               else {
                    char ch[2] = { r < 6 ? kb_rows[r][k] : 0, 0 };
                    text(r < 6 ? U.h.f_item : U.h.f_small, kx + kw / 2, ky, KB_KH, f ? 0xff0b0e16 : K_TEXT,
                         r < 6 ? ch : kb_acts[k], 0);
               }
          }
     }
     if (!c && !U.nsh) {
          const char *msg = strlen(U.query) < 2 ? "Type to search YouTube"
                          : (U.srch_done && !U.query_at) ? "Nothing found" : "Searching\xe2\x80\xa6";
          text(U.h.f_item, SR_X, C_TOP, 72, K_TEXT2, msg, -1);
     }
}

/* one Settings row: a title, up to two lines of text, a button on the right */
static void set_row(struct canvas *c, int y, int row, const char *title, const char *sub, const char *btn)
{
     int W = U.h.W, x = C_X0;
     int bw = (int)strlen(btn) * 18;
     if (U.h.f_bold) U.h.f_bold->GetStringWidth(U.h.f_bold, btn, -1, &bw);
     bw += 72;
     int bx = W - C_X0 - bw, foc = !U.tab_focus && U.set_sel == row;
     if (c) {
          gfx_rrect(c, x - 24, y - 16, W - 2 * C_X0 + 48, 1, 0, K_PANEL_EDGE);
          if (foc) gfx_rrect(c, bx, y + 10, bw, 72, 36, 0xffffffff);
          else {
               gfx_rrect(c, bx, y + 10, bw, 72, 36, 0x40ffffff);
               gfx_rrect_outline(c, bx, y + 10, bw, 72, 36, 1, 0x40ffffff);
          }
     } else {
          text(U.h.f_bold, x, y, 50, K_TEXT, title, -1);
          wrap(U.h.f_small, sub, x, y + 52, 40, bx - x - 40, 2, K_TEXT2);
          text(U.h.f_bold, bx + bw / 2, y + 10, 72, foc ? 0xff0b0e16 : K_TEXT, btn, 0);
     }
}

/* a big code in a box, with where to enter it */
static void code_box(struct canvas *c, int y, const char *where, const char *code)
{
     int x = C_X0;
     if (c) {
          gfx_rrect(c, x, y + 48, 720, 150, 28, K_CARD);
          gfx_rrect_outline(c, x, y + 48, 720, 150, 28, 1, K_PANEL_EDGE);
     } else {
          text(U.h.f_small, x, y, 40, K_TEXT2, where, -1);
          text(U.h.f_title, x + 36, y + 48, 150, K_TEXT, code, -1);
     }
}

static void draw_account(struct canvas *c, double now)
{
     char sub[240], name[160];
     /* the account */
     int y = C_TOP + 20;
     if (U.code[0]) {
          set_row(c, y, 0, "YouTube account", "Waiting for you to approve the sign-in\xe2\x80\xa6", "Cancel");
          char where[200];
          snprintf(where, sizeof where, "On your phone or computer, go to %s and enter:",
                   U.code_url[0] ? U.code_url + (strncmp(U.code_url, "https://", 8) ? 0 : 8) : "google.com/device");
          code_box(c, y + 150, where, U.code);
          y += 340;
     } else if (U.acct.signed_in) {
          snprintf(name, sizeof name, "Signed in%s%s", U.acct.name[0] ? " as " : "", U.acct.name);
          set_row(c, y, 0, name, "Home shows your subscriptions, Watch later, likes and recommendations. "
                  "Videos played here are not added to your YouTube history.",
                  U.confirm_until > now ? "Press OK again to sign out" : "Sign out");
          y += 170;
     } else {
          set_row(c, y, 0, U.acct_known ? "Not signed in" : "Checking\xe2\x80\xa6",
                  "Sign in to see your subscriptions and recommendations. You approve it on your phone; "
                  "no password is typed here.", "Sign in");
          y += 170;
     }
     /* phone casting */
     if (U.remote_enabled && U.rconn[0]) snprintf(sub, sizeof sub, "Connected: %s", U.rconn);
     else if (U.remote_enabled) snprintf(sub, sizeof sub, "On. In the YouTube app on your phone: cast, or "
                                          "Settings \xe2\x80\xba Watch on TV \xe2\x80\xba Link with TV code.");
     else snprintf(sub, sizeof sub, "Play from the YouTube app on your phone: link it once with a TV code.");
     set_row(c, y, 1, "Phone casting", sub, U.rcode[0] ? "Done" : "Link a phone");
     if (U.rcode[0]) {
          code_box(c, y + 150, "In the YouTube app: Settings \xe2\x80\xba Watch on TV \xe2\x80\xba "
                   "Link with TV code, then enter:", U.rcode);
          y += 340;
     } else {
          y += 170;
     }
     set_row(c, y, 2, "Autoplay", "When a video ends, play YouTube's next suggestion after a short countdown.",
             U.autoplay ? "On" : "Off");
     y += 170;
     /* DeArrow */
     set_row(c, y, 3, "DeArrow", "Community-submitted titles and thumbnails in place of clickbait "
             "(dearrow.ajay.app). Only a short hash prefix of each video id is sent.",
             U.dearrow ? "On" : "Off");
}
static void draw_toast(struct canvas *c, double now)
{
     if (U.toast_until <= now) return;
     int W = U.h.W, tw = (int)strlen(U.toast) * 16 + 60;
     if (U.h.f_small) { U.h.f_small->GetStringWidth(U.h.f_small, U.toast, -1, &tw); tw += 60; }
     if (c) gfx_rrect(c, (W - tw) / 2, 50, tw, 64, 32, 0xe0101420);
     else text(U.h.f_small, W / 2, 50, 64, K_TEXT, U.toast, 0);
}

/* the chapter at pos (seconds), or NULL */
static const struct yt_chapter *chapter_at(double pos)
{
     const struct yt_chapter *c = NULL;
     for (int i = 0; U.have_det && i < U.det.nchapters; i++)
          if (U.det.chapters[i].start <= pos) c = &U.det.chapters[i];
     return c;
}

/* above the overlay, while switched on (they outlast the overlay): the
 * description or the comments */
#define O_TOP 600                     /* the panel: 600..1040 (hints in its last 70 px) */
static void draw_panels(struct canvas *c)
{
     int W = U.h.W, top = 60, h = O_TOP - 90;
     if (U.desc_on && U.have_det && U.det.description) {
          if (c) gfx_rrect(c, 60, top, W - 120, h, 28, 0xe0101420);
          else {
               text(U.h.f_bold, 110, top + 24, 50, K_TEXT, "Description", -1);
               wrap(U.h.f_small, U.det.description, 110, top + 90, 38, W - 220, (h - 110) / 38, 0xffd0d6e2);
          }
     }
     if (!U.comm_on) return;
     if (c) {
          gfx_rrect(c, 60, top, W - 120, h, 28, 0xe0101420);
          return;
     }
     char head[64];
     if (U.ncomm) snprintf(head, sizeof head, "Comments  %d / %d", U.comm_top + 1, U.ncomm);
     else snprintf(head, sizeof head, "Comments");
     text(U.h.f_bold, 110, top + 24, 50, K_TEXT, head, -1);
     if (!U.ncomm) {
          text(U.h.f_small, 110, top + 90, 40, K_TEXT2, U.comm_loading ? "Loading\xe2\x80\xa6" : "No comments", -1);
          return;
     }
     int y = top + 90, bottom = top + h - 20;
     for (int i = U.comm_top; i < U.ncomm && y + 80 < bottom; i++) {
          const struct yt_comment *m = &U.comm[i];
          char who[160];
          snprintf(who, sizeof who, "%s", m->author);
          if (m->age[0]) join(who, sizeof who, m->age);
          if (m->likes[0]) { size_t l = strlen(who); snprintf(who + l, sizeof who - l, DOT "%s likes", m->likes); }
          text(U.h.f_small, 110, y, 40, K_TEXT, who, -1);
          int lines = wrap(U.h.f_small, m->text, 110, y + 40, 38, W - 220, (bottom - y - 40) / 38 < 3 ? (bottom - y - 40) / 38 : 3,
                           0xffd0d6e2);
          y += 40 + (lines ? lines : 1) * 38 + 22;
     }
}

/* "Up next": the suggestions, a row of cards above the overlay */
static void draw_related(struct canvas *c)
{
     int W = U.h.W, y = O_TOP - 300, x0 = 110, n = U.rel.n;
     int per = (W - 2 * x0 + C_GAP) / (L_W + C_GAP);
     if (U.rel_sel < U.rel_scroll) U.rel_scroll = U.rel_sel;
     if (U.rel_sel >= U.rel_scroll + per) U.rel_scroll = U.rel_sel - per + 1;
     if (c) gfx_rrect(c, 60, y - 70, W - 120, L_H + 160, 28, 0xd8101420);
     else text(U.h.f_bold, x0, y - 60, 50, K_TEXT, "Up next", -1);
     for (int i = U.rel_scroll; i < n && i < U.rel_scroll + per; i++)
          draw_card(c, &U.rel.items[i], x0 + (i - U.rel_scroll) * (L_W + C_GAP), y, i == U.rel_sel);
}

/* the player's overlay: details, the bar with SponsorBlock and chapters on
 * it, actions */
static void draw_overlay(struct canvas *c, double now)
{
     int W = U.h.W, H = U.h.H;
     int bx = 110, bw = W - 220, by = O_TOP + 250;   /* bar; times and actions under it */
     double pos = U.seek_pending ? U.seek_target_ms : cur_pos(now);
     const struct yt_item *it = &U.playing;
     if (U.osd_focus == 2 && U.rel.n) draw_related(c);
     if (c) {
          gfx_rrect(c, 60, O_TOP, W - 120, H - O_TOP - 40, 32, 0xd8101420);
          gfx_rrect(c, bx, by, bw, 10, 5, K_TRACK);
          if (U.dur_ms > 0) {
               int w = (int)(bw * (pos / U.dur_ms));
               if (w > bw) w = bw;
               if (w > 10) gfx_rrect(c, bx, by, w, 10, 5, K_RED);
               for (int i = 0; U.have_det && i < U.det.nsegs; i++) {
                    const struct yt_seg *g = &U.det.segs[i];
                    int sx = bx + (int)(bw * (g->start * 1000.0 / U.dur_ms));
                    int ex = bx + (int)(bw * (g->end * 1000.0 / U.dur_ms));
                    if (ex - sx < 4) ex = sx + 4;
                    gfx_rrect(c, sx, by, ex - sx, 10, 2, g->skip ? K_SKIP : K_SEG);
               }
               /* chapters: a gap in the bar where each starts */
               for (int i = 1; U.have_det && i < U.det.nchapters; i++) {
                    int cx = bx + (int)(bw * (U.det.chapters[i].start * 1000.0 / U.dur_ms));
                    gfx_rrect(c, cx - 2, by - 1, 4, 12, 0, 0xff101420);
               }
               int kx = bx + w;                        /* the playhead */
               gfx_rrect(c, kx - 10, by - 5, 20, 20, 10, 0xffffffff);
          }
     } else {
          char t[240], line[300], v[32], a[32];
          fit(U.h.f_head, it->title, bw - 260, t, sizeof t);
          text(U.h.f_head, bx, O_TOP + 26, 64, K_TEXT, t, -1);
          const char *st = U.pstate == 2 ? "Paused" : U.seek_pending ? "Seeking" : "";
          if (st[0]) text(U.h.f_bold, bx + bw, O_TOP + 26, 64, K_TEXT2, st, 1);
          snprintf(line, sizeof line, "%s", it->channel);
          if (U.have_det && U.det.followed) join(line, sizeof line, "Following");
          fmt_views(it->views, v, sizeof v);
          fmt_ago(it->published, a, sizeof a);
          join(line, sizeof line, v);
          join(line, sizeof line, a);
          text(U.h.f_small, bx, O_TOP + 96, 40, K_TEXT2, line, -1);
          const struct yt_chapter *ch = chapter_at(pos / 1000.0);
          if (ch && ch->title[0]) {
               char ct[120], ft[130];
               snprintf(ct, sizeof ct, "Chapter: %s", ch->title);
               fit(U.h.f_small, ct, bw, ft, sizeof ft);
               text(U.h.f_small, bx, O_TOP + 140, 40, 0xffd0d6e2, ft, -1);
          }
          char p1[32], p2[32];
          if (U.live) snprintf(p1, sizeof p1, "LIVE");
          else fmt_clock(pos / 1000.0, p1, sizeof p1);
          fmt_clock(U.dur_ms / 1000.0, p2, sizeof p2);
          text(U.h.f_small, bx, by + 22, 40, K_TEXT2, p1, -1);
          if (!U.live && U.dur_ms > 0) text(U.h.f_small, bx + bw, by + 22, 40, K_TEXT2, p2, 1);
     }
     /* the actions, right of the time under the bar */
     int ay = by + 22, ah = 58, ax = bx + 200;
     for (int i = 0; i < U.nbtn; i++) {
          int aw = (int)strlen(U.btn[i].title) * 16;
          if (U.h.f_small) U.h.f_small->GetStringWidth(U.h.f_small, U.btn[i].title, -1, &aw);
          aw += 56;
          int foc = U.osd_focus == 1 && i == U.bsel;
          if (c) {
               if (foc) gfx_rrect(c, ax, ay, aw, ah, ah / 2, 0xffffffff);
               else gfx_rrect_outline(c, ax, ay, aw, ah, ah / 2, 2, 0x50ffffff);
          } else {
               text(U.h.f_small, ax + aw / 2, ay, ah, foc ? 0xff0b0e16 : K_TEXT, U.btn[i].title, 0);
          }
          ax += aw + 16;
     }
     static const char *const hb[] = { "OK", "Pause", "LEFT/RIGHT", "Seek", "DOWN", "More", "BACK", "Hide" };
     static const char *const ha[] = { "LEFT/RIGHT", "Move", "OK", "Select", "DOWN", "Up next", "UP", "Back" };
     static const char *const hr[] = { "LEFT/RIGHT", "Move", "OK", "Play", "UP", "Back" };
     if (U.osd_focus == 2) U.h.hints(U.h.ctx, c, W / 2, H - 98, hr, 3);
     else if (U.osd_focus) U.h.hints(U.h.ctx, c, W / 2, H - 98, ha, U.rel.n ? 4 : 3);
     else U.h.hints(U.h.ctx, c, W / 2, H - 98, hb, 4);
}

/* the end, autoplay: what plays next, and when */
static void draw_upnext(struct canvas *c, double now)
{
     int W = U.h.W, H = U.h.H;
     const struct yt_item *nx = &U.rel.items[0];
     char url[256];
     yt_thumb_url(nx->id, 'l', url, sizeof url);
     const struct jfimg *im = jfimg_get_url(url, 720, 405);
     if (c) {
          gfx_fill_vgradient_full(c, 0xff05070c, 0xff0b0e16);
          if (im) gfx_image(c, (W - 720) / 2, 180, 720, 405, 18, im->px, im->w, 255);
          else gfx_rrect(c, (W - 720) / 2, 180, 720, 405, 18, 0xff1c2130);
     } else {
          char ft[240], when[48];
          text(U.h.f_bold, W / 2, 100, 60, K_TEXT2, "Up next", 0);
          fit(U.h.f_head, nx->title, W - 240, ft, sizeof ft);
          text(U.h.f_head, W / 2, 620, 70, K_TEXT, ft, 0);
          text(U.h.f_small, W / 2, 700, 44, K_TEXT2, nx->channel, 0);
          int left = (int)(U.upnext_until - now + 0.99);
          snprintf(when, sizeof when, "Playing in %d s", left > 0 ? left : 0);
          text(U.h.f_item, W / 2, 780, 50, K_TEXT, when, 0);
     }
     static const char *const h[] = { "OK", "Play now", "BACK", "Cancel" };
     U.h.hints(U.h.ctx, c, W / 2, H - 98, h, 2);
}

static void draw_player(struct canvas *c, double now)
{
     int W = U.h.W;
     if (U.upnext && U.rel.n) { draw_upnext(c, now); return; }
     if (U.pstate == 0) {
          /* loading: opaque over the video plane until it plays */
          if (c) {
               gfx_fill_vgradient_full(c, 0xff05070c, 0xff0b0e16);
               char url[256];
               yt_thumb_url(U.playing.id, 'l', url, sizeof url);
               const struct jfimg *bd = jfimg_get_url(url, 720, 405);
               if (bd) gfx_image(c, (W - 720) / 2, 200, 720, 405, 18, bd->px, bd->w, 160);
          } else {
               char ft[240];
               fit(U.h.f_head, U.playing.title, W - 240, ft, sizeof ft);
               text(U.h.f_head, W / 2, 650, 70, K_TEXT, ft, 0);
               text(U.h.f_small, W / 2, 730, 44, K_TEXT2, U.playing.channel, 0);
               text(U.h.f_item, W / 2, 800, 50, K_TEXT2, "Loading\xe2\x80\xa6", 0);
               if (U.err_until > now) text(U.h.f_item, W / 2, 880, 50, 0xffff8080, U.err, 0);
          }
          return;
     }
     draw_toast(c, now);
     draw_panels(c);
     if (U.osd_until > now || U.pstate == 2 || U.seek_pending) draw_overlay(c, now);
     if (U.err_until > now && !c) text(U.h.f_item, W / 2, 140, 60, 0xffff8080, U.err, 0);
}

void ytui_draw(struct canvas *c, double now)
{
     if (!U.on) return;
     int W = U.h.W, H = U.h.H;
     if (U.scr == Y_PLAYER) { draw_player(c, now); return; }
     if (c) gfx_fill_vgradient_full(c, 0xf005070c, 0xfa05070c);
     if (!c) {
          char title[140];
          if (U.scr == Y_CHANNEL) fit(U.h.f_title, U.ch.title, 1100, title, sizeof title);
          else snprintf(title, sizeof title, "YouTube");
          text(U.h.f_title, C_X0, 24, 70, K_TEXT, title, -1);
     }
     if (U.scr == Y_CHANNEL && U.have_ch) {            /* Follow / Unfollow, top right */
          const char *t = U.ch.followed ? "Following \xe2\x9c\x93" : "Follow";
          if (U.ch_head) t = U.ch.followed ? "Unfollow" : "Follow";
          int bw = (int)strlen(t) * 18;
          if (U.h.f_bold) U.h.f_bold->GetStringWidth(U.h.f_bold, t, -1, &bw);
          bw += 64;
          int bx = W - C_X0 - bw, by = 30;
          if (c) {
               if (U.ch_head) gfx_rrect(c, bx, by, bw, 60, 30, 0xffffffff);
               else if (U.ch.followed) gfx_rrect_outline(c, bx, by, bw, 60, 30, 2, 0x60ffffff);
               else gfx_rrect(c, bx, by, bw, 60, 30, K_RED);
          } else {
               text(U.h.f_bold, bx + bw / 2, by, 60, U.ch_head ? 0xff0b0e16 : K_TEXT, t, 0);
          }
     }
     if (U.scr == Y_HOME) {
          draw_tabs(c);
          if (U.tab == TAB_SEARCH) draw_search(c);
          else if (U.tab == TAB_ACCOUNT) draw_account(c, now);
          else if (!c && !U.nsh && !U.busy) {
               const char *msg = U.tab == TAB_HISTORY ? "Nothing watched yet"
                               : U.home_loaded ? "Search for a video and play it; in the player, press DOWN and"
                                 " choose Follow: new videos from the channels you follow appear here" : "";
               if (msg[0]) wrap(U.h.f_item, msg, C_X0, C_TOP + 40, 56, W - 2 * C_X0, 3, K_TEXT2);
          }
     }
     draw_cards(c);
     draw_toast(c, now);
     int my = U.scr == Y_HOME ? 74 : 24, mh = U.scr == Y_HOME ? 34 : 70;
     IDirectFBFont *mf = U.scr == Y_HOME ? U.h.f_small : U.h.f_item;
     if (!c && U.err_until > now) text(mf, W / 2, my, mh, 0xffff8080, U.err, 0);
     else if (!c && U.busy && U.busy_msg[0] && U.toast_until <= now) text(mf, W / 2, my, mh, K_TEXT2, U.busy_msg, 0);
     static const char *const hs[] = { "ARROWS", "Move", "OK", "Play", "BACK", "Back" };
     static const char *const ha[] = { "ARROWS", "Move", "OK", "Select", "BACK", "Back" };
     int cards = U.scr == Y_CHANNEL || (!U.tab_focus && (U.tab == TAB_HOME || U.tab == TAB_HISTORY ||
                                                       (U.tab == TAB_SEARCH && !U.kb_focus)));
     U.h.hints(U.h.ctx, c, W / 2, H - 56, cards ? hs : ha, 3);
}
