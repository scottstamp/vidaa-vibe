#ifndef VIBE_JF_H
#define VIBE_JF_H
/*
 * Jellyfin client: discovery, login (Quick Connect or password), browsing,
 * playback negotiation and progress reports, over http.c and cJSON.
 * Blocking calls; the UI runs them on a worker thread.
 * API reference: the server's /api-docs/openapi.json (10.11); notes in
 * docs/findings/07-jellyfin.md.
 */
#include <stddef.h>
#include <stdint.h>

#define JF_URL_MAX   1024
#define JF_ID        40     /* 32 hex + NUL, with slack */

struct jf_session {
     char server[256];        /* "http://192.168.2.31:8096", no trailing slash */
     char server_name[64];
     char server_id[JF_ID];
     char device_id[JF_ID];   /* ours, made once, kept in the state file */
     char token[JF_ID * 2];   /* "" = not signed in */
     char user_id[JF_ID];
     char user_name[64];
};

struct jf_server {            /* from UDP discovery */
     char address[256];
     char name[64];
     char id[JF_ID];
};

/* item kinds we treat differently; anything else is JF_OTHER */
enum jf_type {
     JF_OTHER, JF_VIEW, JF_FOLDER, JF_MOVIE, JF_SERIES, JF_SEASON, JF_EPISODE,
     JF_BOXSET, JF_VIDEO, JF_MUSICVIDEO, JF_AUDIO, JF_ALBUM, JF_PLAYLIST
};

struct jf_item {
     char id[JF_ID];
     char name[128];
     enum jf_type type;
     char type_name[24];      /* the server's Type string */
     char collection[24];     /* CollectionType of a view: movies, tvshows, ... */
     char series_id[JF_ID];
     char series_name[96];
     char season_id[JF_ID];
     int  season, episode;    /* ParentIndexNumber, IndexNumber; -1 if none */
     int  year;               /* ProductionYear; 0 if none */
     int64_t runtime_ticks;   /* 10,000,000 ticks per second */
     int64_t position_ticks;  /* UserData.PlaybackPositionTicks */
     int  played;
     int  unplayed;           /* UserData.UnplayedItemCount (series, seasons) */
     int  child_count;
     float rating;            /* CommunityRating; 0 if none */
     char official_rating[16];
     char primary_tag[JF_ID]; /* ImageTags.Primary; "" if no image */
     char thumb_tag[JF_ID];   /* ImageTags.Thumb */
     char backdrop_tag[JF_ID];/* BackdropImageTags[0] */
     char parent_backdrop_id[JF_ID];
     char parent_backdrop_tag[JF_ID];
     char parent_thumb_id[JF_ID];   /* ParentThumbItemId: the series, for episodes */
     char parent_thumb_tag[JF_ID];  /* ParentThumbImageTag */
     char series_primary_tag[JF_ID];/* SeriesPrimaryImageTag (poster of the series) */
     float aspect;            /* PrimaryImageAspectRatio; 0 if unknown */
     char *overview;          /* malloc'd or NULL: jf_items_free */
};

struct jf_list {
     struct jf_item *items;
     int n, total;            /* total = TotalRecordCount (for paging) */
};

struct jf_stream {            /* one MediaStream of a source */
     int  index;
     char type[12];           /* Video, Audio, Subtitle */
     char codec[16];
     char lang[8];
     char title[96];          /* DisplayTitle */
     int  is_default, is_external, is_text;
     int  channels;
};

#define JF_MAX_STREAMS 48

struct jf_play {
     char url[JF_URL_MAX];    /* what to hand the player */
     int  method;             /* JF_DIRECT_PLAY, JF_DIRECT_STREAM, JF_TRANSCODE */
     char play_session_id[JF_ID * 2];
     char media_source_id[JF_ID * 2];
     char container[24];
     int64_t runtime_ticks;
     int  audio_index, subtitle_index;   /* chosen (defaults), -1 = none */
     int  nstreams;
     struct jf_stream streams[JF_MAX_STREAMS];
};
enum { JF_DIRECT_PLAY, JF_DIRECT_STREAM, JF_TRANSCODE };

/* --- setup -------------------------------------------------------------- */

/* fill device_id with a new random id if empty */
void jf_session_init(struct jf_session *s);
/* state file (key=value lines); 0 on success */
int  jf_session_load(struct jf_session *s, const char *path);
int  jf_session_save(const struct jf_session *s, const char *path);

/* UDP broadcast "who is JellyfinServer?" on port 7359; returns count */
int  jf_discover(struct jf_server *out, int max, int timeout_ms);
/* /System/Info/Public: checks the address, fills server_name/server_id */
int  jf_public_info(struct jf_session *s);

/* --- sign in ------------------------------------------------------------ */

/* Quick Connect: show `code` (6 digits) to the user, who approves it in
 * another signed-in client. 0 on success. */
int  jf_qc_enabled(struct jf_session *s);
int  jf_qc_initiate(struct jf_session *s, char *code /* 16 */, char *secret /* 128 */);
/* 1 approved, 0 pending, -1 error/expired */
int  jf_qc_poll(struct jf_session *s, const char *secret);
/* exchange an approved secret for a token: fills token, user_id, user_name */
int  jf_qc_finish(struct jf_session *s, const char *secret);
int  jf_login_password(struct jf_session *s, const char *user, const char *pw);
void jf_logout(struct jf_session *s);   /* tells the server, clears token */

/* --- browse (0 on success; free with jf_list_free) ---------------------- */

int  jf_views(struct jf_session *s, struct jf_list *out);
int  jf_resume(struct jf_session *s, int limit, struct jf_list *out);
int  jf_next_up(struct jf_session *s, int limit, struct jf_list *out);
int  jf_latest(struct jf_session *s, const char *parent_id, int limit, struct jf_list *out);
/* children of a view or folder, by sort name; start/limit for paging */
int  jf_children(struct jf_session *s, const char *parent_id, int start, int limit,
                 struct jf_list *out);
int  jf_seasons(struct jf_session *s, const char *series_id, struct jf_list *out);
int  jf_episodes(struct jf_session *s, const char *series_id, const char *season_id,
                 struct jf_list *out);
int  jf_item(struct jf_session *s, const char *id, struct jf_item *out);
void jf_list_free(struct jf_list *l);

/* image URL: kind "Primary", "Backdrop", "Thumb"; tag may be "" */
void jf_image_url(const struct jf_session *s, const char *id, const char *kind,
                  const char *tag, int max_w, int max_h, char *out /* JF_URL_MAX */);
/* raw image bytes (JPEG); caller frees *data */
int  jf_fetch(struct jf_session *s, const char *url, char **data, size_t *len);

/* --- play --------------------------------------------------------------- */

/* POST /Items/{id}/PlaybackInfo with our device profile; picks direct play,
 * direct stream or transcode. audio_index -1 = server default;
 * subtitle_index JF_SUB_DEFAULT = server default, -1 = off. */
#define JF_SUB_DEFAULT (-2)
int  jf_playback(struct jf_session *s, const char *item_id, int64_t start_ticks,
                 int audio_index, int subtitle_index, struct jf_play *out);
/* progress reports: event 0 start, 1 progress, 2 stopped */
int  jf_report(struct jf_session *s, const char *item_id, const struct jf_play *p,
               int event, int64_t position_ticks, int paused);
int  jf_set_played(struct jf_session *s, const char *item_id, int played);
/* end a transcode on the server (DELETE /Videos/ActiveEncodings) */
void jf_stop_transcode(struct jf_session *s, const struct jf_play *p);

/* last error text (shared; read it on the thread that made the call, soon) */
const char *jf_error(void);

#endif
