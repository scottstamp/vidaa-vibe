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
#define YT_DEFAULT_BASE "http://192.168.2.154:8770"
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

struct yt_video {
     struct yt_item it;
     char *description;        /* malloc'd, may be NULL */
     struct yt_seg segs[YT_MAX_SEGS];
     int  nsegs;
     int  followed;
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

int  yt_home(struct yt_home *out);
int  yt_search(const char *q, int n, struct yt_list *out);
int  yt_history(int n, struct yt_list *out);
int  yt_channel(const char *id, int n, struct yt_channel *out);
int  yt_video(const char *id, struct yt_video *out);
int  yt_play(const char *id, int maxh, struct yt_play *out);
int  yt_progress(const char *id, double pos, double dur);
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
