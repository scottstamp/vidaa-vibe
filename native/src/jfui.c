/*
 * The Jellyfin panel (see jfui.h).
 *
 * Home and the lists are card screens laid out like Jellyfin's web client:
 * Home is shelves (My Media, Continue Watching, Next Up), each a row of
 * 16:9 cards that scrolls sideways; a list is a grid of posters (films,
 * series, seasons) or 16:9 cards (episodes, folders). Images: jfimg.c.
 *
 * Screens: SERVERS (pick one of the discovered servers) -> SIGNIN (Quick
 * Connect code) -> HOME (continue watching, next up, libraries) -> LIST
 * (a library, folder, series or season; a stack) -> DETAIL (play, resume,
 * from the start, mark played) -> PLAYER (full-screen video, OSD on keys).
 */
#define _GNU_SOURCE
#include "jfui.h"
#include "jf.h"
#include "biz.h"
#include "jfimg.h"
#include "jfsub.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STATE_DEFAULT "/var/local/vibe/state/jellyfin"
/* $JF_STATE overrides (tests run as root must not write the UI's state) */
#define STATE_FILE (getenv("JF_STATE") ? getenv("JF_STATE") : STATE_DEFAULT)
#define SWITCH_OFF "/var/local/vibe/jf.off"
#define PLAYER_BIN "/var/local/vibe/vibe-play"

/* palette: as vibe-ui.c */
#define K_TEXT      0xfff0f3fa
#define K_TEXT2     0xff98a1b4
#define K_TEXT3     0xff6b7488
#define K_CARD      0xeb1c2130
#define K_PANEL_EDGE 0x26ffffff
#define K_ACCENT_L  0xff4f7cff
#define K_ACCENT_R  0xff8a5cff
#define K_TRACK     0x38ffffff

#define TICKS_PER_MS 10000LL
/* the remote's CC button: a Hisense code past DirectFB's list (0xf069,
 * seen in vibe-ui's log), not DIKS_SUBTITLE */
#define KEY_CC 0xf069

/* ------------------------------------------------------------ the worker */

enum job_kind {
     JB_DISCOVER, JB_CONNECT, JB_QC_START, JB_QC_POLL, JB_HOME, JB_LIST,
     JB_DETAIL, JB_PLAYINFO, JB_REPORT, JB_PLAYED, JB_LOGOUT, JB_STOP_TC, JB_SUBS
};
enum list_kind { LK_CHILDREN, LK_SEASONS, LK_EPISODES };

struct job {
     enum job_kind kind;
     int  gen;                 /* browse jobs: results of older gens dropped */
     char id[JF_ID], id2[JF_ID];
     char arg[256];
     int  start, lkind;
     int64_t ticks;
     int  event, paused, flag;
     int  audio, sub;          /* JB_PLAYINFO: tracks (-1 / JF_SUB_DEFAULT: the server's) */
     struct jf_play *play;     /* JB_REPORT / JB_STOP_TC: a copy, freed by worker */
};

struct result {
     enum job_kind kind;
     int  gen, rc;
     char err[160];
     struct jf_server servers[8];
     int  nservers;
     char code[16], secret[128];
     struct jf_list lists[3];
     struct jf_item item;
     struct jf_play *play;
     char server_name[64], user_name[64];
     int  start;
     struct jf_subs subs;       /* JB_SUBS */
};

#define QMAX 32
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_cond = PTHREAD_COND_INITIALIZER;
static struct job      jobs[QMAX];
static int             jq_head, jq_n;
static struct result  *results[QMAX];
static int             rq_head, rq_n;
static int             wake_fd = -1;          /* unused; the main loop polls */
static int             worker_up;

static struct jf_session S;                   /* the worker's */

static void post(struct job j)
{
     pthread_mutex_lock(&q_lock);
     if (jq_n < QMAX) {
          jobs[(jq_head + jq_n) % QMAX] = j;
          jq_n++;
          pthread_cond_signal(&q_cond);
     } else if (j.play) {
          free(j.play);
     }
     pthread_mutex_unlock(&q_lock);
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
     if (r) {               /* queue full: drop */
          for (int i = 0; i < 3; i++) jf_list_free(&r->lists[i]);
          free(r->item.overview);
          free(r->play);
          jf_subs_free(&r->subs);
          free(r);
     }
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
          if (!r) { free(j.play); continue; }
          r->kind = j.kind;
          r->gen = j.gen;
          r->start = j.start;
          switch (j.kind) {
          case JB_DISCOVER:
               r->nservers = jf_discover(r->servers, 8, 2500);
               r->rc = 0;
               break;
          case JB_CONNECT: {
               struct jf_session t = S;
               snprintf(t.server, sizeof t.server, "%s", j.arg);
               r->rc = jf_public_info(&t);
               if (r->rc == 0) {
                    if (strcmp(t.server_id, S.server_id) != 0)
                         t.token[0] = t.user_id[0] = t.user_name[0] = 0;
                    S = t;
                    jf_session_save(&S, STATE_FILE);
                    jfimg_session(&S);
               }
               break;
          }
          case JB_QC_START:
               r->rc = jf_qc_enabled(&S) ? jf_qc_initiate(&S, r->code, r->secret) : -1;
               if (r->rc && !jf_error()[0]) snprintf(r->err, sizeof r->err, "Quick Connect is off on this server");
               break;
          case JB_QC_POLL: {
               int p = jf_qc_poll(&S, j.arg);
               if (p == 1) {
                    r->rc = jf_qc_finish(&S, j.arg);
                    if (r->rc == 0) { jf_session_save(&S, STATE_FILE); jfimg_session(&S); }
                    r->start = 1;           /* signed in */
               } else {
                    r->rc = p < 0 ? -1 : 0;
                    r->start = 0;
               }
               break;
          }
          case JB_HOME:
               r->rc = jf_views(&S, &r->lists[2]);
               if (r->rc == 0) {
                    jf_resume(&S, 12, &r->lists[0]);     /* optional rows */
                    jf_next_up(&S, 12, &r->lists[1]);
               }
               break;
          case JB_LIST:
               if (j.lkind == LK_SEASONS) r->rc = jf_seasons(&S, j.id, &r->lists[0]);
               else if (j.lkind == LK_EPISODES) r->rc = jf_episodes(&S, j.id, j.id2, &r->lists[0]);
               else r->rc = jf_children(&S, j.id, j.start, 100, &r->lists[0]);
               break;
          case JB_DETAIL:
               r->rc = jf_item(&S, j.id, &r->item);
               break;
          case JB_PLAYINFO:
               r->play = calloc(1, sizeof *r->play);
               r->rc = r->play ? jf_playback(&S, j.id, j.ticks, j.audio, j.sub, r->play) : -1;
               break;
          case JB_REPORT:
               r->rc = jf_report(&S, j.id, j.play, j.event, j.ticks, j.paused);
               break;
          case JB_STOP_TC:
               jf_stop_transcode(&S, j.play);
               r->rc = 0;
               break;
          case JB_PLAYED:
               r->rc = jf_set_played(&S, j.id, j.flag);
               break;
          case JB_SUBS: {
               char *data = NULL;
               size_t len = 0;
               r->rc = jf_subtitle(&S, j.id, j.arg, j.start, &data, &len);
               if (r->rc == 0) r->rc = jf_subs_parse(data, len, &r->subs);
               free(data);
               break;
          }
          case JB_LOGOUT:
               jf_logout(&S);
               jf_session_save(&S, STATE_FILE);
               jfimg_session(&S);
               r->rc = 0;
               break;
          }
          if (r->rc != 0 && !r->err[0]) snprintf(r->err, sizeof r->err, "%s", jf_error());
          snprintf(r->server_name, sizeof r->server_name, "%s", S.server_name);
          snprintf(r->user_name, sizeof r->user_name, "%s", S.user_name);
          free(j.play);
          deliver(r);
     }
     return NULL;
}

/* ------------------------------------------------------------- UI state */

enum scr { J_SERVERS, J_SIGNIN, J_HOME, J_LIST, J_DETAIL, J_PLAYER };

struct row {                   /* a line of a list screen */
     int  header;              /* a section title, not selectable */
     const char *title;
     struct jf_item *it;       /* NULL for headers and fixed rows */
     int  action;              /* DETAIL: what OK does */
};

#define MAX_ROWS 512
#define MAX_DEPTH 6

struct level {                 /* one LIST screen */
     struct jf_list l;
     enum list_kind lkind;
     char id[JF_ID], id2[JF_ID];
     char title[128];
     int  sel, top;
     int  loading_more;
};

enum { A_PLAY, A_RESUME, A_RESTART, A_PLAYED, A_UNPLAYED, A_OPEN };

static struct {
     int  on, inited;
     struct jfui_host h;
     enum scr scr;
     int  gen;
     int  busy;
     char busy_msg[64];
     char err[160];
     double err_until;
     char server_name[64], user_name[64];
     int  have_server, signed_in;

     struct jf_server servers[8];
     int  nservers;

     char qc_code[16], qc_secret[128];
     double qc_next, qc_until;

     struct jf_list home[3];     /* resume, next up, views */
     struct level lv[MAX_DEPTH];
     int  depth;                 /* LIST screens open */

     struct jf_item detail;
     int  have_detail;

     struct row rows[MAX_ROWS];  /* the current screen's rows */
     int  nrows, sel, top;
     int  home_sel;              /* kept across trips into lists */

     /* card screens (HOME, LIST): shelves of cards */
     struct shelf {
          int  kind;              /* SH_* */
          const char *title;
          struct jf_item *items;
          int  n, land, grid;     /* 16:9 cards (else 2:3 posters); a wrapping grid */
          int  sel, scroll;       /* focused card; first card shown (rows) */
     } sh[4];
     int  nsh, cur, vtop;        /* shelves, the focused one, the first shown
                                  * (grid: the first line shown) */
     int  hsel[4], hscroll[4], hcur, hvtop;   /* Home's, kept by SH_* kind */

     /* playback */
     pid_t pid;
     char prev_input[64];   /* what showed before the player took the path */
     int  to_fd, from_fd;
     char rbuf[2048];
     int  rlen;
     struct jf_play *play;
     struct jf_item playing;     /* what is playing (copy, no overview) */
     enum scr detail_back;       /* where BACK from the detail goes */
     enum scr play_back;         /* where the end of playback goes */
     int  pstate;                /* 0 opening, 1 playing, 2 paused, 3 ended */
     double pos_ms, dur_ms;
     double osd_until, next_report, next_redraw;
     double seek_target_ms, seek_at;   /* collapsed LEFT/RIGHT presses */
     int  seek_pending;
     int  reported_start;
     double pos_at;              /* when pos_ms was last set (the player says it every second) */
     double pos_moved;           /* when it last changed (a frozen end) */
     /* subtitles: the track shown (-1 off), its cues, the menu */
     int  sub_index;
     struct jf_subs subs;
     const struct jf_cue *cue_shown;
     int  menu_on, menu_sel;
     int  want_audio;            /* switch to this audio track once playing (-1 none) */
     int  dirty;
} U;

static void set_err(const char *msg, double now)
{
     snprintf(U.err, sizeof U.err, "%s", msg);
     U.err_until = now + 6;
     U.dirty = 1;
     fprintf(stderr, "jellyfin: %s\n", msg);
}

