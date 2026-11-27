#ifndef VIBE_YT_H
#define VIBE_YT_H
/*
 * The YouTube client's talk with the helper (helper/ythelper.py on a home
 * machine): JSON over http on the LAN. Blocking; call from one worker
 * thread. Errors: yt_error() (that thread's last message).
 *
 * Where the helper is: /var/local/vibe/state/youtube, a line
 * "helper=http://HOST:PORT" (default below); "maxh=N" caps the quality
 * (default 1080).
 */
#include <stdint.h>

#define YT_STATE_FILE   "/var/local/vibe/state/youtube"
#define YT_DEFAULT_BASE "http://192.168.2.31:8770"     /* fin, the Jellyfin box */
#define YT_MAX_SHELVES  16
#define YT_MAX_SEGS     40

struct yt_item {
     char id[16];              /* video id (11 chars) */
     char title[200];
     char channel[100];
     char channel_id[32];
     int  duration;            /* seconds; 0 = live or unknown */
     long long views;
     char published[16];       /* YYYY-MM-DD or "" */
     char info[100];           /* ready-made "1.2M views · 3 days ago" (YouTube's feeds), or "" */
     int  live;
     double position;          /* seconds watched (helper's record), 0 = none */
};

struct yt_list {
     struct yt_item *items;
     int n;
};

struct yt_shelf {
     char title[100];
     char channel_id[32];      /* a followed channel's shelf; "" otherwise */
     struct yt_list l;
};

struct yt_home {
     struct yt_shelf sh[YT_MAX_SHELVES];
     int n;
};

struct yt_seg {
     double start, end;        /* seconds */
     char category[24];        /* SponsorBlock's: sponsor, selfpromo, ... */
     int  skip;                /* we skip it automatically */
};

#define YT_MAX_CHAPTERS 64
struct yt_chapter {
     double start;             /* seconds */
     char title[80];
};

struct yt_video {
     struct yt_item it;
     char *description;        /* malloc'd, may be NULL */
     struct yt_seg segs[YT_MAX_SEGS];
     int  nsegs;
     int  followed;
     struct yt_chapter chapters[YT_MAX_CHAPTERS];
     int  nchapters;
};

struct yt_comment {
     char author[64];
     char text[600];
     char likes[16], age[32], replies[16];
};

struct yt_channel {
     char id[32], title[100];
     int  followed;
     struct yt_list l;
};

struct yt_play {
     char url[1500];
     char mime[48];
     int  duration;
     int  live;
};

/* reads the state file (helper address, quality cap) */
void yt_config_load(void);
const char *yt_base(void);
int  yt_maxh(void);
/* change the quality cap and save it in the state file (0 on success) */
int  yt_set_maxh(int h);

int  yt_home(struct yt_home *out);
int  yt_search(const char *q, int n, struct yt_list *out);
int  yt_history(int n, struct yt_list *out);
int  yt_channel(const char *id, int n, struct yt_channel *out);
int  yt_video(const char *id, struct yt_video *out);
int  yt_play(const char *id, int maxh, struct yt_play *out);
int  yt_progress(const char *id, double pos, double dur);

/* the YouTube account (sign-in by device code, in the helper) */
struct yt_account {
     int  signed_in, pending;
     char name[100];
};
int  yt_account(struct yt_account *out);
/* start: the code to show and where to enter it */
int  yt_signin_start(char *code, int code_n, char *url, int url_n);
/* poll: *status 0 pending, 1 done (name set), -1 expired or refused (yt_error) */
int  yt_signin_poll(int *status, char *name, int name_n);
int  yt_signout(void);

/* settings kept by the helper: DeArrow (community titles and thumbnails).
 * set < 0 only reads. *dearrow gets the value; thumbnails ask for DeArrow's
 * when it is on (yt_thumb_url). */
int  yt_settings(int set_dearrow, int *dearrow);
/* autoplay of the next video (a helper setting too); set < 0 only reads */
int  yt_autoplay(int set, int *on);

/* what YouTube suggests after a video; its top comments (up to max) */
int  yt_related(const char *id, struct yt_list *out);
int  yt_comments(const char *id, struct yt_comment *out, int max, int *n);

/* casting from a phone ("Link with TV code"; the helper is the lounge screen) */
struct yt_cmd {
     char cmd[16];             /* none, play, pause, resume, seek, stop */
     char id[16];              /* play: the video */
     double pos;               /* play, seek: seconds */
};
int  yt_remote_start(char *code, int n);     /* a pairing code to show */
int  yt_remote_status(int *enabled, char *connected, int n);
int  yt_remote_stop(void);
/* waits up to timeout_s for the phone's next command (cmd "none" if none) */
int  yt_remote_next(int timeout_s, struct yt_cmd *out);
/* what the TV is doing, for the phone: state playing|paused|loading|stopped */
int  yt_remote_state(const char *id, double pos, double dur, const char *state);
int  yt_follow(const char *channel_id, int on);

/* image URLs on the helper (out >= 256 bytes); size 'm' 320x180, 'l' 1280x720 */
void yt_thumb_url(const char *id, char size, char *out, int n);

void yt_list_free(struct yt_list *l);
void yt_home_free(struct yt_home *h);
void yt_video_free(struct yt_video *v);

/* the categories we skip without asking (SmartTube's defaults) */
int  yt_auto_skip(const char *category);
/* a short name for a category ("Sponsor", "Self-promotion", ...) */
const char *yt_cat_name(const char *category);

const char *yt_error(void);

#endif