static void busy(const char *msg)
{
     U.busy = 1;
     snprintf(U.busy_msg, sizeof U.busy_msg, "%s", msg);
     U.dirty = 1;
}

/* a browse job: anything older is now stale */
static void post_browse(struct job j, const char *msg)
{
     j.gen = ++U.gen;
     post(j);
     busy(msg);
}

int jfui_available(void)
{
     return access(SWITCH_OFF, F_OK) != 0;
}

int jfui_playing(void)
{
     return U.on && U.scr == J_PLAYER;
}

/* ----------------------------------------------------------- row models */

static void fmt_dur(int64_t ticks, char *out, size_t n)
{
     long m = (long)(ticks / 10000000 / 60);
     if (m >= 60) snprintf(out, n, "%ldh %02ldm", m / 60, m % 60);
     else snprintf(out, n, "%ld min", m);
}

static void fmt_clock(double ms, char *out, size_t n)
{
     long s = (long)(ms / 1000);
     if (s < 0) s = 0;
     if (s >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
     else snprintf(out, n, "%ld:%02ld", s / 60, s % 60);
}

static void item_sub(const struct jf_item *it, char *out, size_t n)
{
     char d[32] = "";
     out[0] = 0;
     switch (it->type) {
     case JF_VIEW:
          snprintf(out, n, "%s", it->collection[0] ? it->collection : "Library");
          break;
     case JF_EPISODE:
          if (it->season >= 0 && it->episode >= 0)
               snprintf(out, n, "%s%sS%d E%d", it->series_name, it->series_name[0] ? "  \xc2\xb7  " : "",
                        it->season, it->episode);
          else snprintf(out, n, "%s", it->series_name);
          break;
     case JF_SERIES:
          if (it->unplayed) snprintf(out, n, "%d%s%d unwatched", it->year, it->year ? "  \xc2\xb7  " : "", it->unplayed);
          else if (it->year) snprintf(out, n, "%d", it->year);
          break;
     case JF_SEASON:
          if (it->unplayed) snprintf(out, n, "%d unwatched", it->unplayed);
          else if (it->played) snprintf(out, n, "Watched");
          break;
     case JF_FOLDER: case JF_BOXSET: case JF_PLAYLIST:
          if (it->child_count) snprintf(out, n, "%d items", it->child_count);
          break;
     default:
          if (it->runtime_ticks) fmt_dur(it->runtime_ticks, d, sizeof d);
          if (it->year && d[0]) snprintf(out, n, "%d  \xc2\xb7  %s", it->year, d);
          else if (it->year) snprintf(out, n, "%d", it->year);
          else snprintf(out, n, "%s", d);
          break;
     }
     if (it->position_ticks > 0 && it->runtime_ticks > 0) {
          size_t l = strlen(out);
          snprintf(out + l, n - l, "%s%d%% watched", l ? "  \xc2\xb7  " : "",
                   (int)(it->position_ticks * 100 / it->runtime_ticks));
     } else if (it->played && it->type != JF_SEASON && it->type != JF_SERIES) {
          size_t l = strlen(out);
          snprintf(out + l, n - l, "%sWatched", l ? "  \xc2\xb7  " : "");
     }
}

static void add_row(int header, const char *title, struct jf_item *it, int action)
{
     if (U.nrows >= MAX_ROWS) return;
     U.rows[U.nrows++] = (struct row){ header, title, it, action };
}

static int first_selectable(int from, int dir)
{
     for (int i = from; i >= 0 && i < U.nrows; i += dir)
          if (!U.rows[i].header) return i;
     return -1;
}

/* ------------------------------------------------------------ card screens */

enum { SH_MEDIA, SH_RESUME, SH_NEXT, SH_ACCOUNT };

/* geometry, UI pixels (1920x1080) */
#define C_X0     96                 /* left edge of the cards */
#define C_TOP    170                /* below the title */
#define C_BOT    (1080 - 120)       /* above the key hints */
#define L_W      352                /* 16:9 card */
#define L_H      198
#define P_W      196                /* 2:3 poster */
#define P_H      294
#define C_GAP    32
#define C_RAD    14
#define SH_TITLE 64                 /* a shelf's title above its cards */
#define C_TEXT   78                 /* two lines under a card */
#define SH_GAP   24

static int card_w(const struct shelf *h) { return h->land ? L_W : P_W; }
static int card_h(const struct shelf *h) { return h->land ? L_H : P_H; }
static int per_line(const struct shelf *h)
{
     return (1920 - 2 * C_X0 + C_GAP) / (card_w(h) + C_GAP);
}
static int shelf_h(const struct shelf *h) { return SH_TITLE + card_h(h) + C_TEXT + SH_GAP; }
static int grid_line_h(const struct shelf *h) { return card_h(h) + C_TEXT + 16; }
static int grid_lines_shown(const struct shelf *h)
{
     int n = (C_BOT - C_TOP) / grid_line_h(h);
     return n < 1 ? 1 : n;
}

/* keep the focus in view: rows scroll sideways, Home and grids up/down */
static void cards_follow(void)
{
     if (!U.nsh) return;
     struct shelf *h = &U.sh[U.cur];
     if (h->grid) {
          int pl = per_line(h), line = h->sel / pl, shown = grid_lines_shown(h);
          if (line < U.vtop) U.vtop = line;
          if (line >= U.vtop + shown) U.vtop = line - shown + 1;
          return;
     }
     int full = per_line(h);
     if (h->sel < h->scroll) h->scroll = h->sel;
     if (h->sel >= h->scroll + full) h->scroll = h->sel - full + 1;
     if (U.cur < U.vtop) U.vtop = U.cur;
     for (;;) {                           /* shelves vtop..cur must fit */
          int y = C_TOP;
          for (int i = U.vtop; i <= U.cur; i++) y += shelf_h(&U.sh[i]);
          if (y - SH_GAP <= C_BOT || U.vtop >= U.cur) break;
          U.vtop++;
     }
}

/* a list's cards: posters for films, series, seasons, collections */
static int poster_type(enum jf_type t)
{
     return t == JF_MOVIE || t == JF_SERIES || t == JF_SEASON || t == JF_BOXSET;
}

static int card_screen(void) { return U.scr == J_HOME || U.scr == J_LIST; }

static void add_shelf(int kind, const char *title, struct jf_item *items, int n, int land, int grid)
{
     if (U.nsh >= 4) return;
     U.sh[U.nsh++] = (struct shelf){ kind, title, items, n, land, grid, 0, 0 };
}

static void build_cards(void)
{
     U.nsh = 0;
     if (U.scr == J_HOME) {
          if (U.home[2].n) add_shelf(SH_MEDIA, "My Media", U.home[2].items, U.home[2].n, 1, 0);
          if (U.home[0].n) add_shelf(SH_RESUME, "Continue Watching", U.home[0].items, U.home[0].n, 1, 0);
          if (U.home[1].n) add_shelf(SH_NEXT, "Next Up", U.home[1].items, U.home[1].n, 1, 0);
          add_shelf(SH_ACCOUNT, "Account", NULL, 1, 1, 0);       /* one card: Sign out */
          U.cur = 0;
          for (int i = 0; i < U.nsh; i++) {
               struct shelf *h = &U.sh[i];
               h->sel = U.hsel[h->kind];
               h->scroll = U.hscroll[h->kind];
               if (h->sel >= h->n) h->sel = h->n - 1;
               if (h->sel < 0) h->sel = 0;
               if (h->scroll > h->sel) h->scroll = h->sel;
               if (h->kind == U.hcur) U.cur = i;
          }
          U.vtop = U.hvtop < U.nsh ? U.hvtop : 0;
     } else if (U.scr == J_LIST) {
          struct level *L = &U.lv[U.depth - 1];
          int posters = L->l.n > 0 && poster_type(L->l.items[0].type);
          add_shelf(0, NULL, L->l.items, L->l.n, !posters, 1);
          U.sh[0].sel = L->sel < L->l.n ? L->sel : 0;
          U.cur = 0;
          U.vtop = L->top;
     }
}

/* where the focus is, kept for when the screen comes back */
static void save_cards(void)
{
     if (U.scr == J_HOME) {
          for (int i = 0; i < U.nsh; i++) {
               U.hsel[U.sh[i].kind] = U.sh[i].sel;
               U.hscroll[U.sh[i].kind] = U.sh[i].scroll;
          }
          if (U.nsh) U.hcur = U.sh[U.cur].kind;
          U.hvtop = U.vtop;
     } else if (U.scr == J_LIST && U.depth > 0 && U.nsh) {
          U.lv[U.depth - 1].sel = U.sh[0].sel;
          U.lv[U.depth - 1].top = U.vtop;
     }
}

static struct jf_item *focused_item(void)
{
     if (!card_screen() || U.cur >= U.nsh) return NULL;
     struct shelf *h = &U.sh[U.cur];
     return h->items && h->sel >= 0 && h->sel < h->n ? &h->items[h->sel] : NULL;
}

static void build_rows(void)
{
     U.nrows = 0;
     if (card_screen()) { build_cards(); return; }
     switch (U.scr) {
     case J_SERVERS:
          for (int i = 0; i < U.nservers; i++) add_row(0, U.servers[i].name, NULL, i);
          add_row(0, "Search again", NULL, -1);
          break;
     case J_SIGNIN:
          add_row(0, "Use another server", NULL, 0);
          break;
     case J_HOME: {
          static const char *const H[3] = { "Continue watching", "Next up", "Libraries" };
          for (int s = 0; s < 3; s++) {
               if (!U.home[s].n) continue;
               add_row(1, H[s], NULL, 0);
               for (int i = 0; i < U.home[s].n; i++)
                    add_row(0, U.home[s].items[i].name, &U.home[s].items[i], A_OPEN);
          }
          add_row(1, "Account", NULL, 0);
          add_row(0, "Sign out", NULL, -1);
          break;
     }
     case J_LIST: {
          struct level *L = &U.lv[U.depth - 1];
          for (int i = 0; i < L->l.n; i++)
               add_row(0, L->l.items[i].name, &L->l.items[i], A_OPEN);
          break;
     }
     case J_DETAIL: {
          struct jf_item *it = &U.detail;
          int playable = it->type == JF_MOVIE || it->type == JF_EPISODE || it->type == JF_VIDEO ||
                         it->type == JF_MUSICVIDEO;
          if (playable && it->position_ticks > 0) {
               add_row(0, "Resume", it, A_RESUME);
               add_row(0, "Play from start", it, A_RESTART);
          } else if (playable) {
               add_row(0, "Play", it, A_PLAY);
          }
          if (playable) add_row(0, it->played ? "Mark unwatched" : "Mark watched", it,
                                it->played ? A_UNPLAYED : A_PLAYED);
          break;
     }
     case J_PLAYER:
          break;
     }
     if (U.sel >= U.nrows || U.sel < 0 || (U.nrows && U.rows[U.sel].header)) {
          int s = first_selectable(U.sel < 0 ? 0 : U.sel, 1);
          if (s < 0) s = first_selectable(U.nrows - 1, -1);
          U.sel = s < 0 ? 0 : s;
     }
}

static void go(enum scr s, int sel)
{
     U.scr = s;
     U.sel = sel;
     U.top = 0;
     build_rows();
     U.dirty = 1;
}

/* ------------------------------------------------------------ navigation */

static void load_home(void)
{
     struct job j = { .kind = JB_HOME };
     post_browse(j, "Loading\xe2\x80\xa6");
}

static void open_item(struct jf_item *it)
{
     if (U.depth >= MAX_DEPTH && (it->type == JF_VIEW || it->type == JF_FOLDER ||
         it->type == JF_BOXSET || it->type == JF_SERIES || it->type == JF_SEASON ||
         it->type == JF_PLAYLIST))
          return;
     struct job j = { .kind = JB_LIST };
     struct level *L = &U.lv[U.depth];
     memset(L, 0, sizeof *L);
     snprintf(L->title, sizeof L->title, "%s", it->name);
     switch (it->type) {
     case JF_SERIES:
          L->lkind = LK_SEASONS;
          snprintf(L->id, sizeof L->id, "%s", it->id);
          break;
     case JF_SEASON:
          L->lkind = LK_EPISODES;
          snprintf(L->id, sizeof L->id, "%s", it->series_id);
          snprintf(L->id2, sizeof L->id2, "%s", it->id);
          if (it->series_name[0])
               snprintf(L->title, sizeof L->title, "%s  \xc2\xb7  %s", it->series_name, it->name);
          break;
     case JF_VIEW: case JF_FOLDER: case JF_BOXSET: case JF_PLAYLIST: case JF_OTHER: case JF_ALBUM:
          L->lkind = LK_CHILDREN;
          snprintf(L->id, sizeof L->id, "%s", it->id);
          break;
     default: {
          /* something playable: the detail screen */
          struct job d = { .kind = JB_DETAIL };
          snprintf(d.id, sizeof d.id, "%s", it->id);
          post_browse(d, "Loading\xe2\x80\xa6");
          return;
     }
     }
     j.lkind = L->lkind;
     snprintf(j.id, sizeof j.id, "%s", L->id);
     snprintf(j.id2, sizeof j.id2, "%s", L->id2);
     j.start = 0;
     j.flag = 1;              /* a new level */
     post_browse(j, "Loading\xe2\x80\xa6");
}

/* ------------------------------------------------------------- playback */

static void player_send(const char *fmt, ...)
{
     if (U.to_fd < 0) return;
     char buf[4400];
     va_list ap;
     va_start(ap, fmt);
     int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
     va_end(ap);
     if (n < 0) return;
     if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
     buf[n++] = '\n';
     if (write(U.to_fd, buf, (size_t)n) != n)
          fprintf(stderr, "jellyfin: player write: %s\n", strerror(errno));
}

/* ---- subtitles */

static const struct jf_stream *stream_of(int index)
{
     if (!U.play) return NULL;
     for (int i = 0; i < U.play->nstreams; i++)
          if (U.play->streams[i].index == index) return &U.play->streams[i];
     return NULL;
}

/* the track to show: text tracks are fetched and drawn here; an image
 * track was burned in by the server (it is in the picture already) */
static void choose_subs(int index)
{
     jf_subs_free(&U.subs);
     U.cue_shown = NULL;
     U.sub_index = index;
     if (U.play) U.play->subtitle_index = index;      /* reported to the server */
     const struct jf_stream *st = index >= 0 ? stream_of(index) : NULL;
     if (!st || !st->is_text) return;
     struct job j = { .kind = JB_SUBS, .start = index };
     snprintf(j.id, sizeof j.id, "%s", U.playing.id);
     snprintf(j.arg, sizeof j.arg, "%s", U.play->media_source_id);
     post(j);
}

static double cur_pos(double now)
{
     if (U.seek_pending) return U.seek_target_ms;
     return U.pos_ms + (U.pstate == 1 && U.pos_at > 0 ? (now - U.pos_at) * 1000.0 : 0);
}

static void report(int event, double now)
{
     if (!U.play) return;
     struct job j = { .kind = JB_REPORT, .event = event, .paused = U.pstate == 2 };
     snprintf(j.id, sizeof j.id, "%s", U.playing.id);
     j.ticks = (int64_t)(U.pos_ms * TICKS_PER_MS);
     j.play = malloc(sizeof *j.play);
     if (!j.play) return;
     *j.play = *U.play;
     post(j);
     U.next_report = now + 10;
}

/* the HMP player needs the MIME type next to the URL (word 4 of its URI
 * struct; findings 07) */
static const char *play_mime(const struct jf_play *p)
{
     if (p->method == JF_TRANSCODE) return "application/x-mpegURL";   /* HLS */
     const char *c = p->container;
     if (strstr(c, "webm")) return "video/webm";
     if (strstr(c, "mkv") || strstr(c, "matroska")) return "video/x-matroska";
     if (strstr(c, "mp4") || strstr(c, "mov") || strstr(c, "m4v")) return "video/mp4";
     if (strstr(c, "ts")) return "video/mp2t";
     return "video/x-matroska";
}

static int spawn_player(const char *url, const char *mime, int64_t start_ms)
{
     char mime_arg[64];
     snprintf(mime_arg, sizeof mime_arg, "--mime=%s", mime);
     int in[2], out[2];
     if (pipe2(in, O_CLOEXEC) != 0) return -1;
     if (pipe2(out, O_CLOEXEC) != 0) { close(in[0]); close(in[1]); return -1; }
     /* vibe-play switches the source to DMP and back to this input itself;
      * kept here too, for when it dies without doing so */
     if (biz_current_input(U.prev_input, sizeof U.prev_input) != 0) U.prev_input[0] = 0;
     pid_t pid = fork();
     if (pid < 0) {
          close(in[0]); close(in[1]); close(out[0]); close(out[1]);
          return -1;
     }
     if (pid == 0) {
          /* only async-signal-safe calls between fork and exec (threads) */
          dup2(in[0], 0);
          dup2(out[1], 1);
          int devnull = open("/tmp/vibe-play.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
          if (devnull >= 0) dup2(devnull, 2);
          /* no DirectFB or fusion descriptors in the child */
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
     player_send("open %s %lld", url, (long long)start_ms);
     return 0;
}

static void player_reap(void)
{
     if (U.to_fd >= 0) { close(U.to_fd); U.to_fd = -1; }
     if (U.from_fd >= 0) { close(U.from_fd); U.from_fd = -1; }
     if (U.pid > 0) {
          int st = 0, clean = 0;
          /* it stops on stdin closing (HMP teardown, then the switch back
           * to the input): give it a moment, then insist */
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
          /* killed or crashed: the window is left without a picture, and
           * switching to the input alone does not bring it back (video
           * info stayed 0,0,0,0). Going through DMP and back rebuilds the
           * path, as a clean stop does (verified on the screen,
           * 2026-11-26). The dead player left the source on DMP, where a
           * switch to DMP does nothing: first the input (ends that DMP
           * session), then DMP, then the input. Blocks ~5 s, only here. */
          if (!clean && U.prev_input[0] && strcmp(U.prev_input, "DMP")) {
               fprintf(stderr, "jellyfin: player died (status 0x%x), %s -> DMP -> %s\n",
                       st, U.prev_input, U.prev_input);
               biz_switch_input(U.prev_input);
               sleep(2);
               biz_switch_input("DMP");
               sleep(3);
               fprintf(stderr, "jellyfin: back to %s -> %d\n", U.prev_input, biz_switch_input(U.prev_input));
          }
     }
}

static void stop_playback(double now, int ended)
{
     if (U.scr != J_PLAYER && !U.pid) return;
     if (U.play) {
          if (ended) U.pos_ms = U.dur_ms;
          report(2, now);
          if (ended) {               /* watched, whatever the server makes of the position */
               struct job pj = { .kind = JB_PLAYED, .flag = 1 };
               snprintf(pj.id, sizeof pj.id, "%s", U.playing.id);
               post(pj);
          }
          struct job j = { .kind = JB_STOP_TC };
          j.play = malloc(sizeof *j.play);
          if (j.play) { *j.play = *U.play; post(j); }
     }
     player_send("stop");
     player_reap();
     jf_subs_free(&U.subs);
     U.cue_shown = NULL;
     U.sub_index = -1;
     U.menu_on = 0;
     free(U.play);
     U.play = NULL;
     U.scr = U.play_back;
     build_rows();
     U.dirty = 1;
     /* the lists show progress: refresh what we came back to */
     if (U.scr == J_HOME) load_home();
     else if (U.scr == J_DETAIL && U.have_detail) {
          struct job d = { .kind = JB_DETAIL };
          snprintf(d.id, sizeof d.id, "%s", U.detail.id);
          post_browse(d, "");
          U.busy = 0;
     }
}

static void start_playback(struct jf_item *it, int64_t start_ticks, double now)
{
     U.playing = *it;
     U.playing.overview = NULL;
     struct job j = { .kind = JB_PLAYINFO, .ticks = start_ticks, .audio = -1, .sub = JF_SUB_DEFAULT };
     snprintf(j.id, sizeof j.id, "%s", it->id);
     post_browse(j, "Starting\xe2\x80\xa6");
}

static void switch_audio(int index, double now);

static void on_player_line(char *l, double now)
{
     if (!strncmp(l, "state ", 6)) {
          const char *s = l + 6;
          if (!strcmp(s, "playing")) {
               U.pstate = 1;
               if (U.want_audio >= 0) {
                    int a = U.want_audio;
                    U.want_audio = -1;
                    switch_audio(a, now);
               }
               if (!U.reported_start) { U.reported_start = 1; report(0, now); }
          } else if (!strcmp(s, "paused")) {
               U.pstate = 2;
               report(1, now);
          }
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
     } else if (!strcmp(l, "exit")) {
          /* the child ends on its own (failure or the end) */
     }
     fprintf(stderr, "vibe-play: %s\n", l);
}

static void read_player(double now)
{
     if (U.from_fd < 0) return;
     for (;;) {
          ssize_t n = read(U.from_fd, U.rbuf + U.rlen, sizeof U.rbuf - 1 - (size_t)U.rlen);
          if (n == 0) {                    /* the child is gone */
               if (U.scr == J_PLAYER) {
                    if (U.pstate == 0) set_err("The player stopped before the video started", now);
                    stop_playback(now, 0);
               }
               return;
          }
          if (n < 0) return;               /* EAGAIN */
          U.rlen += (int)n;
          U.rbuf[U.rlen] = 0;
          char *nl;
          while ((nl = strchr(U.rbuf, '\n'))) {
               *nl = 0;
               on_player_line(U.rbuf, now);
               if (U.from_fd < 0) return;   /* stopped while handling */
               int rest = U.rlen - (int)(nl + 1 - U.rbuf);
               memmove(U.rbuf, nl + 1, (size_t)rest);
               U.rlen = rest;
               U.rbuf[U.rlen] = 0;
          }
          if (U.rlen >= (int)sizeof U.rbuf - 1) U.rlen = 0;   /* overlong line */
     }
}

/* ---------------------------------------------------------------- open */

void jfui_open(const struct jfui_host *h, double now)
{
     U.h = *h;
     U.on = 1;
     U.dirty = 1;
     if (!U.inited) {
          U.inited = 1;
          U.to_fd = U.from_fd = -1;
          jf_session_load(&S, STATE_FILE);
          jf_session_init(&S);
          jfimg_session(&S);
          snprintf(U.server_name, sizeof U.server_name, "%s", S.server_name);
          snprintf(U.user_name, sizeof U.user_name, "%s", S.user_name);
          U.have_server = S.server[0] != 0;
          U.signed_in = S.token[0] != 0;
          pthread_t t;
          pthread_attr_t at;
          pthread_attr_init(&at);
          pthread_attr_setstacksize(&at, 256 * 1024);
          if (pthread_create(&t, &at, worker, NULL) == 0) {
               pthread_detach(t);
               worker_up = 1;
          }
          pthread_attr_destroy(&at);
          (void)wake_fd;
     }
     if (!worker_up) { set_err("could not start the network thread", now); return; }
     if (U.signed_in) {
          go(J_HOME, U.home_sel);
          load_home();
     } else if (U.have_server) {
          go(J_SIGNIN, 0);
          struct job j = { .kind = JB_QC_START };
          post_browse(j, "Asking the server for a code\xe2\x80\xa6");
     } else {
          go(J_SERVERS, 0);
          struct job j = { .kind = JB_DISCOVER };
          post_browse(j, "Looking for Jellyfin servers\xe2\x80\xa6");
     }
}

void jfui_close(double now)
{
     if (U.scr == J_PLAYER || U.pid) stop_playback(now, 0);
     U.on = 0;
     U.gen++;                 /* whatever is still loading is not wanted */
     U.busy = 0;
}

/* ---------------------------------------------------------------- tick */

static void apply(struct result *r, double now)
{
     int stale = r->gen && r->gen != U.gen;
     snprintf(U.server_name, sizeof U.server_name, "%s", r->server_name);
     snprintf(U.user_name, sizeof U.user_name, "%s", r->user_name);
     if (r->kind == JB_REPORT || r->kind == JB_STOP_TC) {
          if (r->rc) fprintf(stderr, "jellyfin: report: %s\n", r->err);
          return;
     }
     if (stale) return;
     U.busy = 0;
     U.dirty = 1;
     if (r->rc != 0 && r->kind != JB_QC_POLL) {
          set_err(r->err[0] ? r->err : "request failed", now);
          /* a dead token: back to sign-in */
          if (strstr(r->err, "HTTP 401")) {
               U.signed_in = 0;
               go(J_SIGNIN, 0);
               struct job j = { .kind = JB_QC_START };
               post_browse(j, "Asking the server for a code\xe2\x80\xa6");
          }
          return;
     }
     switch (r->kind) {
     case JB_DISCOVER:
          U.nservers = r->nservers;
          memcpy(U.servers, r->servers, sizeof U.servers);
          if (!U.nservers) set_err("No Jellyfin server answered on this network", now);
          go(J_SERVERS, 0);
          break;
     case JB_CONNECT: {
          U.have_server = 1;
          go(J_SIGNIN, 0);
          struct job j = { .kind = JB_QC_START };
          post_browse(j, "Asking the server for a code\xe2\x80\xa6");
          break;
     }
     case JB_QC_START:
          snprintf(U.qc_code, sizeof U.qc_code, "%s", r->code);
          snprintf(U.qc_secret, sizeof U.qc_secret, "%s", r->secret);
          U.qc_next = now + 3;
          U.qc_until = now + 10 * 60;
          go(J_SIGNIN, 0);
          break;
     case JB_QC_POLL:
          if (r->rc != 0) {           /* expired or refused: a new code */
               U.qc_code[0] = 0;
               struct job j = { .kind = JB_QC_START };
               post_browse(j, "Getting a new code\xe2\x80\xa6");
          } else if (r->start == 1) {
               U.signed_in = 1;
               U.qc_code[0] = 0;
               go(J_HOME, 0);
               load_home();
          } else {
               U.qc_next = now + 3;
          }
          break;
     case JB_HOME:
          /* a reload keeps the focus; the first load (only the Account
           * shelf was there) starts at the top */
          if (U.scr == J_HOME && (U.home[0].n || U.home[1].n || U.home[2].n)) save_cards();
          else if (!(U.home[0].n || U.home[1].n || U.home[2].n)) { U.hcur = SH_MEDIA; U.hvtop = 0; }
          for (int s = 0; s < 3; s++) { jf_list_free(&U.home[s]); U.home[s] = r->lists[s]; r->lists[s] = (struct jf_list){ 0 }; }
          if (U.scr == J_HOME) go(J_HOME, 0);
          break;
     case JB_LIST: {
          struct level *L;
          if (r->start == 0 && (U.scr != J_LIST || !U.lv[U.depth - 1].loading_more)) {
               /* a new level (or a reload of the top one) */
               save_cards();
               L = &U.lv[U.depth];
               jf_list_free(&L->l);
               L->l = r->lists[0];
               r->lists[0] = (struct jf_list){ 0 };
               U.depth++;
               go(J_LIST, 0);
          } else {
               /* the next page of the top level */
               L = &U.lv[U.depth - 1];
               L->loading_more = 0;
               struct jf_list *m = &r->lists[0];
               struct jf_item *n = realloc(L->l.items, (size_t)(L->l.n + m->n) * sizeof *n);
               if (n) {
                    memcpy(n + L->l.n, m->items, (size_t)m->n * sizeof *n);
                    L->l.items = n;
                    L->l.n += m->n;
                    free(m->items);       /* overviews moved, not freed */
                    *m = (struct jf_list){ 0 };
               }
               save_cards();
               build_rows();
          }
          break;
     }
     case JB_DETAIL:
          free(U.detail.overview);
          U.detail = r->item;
          r->item.overview = NULL;
          U.have_detail = 1;
          if (U.scr != J_DETAIL) {
               save_cards();
               U.detail_back = U.scr;
               go(J_DETAIL, 0);
          } else {
               build_rows();
          }
          break;
     case JB_PLAYINFO: {
          free(U.play);
          U.play = r->play;
          r->play = NULL;
          if (U.scr != J_PLAYER) U.play_back = U.scr;     /* (a track switch restarts from the player) */
          U.pstate = 0;
          U.reported_start = 0;
          U.pos_ms = 0;
          U.seek_pending = 0;
          U.dur_ms = (double)(U.play->runtime_ticks ? U.play->runtime_ticks : U.playing.runtime_ticks) / TICKS_PER_MS;
          /* every method starts at 0 and seeks (the player covers the
           * video until it lands): the server's HLS conversion starts at
           * 0:00 whatever StartTimeTicks said (an audio-track switch
           * restarted at 0:00, owner) */
          int64_t start_ms = U.playing.position_ticks / TICKS_PER_MS;
          if (r->start == 2) start_ms = 0;
          if (spawn_player(U.play->url, play_mime(U.play), start_ms) != 0) {
               set_err("could not start the player", now);
               free(U.play);
               U.play = NULL;
               break;
          }
          U.pos_ms = (double)start_ms;
          U.pos_at = 0;
          U.pos_moved = 0;
          U.scr = J_PLAYER;
          U.menu_on = 0;
          choose_subs(U.play->subtitle_index);     /* the server's default for this user */
          /* English audio by default (owner): if the default track is in
           * another language and the file has an English one, switch to it
           * once playing (direct play: the player switches in place) */
          U.want_audio = -1;
          if (U.play->method != JF_TRANSCODE) {
               /* direct play: the TV's player takes the file's default
                * track (flagged default, else the first audio), whatever
                * the server's DefaultAudioStreamIndex says (seen: the
                * server said English, the TV played the Portuguese default) */
               int file_def = -1, first = -1, eng = -1;
               for (int i = 0; i < U.play->nstreams; i++) {
                    const struct jf_stream *st = &U.play->streams[i];
                    if (strcmp(st->type, "Audio")) continue;
                    if (first < 0) first = st->index;
                    if (st->is_default && file_def < 0) file_def = st->index;
                    if (eng < 0 && !strcmp(st->lang, "eng")) eng = st->index;
               }
               if (file_def < 0) file_def = first;
               int want = U.play->audio_index;
               const struct jf_stream *ws = stream_of(want);
               if ((!ws || strcmp(ws->lang, "eng")) && eng >= 0) want = eng;
               U.play->audio_index = file_def;          /* what plays now */
               if (want >= 0 && want != file_def) U.want_audio = want;
          }
          U.osd_until = now + 5;
          U.next_report = now + 10;
          U.dirty = 1;
          break;
     }
     case JB_SUBS:
          if (U.scr == J_PLAYER && U.play && r->start == U.sub_index) {
               jf_subs_free(&U.subs);
               U.subs = r->subs;
               r->subs = (struct jf_subs){ 0 };
               U.cue_shown = NULL;
               U.dirty = 1;
          }
          break;
     case JB_PLAYED:
          if (U.have_detail) {
               struct job d = { .kind = JB_DETAIL };
               snprintf(d.id, sizeof d.id, "%s", U.detail.id);
               post_browse(d, "");
          }
          break;
     case JB_LOGOUT:
          U.signed_in = 0;
          for (int s = 0; s < 3; s++) jf_list_free(&U.home[s]);
          go(J_SERVERS, 0);
          {
               struct job j = { .kind = JB_DISCOVER };
               post_browse(j, "Looking for Jellyfin servers\xe2\x80\xa6");
          }
          break;
     default:
          break;
     }
}

int jfui_tick(double now)
{
     if (!U.inited) return 0;
     for (;;) {
          struct result *r = NULL;
          pthread_mutex_lock(&q_lock);
          if (rq_n) { r = results[rq_head]; rq_head = (rq_head + 1) % QMAX; rq_n--; }
          pthread_mutex_unlock(&q_lock);
          if (!r) break;
          apply(r, now);
          for (int i = 0; i < 3; i++) jf_list_free(&r->lists[i]);
          free(r->item.overview);
          free(r->play);
          jf_subs_free(&r->subs);
          free(r);
     }
     read_player(now);
     if (U.on && U.scr == J_SIGNIN && U.qc_code[0] && now >= U.qc_next && !U.busy) {
          U.qc_next = now + 3;
          struct job j = { .kind = JB_QC_POLL, .gen = U.gen };
          snprintf(j.arg, sizeof j.arg, "%s", U.qc_secret);
          post(j);
     }
     if (U.scr == J_PLAYER) {
          if (U.seek_pending && now >= U.seek_at) {
               U.seek_pending = 0;
               player_send("seek %.0f", U.seek_target_ms);
               U.pos_ms = U.seek_target_ms;
               U.pos_at = now;
               report(1, now);
          }
          if (U.pstate == 1 && now >= U.next_report) report(1, now);
          /* the player says "ended" on HMP's video end-of-play event (7);
           * in case it never comes, its clock running past the end will do */
          if (U.pstate == 1 && U.dur_ms > 0 && U.pos_ms > U.dur_ms + 3000) {
               stop_playback(now, 1);
               return U.on;
          }
          /* or frozen on the last frame: near the end and not moving */
          if (U.pstate == 1 && U.dur_ms > 0 && U.pos_ms > U.dur_ms - 15000 &&
              U.pos_moved > 0 && now - U.pos_moved > 6) {
               stop_playback(now, 1);
               return U.on;
          }
          if (U.subs.n && U.pstate) {
               const struct jf_cue *cue = jf_subs_at(&U.subs, (int)cur_pos(now));
               if (cue != U.cue_shown) { U.cue_shown = cue; U.dirty = 1; }
          }
          /* the OSD clock, and its disappearance */
          if (U.osd_until && now >= U.osd_until) { U.osd_until = 0; U.dirty = 1; }
          else if (U.osd_until > now && now >= U.next_redraw) { U.next_redraw = now + 1; U.dirty = 1; }
     }
     if (U.err_until && now >= U.err_until) { U.err_until = 0; U.dirty = 1; }
     if (jfimg_changed() && (card_screen() || U.scr == J_DETAIL)) U.dirty = 1;
     int d = U.dirty;
     U.dirty = 0;
     return d && U.on;
}

/* ---------------------------------------------------------------- keys */

static void move(int d)
{
     int s = U.sel;
     for (;;) {
          s += d;
          if (s < 0 || s >= U.nrows) return;
          if (!U.rows[s].header) break;
     }
     U.sel = s;
     U.dirty = 1;
     /* near the end of a paged list: the next page */
     if (U.scr == J_LIST) {
          struct level *L = &U.lv[U.depth - 1];
          if (L->lkind == LK_CHILDREN && !L->loading_more && L->l.n < L->l.total && U.sel >= L->l.n - 10) {
               L->loading_more = 1;
               struct job j = { .kind = JB_LIST, .lkind = LK_CHILDREN, .start = L->l.n };
               snprintf(j.id, sizeof j.id, "%s", L->id);
               j.gen = U.gen;           /* not a new browse: same gen */
               post(j);
          }
     }
}

static int key_back(double now)
{
     switch (U.scr) {
     case J_DETAIL:
          U.scr = U.detail_back;
          if (U.scr == J_LIST) { struct level *L = &U.lv[U.depth - 1]; go(J_LIST, L->sel); U.top = L->top; }
          else go(J_HOME, U.home_sel);
          return 1;
     case J_LIST:
          U.depth--;
          jf_list_free(&U.lv[U.depth].l);
          if (U.depth > 0) { struct level *L = &U.lv[U.depth - 1]; go(J_LIST, L->sel); U.top = L->top; }
          else go(J_HOME, U.home_sel);
          U.gen++;                   /* drop anything still loading */
          U.busy = 0;
          return 1;
     default:
          return 0;                  /* close the panel */
     }
}

static void seek_by(double d_ms, double now)
{
     if (!U.seek_pending) U.seek_target_ms = U.pos_ms;
     U.seek_target_ms += d_ms;
     if (U.seek_target_ms < 0) U.seek_target_ms = 0;
     if (U.dur_ms > 0 && U.seek_target_ms > U.dur_ms - 2000) U.seek_target_ms = U.dur_ms - 2000;
     U.seek_pending = 1;
     U.seek_at = now + 0.7;          /* presses in a row become one seek */
     U.osd_until = now + 5;
     U.dirty = 1;
}

/* the track menu: the audio tracks (if more than one), then subtitles
 * (Off, then every track). Entries: stream indexes; MENU_SUB_OFF is Off,
 * MENU_HEAD_* are section titles (not selectable) */
#define MENU_SUB_OFF   (-1)
#define MENU_HEAD_AUD  (-100)
#define MENU_HEAD_SUB  (-101)
static int menu_items(int *idx, int max)
{
     int n = 0, na = 0;
     for (int i = 0; U.play && i < U.play->nstreams; i++)
          if (!strcmp(U.play->streams[i].type, "Audio")) na++;
     if (na > 1) {
          idx[n++] = MENU_HEAD_AUD;
          for (int i = 0; U.play && i < U.play->nstreams && n < max; i++)
               if (!strcmp(U.play->streams[i].type, "Audio")) idx[n++] = U.play->streams[i].index;
     }
     idx[n++] = MENU_HEAD_SUB;
     idx[n++] = MENU_SUB_OFF;
     for (int i = 0; U.play && i < U.play->nstreams && n < max; i++)
          if (!strcmp(U.play->streams[i].type, "Subtitle")) idx[n++] = U.play->streams[i].index;
     return n;
}

static int menu_head(int v) { return v == MENU_HEAD_AUD || v == MENU_HEAD_SUB; }

/* another audio track: the TV's player switches (HMPSelectTrack, via
 * vibe-play's "track audio K", K = its place among the file's audio
 * tracks). A direct-played file has them all; the server's remux had
 * the default only, whatever was asked (measured), so a conversion
 * restarts as below */
static void switch_audio(int index, double now)
{
     if (!U.play || index == U.play->audio_index) return;
     if (U.play->method != JF_TRANSCODE) {
          int k = 0;
          for (int i = 0; i < U.play->nstreams; i++) {
               if (strcmp(U.play->streams[i].type, "Audio")) continue;
               if (U.play->streams[i].index == index) break;
               k++;
          }
          player_send("track audio %d", k);
          U.play->audio_index = index;          /* reported to the server */
          U.menu_on = 0;
          U.osd_until = now + 3;
          report(1, now);
          U.dirty = 1;
          return;
     }
     int64_t ticks = (int64_t)(cur_pos(now) * TICKS_PER_MS);
     int sub = U.sub_index;
     report(1, now);
     player_send("stop keep");
     player_reap();
     if (U.play->method == JF_TRANSCODE) {
          struct job tj = { .kind = JB_STOP_TC };
          tj.play = malloc(sizeof *tj.play);
          if (tj.play) { *tj.play = *U.play; post(tj); }
     }
     jf_subs_free(&U.subs);
     U.cue_shown = NULL;
     U.pstate = 0;
     U.menu_on = 0;
     U.playing.position_ticks = ticks;
     struct job j = { .kind = JB_PLAYINFO, .ticks = ticks, .audio = index, .sub = sub < 0 ? -1 : sub };
     snprintf(j.id, sizeof j.id, "%s", U.playing.id);
     post_browse(j, "");
     U.dirty = 1;
}

static void key_menu(int k, int repeat, double now)
{
     int idx[JF_MAX_STREAMS + 3], n = menu_items(idx, JF_MAX_STREAMS + 3);
     switch (k) {
     case DIKS_CURSOR_UP: {
          int s = U.menu_sel - 1;
          while (s >= 0 && menu_head(idx[s])) s--;
          if (s >= 0) U.menu_sel = s;
          break;
     }
     case DIKS_CURSOR_DOWN: {
          int s = U.menu_sel + 1;
          while (s < n && menu_head(idx[s])) s++;
          if (s < n) U.menu_sel = s;
          break;
     }
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: {
          if (repeat) return;
          const struct jf_stream *st = stream_of(idx[U.menu_sel]);
          if (st && !strcmp(st->type, "Audio")) { switch_audio(st->index, now); return; }
          if (idx[U.menu_sel] >= 0 && st && !st->is_text && U.sub_index != idx[U.menu_sel]) {
               set_err("Picture-based subtitles are not supported yet", now);
               return;
          }
          choose_subs(idx[U.menu_sel]);
          U.menu_on = 0;
          break;
     }
     case DIKS_BACK: case DIKS_EXIT: case DIKS_SUBTITLE: case KEY_CC:
          if (repeat) return;
          U.menu_on = 0;
          break;
     default:
          return;
     }
     U.dirty = 1;
}

static void key_player(int k, int repeat, double now)
{
     if (U.menu_on) { key_menu(k, repeat, now); return; }
     switch (k) {
     case DIKS_CURSOR_DOWN: case DIKS_SUBTITLE: case KEY_CC: {
          if (repeat || U.pstate == 0) return;
          int idx[JF_MAX_STREAMS + 3], n = menu_items(idx, JF_MAX_STREAMS + 3);
          U.menu_sel = 0;
          for (int i = 0; i < n; i++)
               if (idx[i] == U.sub_index && (i == 0 || idx[i] >= 0 || idx[i] == MENU_SUB_OFF) &&
                   (idx[i] < 0 || strcmp(stream_of(idx[i])->type, "Audio"))) U.menu_sel = i;
          while (U.menu_sel < n && menu_head(idx[U.menu_sel])) U.menu_sel++;
          U.menu_on = 1;
          U.dirty = 1;
          return;
     }
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: case DIKS_PLAYPAUSE:
     case DIKS_PAUSE: case DIKS_PLAY:
          if (repeat) return;
          if (U.pstate == 1 && k != DIKS_PLAY) player_send("pause");
          else if (U.pstate == 2 && k != DIKS_PAUSE) player_send("resume");
          U.osd_until = now + 5;
          U.dirty = 1;
          break;
     case DIKS_CURSOR_LEFT: case DIKS_REWIND:
          seek_by(repeat ? -30000 : -10000, now);
          break;
     case DIKS_CURSOR_RIGHT: case DIKS_FASTFORWARD:
          seek_by(30000, now);
          break;
     case DIKS_CURSOR_UP: case DIKS_INFO:
          U.osd_until = U.osd_until > now ? 0 : now + 8;
          U.dirty = 1;
          break;
     case DIKS_BACK: case DIKS_EXIT: case DIKS_STOP:
          if (repeat) return;
          stop_playback(now, 0);
          break;
     }
}

/* a list near its loaded end: the next page */
static void more_if_needed(void)
{
     if (U.scr != J_LIST || !U.nsh) return;
     struct level *L = &U.lv[U.depth - 1];
     struct shelf *h = &U.sh[0];
     if (L->lkind == LK_CHILDREN && !L->loading_more && L->l.n < L->l.total &&
         h->sel >= L->l.n - 3 * per_line(h)) {
          L->loading_more = 1;
          struct job j = { .kind = JB_LIST, .lkind = LK_CHILDREN, .start = L->l.n };
          snprintf(j.id, sizeof j.id, "%s", L->id);
          j.gen = U.gen;
          post(j);
     }
}

static int card_key(int k, int repeat, double now)
{
     if (!U.nsh) return k == DIKS_BACK || k == DIKS_EXIT ? key_back(now) : 1;
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
          return key_back(now);
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: case DIKS_PLAY: case DIKS_PLAYPAUSE:
          if (repeat || U.busy) return 1;
          if (h->kind == SH_ACCOUNT && !h->items) {
               /* signing out revokes the token on the server, and a test
                * runs with a copy of the UI's session: never from a test */
               if (getenv("JFUI_TEST")) { set_err("sign-out is off in tests", now); return 1; }
               struct job j = { .kind = JB_LOGOUT };
               post_browse(j, "Signing out…");
          } else if (h->items && h->sel < h->n) {
               save_cards();
               open_item(&h->items[h->sel]);
          }
          return 1;
     default:
          return 1;
     }
     if (step && h->grid) {
          int pl = per_line(h), t = h->sel + step * pl;
          if (t < 0) t = step < 0 && h->sel >= pl ? h->sel % pl : 0;
          if (t > h->n - 1) t = (h->sel / pl + 1) * pl < h->n ? h->n - 1 : h->sel;
          h->sel = t;
     } else if (step) {
          int c = U.cur + (step < 0 ? -1 : 1);
          if (c >= 0 && c < U.nsh) U.cur = c;
     }
     cards_follow();
     more_if_needed();
     U.dirty = 1;
     return 1;
}

int jfui_key(int k, int repeat, double now)
{
     if (U.scr == J_PLAYER) { key_player(k, repeat, now); return 1; }
     if (card_screen()) return card_key(k, repeat, now);
     if (U.scr == J_DETAIL) {          /* the actions are a row of buttons */
          if (k == DIKS_CURSOR_LEFT) { move(-1); return 1; }
          if (k == DIKS_CURSOR_RIGHT) { move(1); return 1; }
          if (k == DIKS_CURSOR_UP || k == DIKS_CURSOR_DOWN) return 1;
     }
     switch (k) {
     case DIKS_CURSOR_UP:   move(-1); return 1;
     case DIKS_CURSOR_DOWN: move(1); return 1;
     case DIKS_PAGE_UP: case DIKS_CURSOR_LEFT:  for (int i = 0; i < 8; i++) move(-1); return 1;
     case DIKS_PAGE_DOWN: case DIKS_CURSOR_RIGHT: for (int i = 0; i < 8; i++) move(1); return 1;
     case DIKS_BACK: case DIKS_EXIT:
          if (repeat) return 1;
          return key_back(now);
     case DIKS_OK: case DIKS_RETURN: case DIKS_SELECT: case DIKS_PLAY: case DIKS_PLAYPAUSE:
          break;
     default:
          return 1;
     }
     if (repeat || U.busy || U.sel < 0 || U.sel >= U.nrows) return 1;
     struct row *r = &U.rows[U.sel];
     switch (U.scr) {
     case J_SERVERS:
          if (r->action < 0) {
               struct job j = { .kind = JB_DISCOVER };
               post_browse(j, "Looking for Jellyfin servers\xe2\x80\xa6");
          } else {
               struct job j = { .kind = JB_CONNECT };
               snprintf(j.arg, sizeof j.arg, "%s", U.servers[r->action].address);
               post_browse(j, "Connecting\xe2\x80\xa6");
          }
          break;
     case J_SIGNIN: {
          U.qc_code[0] = 0;
          go(J_SERVERS, 0);
          struct job j = { .kind = JB_DISCOVER };
          post_browse(j, "Looking for Jellyfin servers\xe2\x80\xa6");
          break;
     }
     case J_HOME:
          if (r->action < 0) {
               struct job j = { .kind = JB_LOGOUT };
               post_browse(j, "Signing out\xe2\x80\xa6");
          } else if (r->it) {
               U.home_sel = U.sel;
               /* episodes and movies from the top rows play from the detail */
               open_item(r->it);
          }
          break;
     case J_LIST:
          if (r->it) open_item(r->it);
          break;
     case J_DETAIL:
          switch (r->action) {
          case A_PLAY: start_playback(r->it, 0, now); break;
          case A_RESUME: start_playback(r->it, r->it->position_ticks, now); break;
          case A_RESTART: {
               struct jf_item t = *r->it;
               t.position_ticks = 0;
               start_playback(&t, 0, now);
               break;
          }
          case A_PLAYED: case A_UNPLAYED: {
               struct job j = { .kind = JB_PLAYED, .flag = r->action == A_PLAYED };
               snprintf(j.id, sizeof j.id, "%s", r->it->id);
               post_browse(j, "");
               U.busy = 0;
               break;
          }
          }
          break;
     case J_PLAYER:
          break;
     }
     return 1;
}

/* ------------------------------------------------------------- drawing */

#define P_X   120
#define P_TOP 220
#define ROW_H 84
#define ROW_W 1100

static void text(IDirectFBFont *f, int x, int y, int h, uint32_t col, const char *s, int align)
{
     U.h.text(U.h.ctx, f, x, y, h, col, s, align);
}

static void draw_rows(struct canvas *c, int y0, int y1)
{
     int vis = (y1 - y0) / ROW_H;
     if (vis < 1) vis = 1;
     if (U.sel < U.top) U.top = U.sel;
     if (U.sel >= U.top + vis) U.top = U.sel - vis + 1;
     /* keep a section header with its first row */
     if (U.top > 0 && U.rows[U.top - 1].header && U.sel == U.top) U.top--;
     char sub[160];
     for (int i = U.top; i < U.nrows && i < U.top + vis; i++) {
          struct row *r = &U.rows[i];
          int y = y0 + (i - U.top) * ROW_H;
          if (r->header) {
               if (!c) text(U.h.f_tiny, P_X, y + 36, 40, K_TEXT3, r->title, -1);
               continue;
          }
          int foc = i == U.sel;
          if (c) {
               if (foc) U.h.focus(c, P_X - 24, y + 6, ROW_W, ROW_H - 12, 20);
          } else {
               sub[0] = 0;
               if (r->it && U.scr != J_DETAIL) item_sub(r->it, sub, sizeof sub);
               text(sub[0] ? U.h.f_bold : U.h.f_item, P_X, y + 6, sub[0] ? 44 : ROW_H - 12,
                    K_TEXT, r->title, -1);
               if (sub[0])
                    text(U.h.f_small, P_X, y + 44, 34, foc ? 0xe6ffffff : K_TEXT2, sub, -1);
          }
     }
     if (U.nrows > vis && !c) {
          char pos[32];
          snprintf(pos, sizeof pos, "%d / %d", U.sel + 1, U.nrows);
          text(U.h.f_tiny, P_X + ROW_W - 40, y1 + 8, 30, K_TEXT3, pos, 1);
     }
}

/* the image a card shows, as Jellyfin's web client picks it: 16:9 cards
 * prefer a Thumb (for episodes the series' Thumb), then the item's own
 * Primary (an episode still, a library tile), then a backdrop; posters are
 * the Primary (a season without one: the series') */
static int card_image(const struct jf_item *it, int land, const char **id, const char **kind, const char **tag)
{
     *kind = "Primary"; *id = it->id; *tag = it->primary_tag;
     if (!land) {
          if (it->primary_tag[0]) return 1;
          if (it->series_id[0] && it->series_primary_tag[0]) { *id = it->series_id; *tag = it->series_primary_tag; return 1; }
          return 0;
     }
     if (it->type == JF_VIEW) return it->primary_tag[0] != 0;
     if (it->thumb_tag[0]) { *kind = "Thumb"; *tag = it->thumb_tag; return 1; }
     if (it->type == JF_EPISODE && it->parent_thumb_id[0] && it->parent_thumb_tag[0]) {
          *kind = "Thumb"; *id = it->parent_thumb_id; *tag = it->parent_thumb_tag; return 1;
     }
     if (it->type != JF_MOVIE && it->type != JF_SERIES && it->primary_tag[0]) return 1;
     if (it->backdrop_tag[0]) { *kind = "Backdrop"; *tag = it->backdrop_tag; return 1; }
     if (it->parent_backdrop_id[0] && it->parent_backdrop_tag[0]) {
          *kind = "Backdrop"; *id = it->parent_backdrop_id; *tag = it->parent_backdrop_tag; return 1;
     }
     return it->primary_tag[0] != 0;
}

/* s cut to maxw pixels with an ellipsis (no font: a rough estimate) */
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
          memcpy(out + len, "\xe2\x80\xa6", 4);   /* the ellipsis and the NUL */
     }
}

/* the two lines under a card, as the web client words them */
static void card_lines(const struct jf_item *it, char *t, size_t tn, char *sub, size_t sn)
{
     sub[0] = 0;
     if (it->type == JF_EPISODE && it->series_name[0]) {
          snprintf(t, tn, "%s", it->series_name);
          if (it->season >= 0 && it->episode >= 0)
               snprintf(sub, sn, "S%d:E%d - %s", it->season, it->episode, it->name);
          else snprintf(sub, sn, "%s", it->name);
          return;
     }
     snprintf(t, tn, "%s", it->name);
     if (it->type == JF_VIEW) return;
     if (it->type == JF_MOVIE || it->type == JF_VIDEO || it->type == JF_SERIES) {
          if (it->year) snprintf(sub, sn, "%d", it->year);
          return;                     /* a series' unwatched count is a badge */
     }
     item_sub(it, sub, sn);
}

static void draw_card(struct canvas *c, const struct shelf *h, int i, int x, int y, int foc)
{
     int w = card_w(h), ch = card_h(h);
     const struct jf_item *it = h->items ? &h->items[i] : NULL;
     const char *id, *kind, *tag;
     const struct jfimg *img = NULL;
     if (it && card_image(it, h->land, &id, &kind, &tag)) img = jfimg_get(id, kind, tag, w, ch);
     if (c) {
          if (foc) gfx_rrect(c, x - 8, y - 8, w + 16, ch + 16, C_RAD + 8, 0xffffffff);
          if (img) gfx_image(c, x, y, w, ch, C_RAD, img->px, img->w, 255);
          else {
               gfx_rrect(c, x, y, w, ch, C_RAD, 0xff1c2130);
               gfx_rrect_outline(c, x, y, w, ch, C_RAD, 1, K_PANEL_EDGE);
          }
          if (it && it->position_ticks > 0 && it->runtime_ticks > 0) {
               int pw = (int)((int64_t)(w - 20) * it->position_ticks / it->runtime_ticks);
               if (pw > w - 20) pw = w - 20;
               gfx_rrect(c, x + 10, y + ch - 16, w - 20, 6, 3, 0xa0000000);
               if (pw >= 6) gfx_rrect_hgrad(c, x + 10, y + ch - 16, pw, 6, 3, K_ACCENT_L, K_ACCENT_R);
          } else if (it && (it->type == JF_SERIES || it->type == JF_SEASON) && it->unplayed > 0) {
               int bw = it->unplayed > 99 ? 58 : it->unplayed > 9 ? 46 : 36;
               gfx_rrect(c, x + w - bw - 10, y + 10, bw, 36, 18, 0xf04f7cff);   /* unwatched count */
          } else if (it && it->played && it->type != JF_VIEW) {
               gfx_rrect(c, x + w - 44, y + 10, 34, 34, 17, 0xe04f7cff);   /* watched */
          }
          return;
     }
     char t[160], sub[200], ft[200];
     if (!it) {                                   /* the Sign out card */
          text(U.h.f_item, x + w / 2, y + ch / 2 - 24, 48, K_TEXT, "Sign out", 0);
          return;
     }
     card_lines(it, t, sizeof t, sub, sizeof sub);
     if ((it->type == JF_SERIES || it->type == JF_SEASON) && it->unplayed > 0 &&
         (it->position_ticks <= 0 || it->runtime_ticks <= 0)) {
          char n[12];
          int bw = it->unplayed > 99 ? 58 : it->unplayed > 9 ? 46 : 36;
          snprintf(n, sizeof n, "%d", it->unplayed);
          text(U.h.f_tiny, x + w - 10 - bw / 2, y + 10, 36, 0xffffffff, n, 0);
     }
     if (!img) {                                  /* no picture (yet): the name on the card */
          fit(U.h.f_item, it->name, w - 32, ft, sizeof ft);
          text(U.h.f_item, x + w / 2, y + ch / 2 - 24, 48, K_TEXT2, ft, 0);
     }
     fit(U.h.f_small, t, w + 8, ft, sizeof ft);
     text(U.h.f_small, x + w / 2, y + ch + 8, 36, foc ? 0xffffffff : K_TEXT, ft, 0);
     if (sub[0]) {
          fit(U.h.f_small, sub, w + 8, ft, sizeof ft);
          text(U.h.f_small, x + w / 2, y + ch + 42, 32, K_TEXT2, ft, 0);
     }
}

static void draw_cards(struct canvas *c)
{
     if (!U.nsh) return;
     cards_follow();
     struct shelf *g = &U.sh[0];
     if (g->grid) {
          int pl = per_line(g), lh = grid_line_h(g), shown = grid_lines_shown(g);
          for (int l = U.vtop; l < U.vtop + shown; l++)
               for (int k = 0; k < pl; k++) {
                    int i = l * pl + k;
                    if (i >= g->n) break;
                    draw_card(c, g, i, C_X0 + k * (card_w(g) + C_GAP), C_TOP + (l - U.vtop) * lh, i == g->sel);
               }
          if (!c && g->n > 0) {
               char pos[32];
               struct level *L = &U.lv[U.depth - 1];
               snprintf(pos, sizeof pos, "%d / %d", g->sel + 1, L->l.total > g->n ? L->l.total : g->n);
               text(U.h.f_small, 1920 - C_X0, 80, 80, K_TEXT3, pos, 1);
          }
          return;
     }
     int y = C_TOP;
     for (int si = U.vtop; si < U.nsh; si++) {
          struct shelf *h = &U.sh[si];
          if (y + shelf_h(h) - SH_GAP > C_BOT + 40) break;
          if (!c) text(U.h.f_bold, C_X0, y, SH_TITLE - 16, si == U.cur ? K_TEXT : K_TEXT2, h->title, -1);
          int cy = y + SH_TITLE;
          for (int i = h->scroll; i < h->n; i++) {
               int x = C_X0 + (i - h->scroll) * (card_w(h) + C_GAP);
               if (x >= 1920) break;
               draw_card(c, h, i, x, cy, si == U.cur && i == h->sel);
          }
          y += shelf_h(h);
     }
}

/* a subtitle line in runs: music notes (U+2669..U+266F, missing from Noto
 * Sans) in the symbols font, the rest in f_bold. Width only if !draw. */
static int is_note(const unsigned char *p)
{
     return p[0] == 0xe2 && p[1] == 0x99 && p[2] >= 0xa9 && p[2] <= 0xaf;
}

static int cue_runs(const char *line, int x, int y, int h, int draw)
{
     IDirectFBFont *fb = U.h.f_bold, *fs = U.h.f_sym ? U.h.f_sym : U.h.f_bold;
     int w = 0;
     const unsigned char *p = (const unsigned char *)line;
     while (*p) {
          int note = is_note(p);
          const unsigned char *q = p;
          while (*q && is_note(q) == note) q += note ? 3 : 1;
          char run[512];
          size_t n = (size_t)(q - p) < sizeof run - 1 ? (size_t)(q - p) : sizeof run - 1;
          memcpy(run, p, n);
          run[n] = 0;
          IDirectFBFont *f = note ? fs : fb;
          int rw = (int)n * 20;
          if (f) f->GetStringWidth(f, run, -1, &rw);
          if (draw) text(f, x + w, y, h, 0xffffffff, run, -1);
          w += rw;
          p = q;
     }
     return w;
}

/* a cue: each line centred on a dark box, bottom of the screen (above the
 * OSD when that is up) */
static void draw_cue(struct canvas *c, const struct jf_cue *cue, int osd)
{
     int W = U.h.W, H = U.h.H;
     const char *lines[4];
     int nl = 0, lh = 56;
     char buf[1024];
     snprintf(buf, sizeof buf, "%s", cue->text);
     for (char *p = buf; p && nl < 4; ) {
          lines[nl++] = p;
          char *e = strchr(p, '\n');
          if (e) { *e = 0; p = e + 1; } else p = NULL;
     }
     int y = (osd ? H - 330 : H - 110) - nl * lh;
     for (int i = 0; i < nl; i++, y += lh) {
          int tw = cue_runs(lines[i], 0, 0, 0, 0);
          if (tw > W - 200) tw = W - 200;
          if (c) gfx_rrect(c, (W - tw) / 2 - 20, y + 2, tw + 40, lh - 4, 10, 0xb0000000);
          else cue_runs(lines[i], (W - tw) / 2, y, lh, 1);
     }
}

static void draw_menu(struct canvas *c)
{
     int idx[JF_MAX_STREAMS + 3], n = menu_items(idx, JF_MAX_STREAMS + 3);
     int W = U.h.W, H = U.h.H, mw = 760, row = 72;
     int show = n > 10 ? 10 : n, top = U.menu_sel >= show ? U.menu_sel - show + 1 : 0;
     int mh = 150 + show * row + 110, x = W - 40 - mw, y = (H - mh) / 2;
     U.h.sheet(U.h.ctx, c, x, y, mw, mh, idx[0] == MENU_HEAD_AUD ? "Audio and subtitles" : "Subtitles");
     for (int i = top; i < top + show; i++) {
          int ry = y + 130 + (i - top) * row;
          if (menu_head(idx[i])) {
               if (!c) text(U.h.f_tiny, x + 56, ry + 20, row - 28, K_TEXT3,
                            idx[i] == MENU_HEAD_AUD ? "AUDIO" : "SUBTITLES", -1);
               continue;
          }
          const struct jf_stream *st = idx[i] >= 0 ? stream_of(idx[i]) : NULL;
          if (c) {
               if (i == U.menu_sel) U.h.focus(c, x + 24, ry, mw - 48, row - 8, 18);
          } else {
               char t[160];
               if (!st) snprintf(t, sizeof t, "Off");
               else snprintf(t, sizeof t, "%s", st->title[0] ? st->title : st->lang[0] ? st->lang : "Track");
               /* grey: picture-based subtitles (not supported yet) */
               uint32_t col = st && !strcmp(st->type, "Subtitle") && !st->is_text ? K_TEXT3 : K_TEXT;
               char ft[180];
               fit(U.h.f_item, t, mw - 160, ft, sizeof ft);
               text(U.h.f_item, x + 56, ry, row - 8, col, ft, -1);
               int on = st && !strcmp(st->type, "Audio") ? idx[i] == U.play->audio_index
                                                          : idx[i] == U.sub_index;
               if (on) text(U.h.f_small, x + mw - 56, ry, row - 8, i == U.menu_sel ? 0xffffffff : K_ACCENT_L, "On", 1);
          }
     }
     static const char *const h[] = { "OK", "Choose", "BACK", "Close" };
     U.h.hints(U.h.ctx, c, x + mw / 2, y + mh - 80, h, 2);
}

static void draw_player(struct canvas *c, double now)
{
     /* the video is on the plane below: only the cue, the menu, the OSD */
     int osd = U.osd_until > now || U.pstate != 1 || U.seek_pending;
     if (U.pstate != 0) {
          if (U.cue_shown && !U.menu_on) draw_cue(c, U.cue_shown, osd);
          if (U.menu_on) { draw_menu(c); return; }
     }
     if (!osd) return;
     int W = U.h.W, H = U.h.H;
     int bx = 120, bw = W - 240, by = H - 200;
     double pos = U.seek_pending ? U.seek_target_ms : U.pos_ms;
     if (U.pstate == 0) {
          /* starting: an opaque screen over the video plane, which may show
           * the first frames from 0:00 before a resume's seek lands */
          if (c) gfx_fill_vgradient_full(c, 0xff05070c, 0xff0b0e16);
          else {
               char t[200];
               if (U.playing.type == JF_EPISODE && U.playing.series_name[0])
                    snprintf(t, sizeof t, "%s  ·  S%d E%d  %s", U.playing.series_name,
                             U.playing.season, U.playing.episode, U.playing.name);
               else snprintf(t, sizeof t, "%s", U.playing.name);
               text(U.h.f_head, W / 2, H / 2 - 90, 70, K_TEXT, t, 0);
               text(U.h.f_item, W / 2, H / 2 + 10, 50, K_TEXT2, "Loading…", 0);
          }
          return;
     }
     if (c) {
          gfx_rrect(c, 60, H - 300, W - 120, 240, 32, 0xc0101420);
          gfx_rrect(c, bx, by, bw, 10, 5, K_TRACK);
          if (U.dur_ms > 0) {
               int w = (int)(bw * (pos / U.dur_ms));
               if (w > bw) w = bw;
               if (w > 10) gfx_rrect_hgrad(c, bx, by, w, 10, 5, K_ACCENT_L, K_ACCENT_R);
          }
     } else {
          char t[200], a[32], b[32];
          if (U.playing.type == JF_EPISODE && U.playing.series_name[0])
               snprintf(t, sizeof t, "%s  \xc2\xb7  S%d:E%d - %s", U.playing.series_name,
                        U.playing.season, U.playing.episode, U.playing.name);
          else snprintf(t, sizeof t, "%s", U.playing.name);
          text(U.h.f_head, bx, H - 280, 60, K_TEXT, t, -1);
          const char *st = U.pstate == 0 ? "Starting\xe2\x80\xa6" : U.pstate == 2 ? "Paused" :
                           U.seek_pending ? "Seeking" : "";
          if (st[0]) text(U.h.f_small, W - 120, H - 280, 60, K_TEXT2, st, 1);
          fmt_clock(pos, a, sizeof a);
          fmt_clock(U.dur_ms, b, sizeof b);
          text(U.h.f_small, bx, by + 20, 40, K_TEXT2, a, -1);
          text(U.h.f_small, bx + bw, by + 20, 40, K_TEXT2, b, 1);
          static const char *const h[] = { "OK", "Pause", "LEFT/RIGHT", "Seek", "DOWN", "Audio, subtitles", "BACK", "Stop" };
          U.h.hints(U.h.ctx, c, W / 2, H - 110, h, 4);
     }
     if (c) {
          static const char *const h[] = { "OK", "Pause", "LEFT/RIGHT", "Seek", "DOWN", "Audio, subtitles", "BACK", "Stop" };
          U.h.hints(U.h.ctx, c, W / 2, H - 110, h, 4);
     }
}

/* text wrapped to maxw pixels, at most maxl lines (the last ends in an
 * ellipsis if cut); returns the number of lines drawn */
static int wrap(IDirectFBFont *f, const char *s, int x, int y, int line_h, int maxw, int maxl,
                uint32_t col)
{
     int lines = 0;
     while (*s && lines < maxl) {
          char buf[512];
          int n = 0, last_space = -1, w = 0;
          for (;;) {
               if (!s[n] || n >= (int)sizeof buf - 8) break;
               int m = n + 1;
               while (((unsigned char)s[m] & 0xc0) == 0x80) m++;      /* whole UTF-8 chars */
               memcpy(buf, s, (size_t)m);
               buf[m] = 0;
               w = m * 15;
               if (f) f->GetStringWidth(f, buf, -1, &w);
               if (w > maxw) break;
               if (s[n] == ' ') last_space = n;
               n = m;
          }
          int cut = n;
          if (s[n] && last_space > 0) cut = last_space;
          if (cut <= 0) cut = n > 0 ? n : 1;
          if (lines == maxl - 1 && s[cut]) {
               char out[520];
               snprintf(buf, sizeof buf, "%.*s", cut, s);
               fit(f, buf, maxw - 20, out, sizeof out - 4);
               size_t l = strlen(out);
               if (l < 3 || strcmp(out + l - 3, "\xe2\x80\xa6")) snprintf(out + l, sizeof out - l, "\xe2\x80\xa6");
               text(f, x, y + lines * line_h, line_h, col, out, -1);
          } else {
               snprintf(buf, sizeof buf, "%.*s", cut, s);
               text(f, x, y + lines * line_h, line_h, col, buf, -1);
          }
          lines++;
          s += cut;
          while (*s == ' ') s++;
     }
     return lines;
}

/* Jellyfin's web layout: the backdrop behind, darkened toward the text;
 * the poster (films) or the still (episodes) on the left; title, a meta
 * line, the overview; the actions as a row of buttons */
static void draw_detail(struct canvas *c, double now)
{
     const struct jf_item *it = &U.detail;
     int W = U.h.W, H = U.h.H;
     const char *bid = NULL, *btag = NULL;
     if (it->backdrop_tag[0]) { bid = it->id; btag = it->backdrop_tag; }
     else if (it->parent_backdrop_id[0] && it->parent_backdrop_tag[0]) {
          bid = it->parent_backdrop_id; btag = it->parent_backdrop_tag;
     }
     const struct jfimg *bd = bid ? jfimg_get(bid, "Backdrop", btag, W, H) : NULL;
     int land = it->type == JF_EPISODE || it->type == JF_VIDEO || it->type == JF_MUSICVIDEO;
     int pw = land ? 480 : 300, ph = land ? 270 : 450;
     const char *pid = it->id, *ptag = it->primary_tag;
     if (!ptag[0] && it->type == JF_EPISODE && it->series_id[0] && it->series_primary_tag[0]) {
          /* no still: the series poster */
          pid = it->series_id; ptag = it->series_primary_tag; land = 0; pw = 300; ph = 450;
     }
     const struct jfimg *po = ptag[0] ? jfimg_get(pid, "Primary", ptag, pw, ph) : NULL;
     int px = 120, py = 170, tx = px + pw + 64, tw = W - tx - 120;

     if (c) {
          if (bd) {
               gfx_image(c, 0, 0, W, H, 0, bd->px, bd->w, 255);
               /* dark toward the text, and at the bottom for the hints */
               gfx_rrect_hgrad(c, 0, 0, W, H, 0, 0xf205070c, 0x9905070c);
               gfx_rrect(c, 0, H - 220, W, 220, 0, 0x8005070c);
          }
          if (po) gfx_image(c, px, py, pw, ph, 16, po->px, po->w, 255);
          else {
               gfx_rrect(c, px, py, pw, ph, 16, 0xff1c2130);
               gfx_rrect_outline(c, px, py, pw, ph, 16, 1, K_PANEL_EDGE);
          }
          if (it->position_ticks > 0 && it->runtime_ticks > 0) {
               int fw = (int)((int64_t)(pw - 24) * it->position_ticks / it->runtime_ticks);
               gfx_rrect(c, px + 12, py + ph - 20, pw - 24, 6, 3, 0xa0000000);
               if (fw >= 6) gfx_rrect_hgrad(c, px + 12, py + ph - 20, fw, 6, 3, K_ACCENT_L, K_ACCENT_R);
          }
     }

     /* title, and for an episode its place in the series */
     int y = py - 10;
     char t[300], ft[320];
     if (it->type == JF_EPISODE && it->series_name[0]) {
          if (!c) {
               fit(U.h.f_title, it->series_name, tw, ft, sizeof ft);
               text(U.h.f_title, tx, y, 80, K_TEXT, ft, -1);
               if (it->season >= 0 && it->episode >= 0)
                    snprintf(t, sizeof t, "S%d:E%d - %s", it->season, it->episode, it->name);
               else snprintf(t, sizeof t, "%s", it->name);
               fit(U.h.f_bold, t, tw, ft, sizeof ft);
               text(U.h.f_bold, tx, y + 84, 54, K_TEXT, ft, -1);
          }
          y += 84 + 60;
     } else {
          if (!c) {
               fit(U.h.f_title, it->name, tw, ft, sizeof ft);
               text(U.h.f_title, tx, y, 80, K_TEXT, ft, -1);
          }
          y += 88;
     }

     /* meta: year, runtime, rating, how long is left or when it ends */
     char meta[300] = "", d[48];
     size_t l = 0;
#define META(...) do { l = strlen(meta); snprintf(meta + l, sizeof meta - l, "%s", l ? "   \xc2\xb7   " : ""); \
                       l = strlen(meta); snprintf(meta + l, sizeof meta - l, __VA_ARGS__); } while (0)
     if (it->year) META("%d", it->year);
     if (it->runtime_ticks > 0) { fmt_dur(it->runtime_ticks, d, sizeof d); META("%s", d); }
     if (it->official_rating[0]) META("%s", it->official_rating);
     if (it->rating > 0) META("%.1f / 10", it->rating);
     if (it->runtime_ticks > 0) {
          int64_t left = it->runtime_ticks - (it->position_ticks > 0 ? it->position_ticks : 0);
          if (it->position_ticks > 0) { fmt_dur(left, d, sizeof d); META("%s left", d); }
          time_t end = time(NULL) + (time_t)(left / 10000000);
          struct tm tm;
          localtime_r(&end, &tm);
          strftime(d, sizeof d, "%H:%M", &tm);
          META("Ends at %s", d);
     }
     if (it->played) META("Watched");
#undef META
     if (!c && meta[0]) text(U.h.f_small, tx, y, 40, K_TEXT2, meta, -1);
     y += 60;

     /* the overview, wrapped to the column */
     if (!c && it->overview) wrap(U.h.f_small, it->overview, tx, y, 40, tw, 6, 0xffd0d6e2);

     /* the actions: a row of buttons under the poster */
     int by = py + (ph > 450 ? ph : 450) + 60, bx = px, bh = 72;
     for (int i = 0; i < U.nrows; i++) {
          if (U.rows[i].header) continue;
          int bw = 0;
          if (U.h.f_bold) U.h.f_bold->GetStringWidth(U.h.f_bold, U.rows[i].title, -1, &bw);
          else bw = (int)strlen(U.rows[i].title) * 18;
          bw += 80;
          int foc = i == U.sel;
          if (c) {
               if (foc) gfx_rrect(c, bx, by, bw, bh, bh / 2, 0xffffffff);
               else {
                    gfx_rrect(c, bx, by, bw, bh, bh / 2, 0x40ffffff);
                    gfx_rrect_outline(c, bx, by, bw, bh, bh / 2, 1, 0x40ffffff);
               }
          } else {
               text(U.h.f_bold, bx + bw / 2, by, bh, foc ? 0xff0b0e16 : K_TEXT, U.rows[i].title, 0);
          }
          bx += bw + 24;
     }
     (void)now;
}

void jfui_draw(struct canvas *c, double now)
{
     if (!U.on) return;
     int W = U.h.W, H = U.h.H;
     if (U.scr == J_PLAYER) {
          draw_player(c, now);
          if (U.err_until > now && !c) text(U.h.f_item, W / 2, 80, 60, 0xffff8080, U.err, 0);
          return;
     }
     if (c) gfx_fill_vgradient_full(c, 0xf005070c, 0xfa05070c);
     if (U.scr == J_DETAIL) {
          draw_detail(c, now);
          if (!c && U.busy && U.busy_msg[0]) text(U.h.f_item, W - P_X, H - 150, 50, K_TEXT2, U.busy_msg, 1);
          if (!c && U.err_until > now) text(U.h.f_item, P_X, H - 150, 50, 0xffff8080, U.err, -1);
          static const char *const hd[] = { "LEFT/RIGHT", "Move", "OK", "Select", "BACK", "Back" };
          U.h.hints(U.h.ctx, c, W / 2, H - 96, hd, 3);
          return;
     }
     const char *title = "Jellyfin";
     char tbuf[160];
     switch (U.scr) {
     case J_SERVERS: title = "Choose a Jellyfin server"; break;
     case J_SIGNIN:  title = "Sign in to Jellyfin"; break;
     case J_HOME:    title = "Jellyfin"; break;
     case J_LIST:    title = U.lv[U.depth - 1].title; break;
     case J_DETAIL:  title = U.detail.name; break;
     default: break;
     }
     if (!c) {
          text(U.h.f_title, P_X, 80, 80, K_TEXT, title, -1);
          if (U.server_name[0]) {
               snprintf(tbuf, sizeof tbuf, "%s%s%s", U.server_name, U.user_name[0] ? "  \xc2\xb7  " : "",
                        U.user_name);
               text(U.h.f_small, W - P_X, 80, 80, K_TEXT2, tbuf, 1);
          }
     }
     int y0 = P_TOP, y1 = H - 160;
     if (U.scr == J_SIGNIN) {
          if (!c) {
               if (U.qc_code[0]) {
                    text(U.h.f_item, P_X, 230, 50, K_TEXT2,
                         "In another Jellyfin app, open Quick Connect and enter:", -1);
                    char spaced[32] = "";
                    for (int i = 0; U.qc_code[i] && i < 12; i++) {
                         size_t l = strlen(spaced);
                         snprintf(spaced + l, sizeof spaced - l, "%c%s", U.qc_code[i], U.qc_code[i + 1] ? " " : "");
                    }
                    text(U.h.f_title, P_X, 300, 160, K_TEXT, spaced, -1);
                    text(U.h.f_small, P_X, 470, 40, K_TEXT3,
                         "(Settings \xe2\x80\xba Quick Connect, while signed in)", -1);
               }
          } else if (U.qc_code[0]) {
               gfx_rrect(c, P_X - 30, 290, 640, 180, 28, K_CARD);
               gfx_rrect_outline(c, P_X - 30, 290, 640, 180, 28, 1, K_PANEL_EDGE);
          }
          y0 = 560;
     } else if (U.scr == J_DETAIL) {
          if (!c) {
               char sub[160];
               item_sub(&U.detail, sub, sizeof sub);
               text(U.h.f_item, P_X, 170, 50, K_TEXT2, sub, -1);
               if (U.detail.overview) {
                    /* wrap the overview into up to 5 lines of ~95 chars */
                    const char *p = U.detail.overview;
                    for (int line = 0; line < 5 && *p; line++) {
                         char buf[128];
                         int n = (int)strlen(p), cut = n;
                         if (n > 95) {
                              cut = 95;
                              while (cut > 40 && p[cut] != ' ') cut--;
                         }
                         snprintf(buf, sizeof buf, "%.*s%s", cut, p, line == 4 && cut < n ? "\xe2\x80\xa6" : "");
                         text(U.h.f_small, P_X, 230 + line * 44, 44, K_TEXT2, buf, -1);
                         p += cut;
                         while (*p == ' ') p++;
                    }
               }
          }
          y0 = 480;
     }
     if (card_screen()) draw_cards(c);
     else draw_rows(c, y0, y1);
     if (!c && U.busy && U.busy_msg[0]) text(U.h.f_item, W - P_X, H - 150, 50, K_TEXT2, U.busy_msg, 1);
     if (!c && U.err_until > now) text(U.h.f_item, P_X, H - 150, 50, 0xffff8080, U.err, -1);
     static const char *const hs[] = { "ARROWS", "Move", "OK", "Select", "BACK", "Back" };
     U.h.hints(U.h.ctx, c, W / 2, H - 96, hs, 3);
}
