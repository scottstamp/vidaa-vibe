/*
 * Jellyfin client (see jf.h). Endpoints as in Jellyfin 10.11's OpenAPI
 * document; the two servers on the owner's LAN run 10.11.11.
 */
#include "jf.h"
#include "http.h"
#include "../third_party/cjson/cJSON.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_NAME    "vibe-ui"
#define CLIENT_VERSION "0.1"
#define DEVICE_NAME    "Hisense TV"

/* the fields every list asks for: what the grid and the detail panel show */
#define ITEM_FIELDS "PrimaryImageAspectRatio,Overview,ChildCount,ParentId,SeriesPrimaryImage"

/* not thread-local: TLS made zig link ld-linux-armhf.so.3, which the TV lacks */
static char err_text[256];   /* shared with the image fetcher: only ever a message, a race garbles text at worst */

const char *jf_error(void) { return err_text; }

static void set_err(const char *fmt, ...)
{
     va_list ap;
     va_start(ap, fmt);
     vsnprintf(err_text, sizeof err_text, fmt, ap);
     va_end(ap);
}

static void copy(char *dst, size_t n, const char *src)
{
     if (!n) return;
     snprintf(dst, n, "%s", src ? src : "");
}
#define COPY(dst, src) copy((dst), sizeof(dst), (src))

/* ---- session ------------------------------------------------------------ */

void jf_session_init(struct jf_session *s)
{
     if (s->device_id[0]) return;
     unsigned char r[16];
     int fd = open("/dev/urandom", O_RDONLY);
     if (fd < 0 || read(fd, r, sizeof r) != (ssize_t)sizeof r) {
          struct timespec ts;
          clock_gettime(CLOCK_REALTIME, &ts);
          srand((unsigned)(ts.tv_nsec ^ ts.tv_sec ^ getpid()));
          for (size_t i = 0; i < sizeof r; i++) r[i] = (unsigned char)rand();
     }
     if (fd >= 0) close(fd);
     for (int i = 0; i < 16; i++) sprintf(s->device_id + 2 * i, "%02x", r[i]);
}

static const struct { const char *key; size_t off, size; } FIELDS[] = {
#define F(k, m) { k, offsetof(struct jf_session, m), sizeof(((struct jf_session *)0)->m) }
     F("server", server), F("server_name", server_name), F("server_id", server_id),
     F("device_id", device_id), F("token", token), F("user_id", user_id),
     F("user_name", user_name),
#undef F
};
#define NFIELDS (int)(sizeof FIELDS / sizeof FIELDS[0])

int jf_session_load(struct jf_session *s, const char *path)
{
     FILE *f = fopen(path, "r");
     if (!f) { set_err("%s: %s", path, strerror(errno)); return -1; }
     char line[512];
     while (fgets(line, sizeof line, f)) {
          line[strcspn(line, "\r\n")] = 0;
          char *eq = strchr(line, '=');
          if (!eq) continue;
          *eq = 0;
          for (int i = 0; i < NFIELDS; i++)
               if (strcmp(line, FIELDS[i].key) == 0)
                    copy((char *)s + FIELDS[i].off, FIELDS[i].size, eq + 1);
     }
     fclose(f);
     return 0;
}

int jf_session_save(const struct jf_session *s, const char *path)
{
     char tmp[512];
     snprintf(tmp, sizeof tmp, "%s.new", path);
     int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* holds the token */
     if (fd < 0) { set_err("%s: %s", tmp, strerror(errno)); return -1; }
     FILE *f = fdopen(fd, "w");
     for (int i = 0; i < NFIELDS; i++)
          fprintf(f, "%s=%s\n", FIELDS[i].key, (const char *)s + FIELDS[i].off);
     fflush(f);
     fsync(fd);
     fclose(f);
     if (rename(tmp, path) != 0) { set_err("rename %s: %s", path, strerror(errno)); return -1; }
     return 0;
}

/* ---- requests ----------------------------------------------------------- */

static void auth_header(const struct jf_session *s, char *out, size_t n)
{
     snprintf(out, n,
              "Authorization: MediaBrowser Client=\"" CLIENT_NAME "\", Device=\"" DEVICE_NAME
              "\", DeviceId=\"%s\", Version=\"" CLIENT_VERSION "\"%s%s%s",
              s->device_id, s->token[0] ? ", Token=\"" : "", s->token, s->token[0] ? "\"" : "");
}

/* method + path (with query) -> parsed JSON (caller cJSON_Delete), or NULL.
 * *status gets the HTTP status. A 2xx with an empty body returns a JSON
 * null so callers can tell success. */
static cJSON *call(struct jf_session *s, const char *method, const char *path,
                   const char *body, long *status_out)
{
     char url[JF_URL_MAX + 256], auth[400], e[128];
     snprintf(url, sizeof url, "%s%s", s->server, path);
     auth_header(s, auth, sizeof auth);
     const char *hdr[] = { auth, "Accept: application/json",
                           body ? "Content-Type: application/json" : NULL, NULL };
     struct http_buf b;
     long st = 0;
     if (http_request(method, url, hdr, body, 0, &b, &st, e) != 0) {
          set_err("%s %s: %s", method, path, e);
          if (status_out) *status_out = 0;
          return NULL;
     }
     if (status_out) *status_out = st;
     if (st < 200 || st >= 300) {
          set_err("%s %s: HTTP %ld %.120s", method, path, st, b.data);
          http_buf_free(&b);
          return NULL;
     }
     cJSON *j = b.len ? cJSON_Parse(b.data) : cJSON_CreateNull();
     if (!j) set_err("%s %s: bad JSON", method, path);
     http_buf_free(&b);
     return j;
}

static const char *jstr(const cJSON *o, const char *k)
{
     const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
     return cJSON_IsString(v) ? v->valuestring : "";
}
static double jnum(const cJSON *o, const char *k, double dflt)
{
     const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
     return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}
static int jbool(const cJSON *o, const char *k)
{
     return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

/* ---- discovery ---------------------------------------------------------- */

int jf_discover(struct jf_server *out, int max, int timeout_ms)
{
     int fd = socket(AF_INET, SOCK_DGRAM, 0);
     if (fd < 0) { set_err("socket: %s", strerror(errno)); return 0; }
     int one = 1;
     setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
     struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(7359),
                               .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
     static const char q[] = "who is JellyfinServer?";
     sendto(fd, q, sizeof q - 1, 0, (struct sockaddr *)&to, sizeof to);
     /* The TV's vendor firewall (FIREWALL_IN_EULAON) drops incoming UDP
      * unless conntrack ties it to something we sent, and an answer to a
      * broadcast is not tied (measured: no answers, while the PC got two).
      * So also ask every address of our own /24 directly: those answers
      * are ESTABLISHED and pass. 254 small packets, once, on request. */
     struct ifaddrs *ifs, *i;
     if (getifaddrs(&ifs) == 0) {
          for (i = ifs; i; i = i->ifa_next) {
               if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
               if (i->ifa_flags & IFF_LOOPBACK || !(i->ifa_flags & IFF_UP)) continue;
               uint32_t me = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
               for (uint32_t h = 1; h < 255; h++) {
                    uint32_t a = (me & 0xffffff00u) | h;
                    if (a == me) continue;
                    to.sin_addr.s_addr = htonl(a);
                    sendto(fd, q, sizeof q - 1, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof to);
               }
          }
          freeifaddrs(ifs);
     }
     int n = 0;
     struct timespec t0;
     clock_gettime(CLOCK_MONOTONIC, &t0);
     while (n < max) {
          struct timespec t;
          clock_gettime(CLOCK_MONOTONIC, &t);
          int left = timeout_ms - (int)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
          if (left <= 0) break;
          struct pollfd p = { fd, POLLIN, 0 };
          if (poll(&p, 1, left) <= 0) break;
          char buf[2048];
          ssize_t r = recv(fd, buf, sizeof buf - 1, 0);
          if (r <= 0) break;
          buf[r] = 0;
          cJSON *j = cJSON_Parse(buf);
          if (!j) continue;
          const char *id = jstr(j, "Id");
          int dup = 0;
          for (int i = 0; i < n; i++) if (strcmp(out[i].id, id) == 0) dup = 1;
          if (!dup && jstr(j, "Address")[0]) {
               COPY(out[n].address, jstr(j, "Address"));
               COPY(out[n].name, jstr(j, "Name"));
               COPY(out[n].id, id);
               n++;
          }
          cJSON_Delete(j);
     }
     close(fd);
     return n;
}

int jf_public_info(struct jf_session *s)
{
     size_t l = strlen(s->server);
     while (l && s->server[l - 1] == '/') s->server[--l] = 0;
     cJSON *j = call(s, "GET", "/System/Info/Public", NULL, NULL);
     if (!j) return -1;
     if (!jstr(j, "Id")[0]) { set_err("not a Jellyfin server"); cJSON_Delete(j); return -1; }
     COPY(s->server_name, jstr(j, "ServerName"));
     COPY(s->server_id, jstr(j, "Id"));
     cJSON_Delete(j);
     return 0;
}

/* ---- sign in ------------------------------------------------------------ */

int jf_qc_enabled(struct jf_session *s)
{
     cJSON *j = call(s, "GET", "/QuickConnect/Enabled", NULL, NULL);
     int on = j && cJSON_IsTrue(j);
     cJSON_Delete(j);
     return on;
}

int jf_qc_initiate(struct jf_session *s, char *code, char *secret)
{
     cJSON *j = call(s, "POST", "/QuickConnect/Initiate", NULL, NULL);
     if (!j) return -1;
     copy(code, 16, jstr(j, "Code"));
     copy(secret, 128, jstr(j, "Secret"));
     cJSON_Delete(j);
     if (!code[0] || !secret[0]) { set_err("Quick Connect: no code"); return -1; }
     return 0;
}

int jf_qc_poll(struct jf_session *s, const char *secret)
{
     char path[256], esc[160];
     snprintf(path, sizeof path, "/QuickConnect/Connect?secret=%s", http_escape(secret, esc, sizeof esc));
     long st;
     cJSON *j = call(s, "GET", path, NULL, &st);
     if (!j) return -1;
     int ok = jbool(j, "Authenticated");
     cJSON_Delete(j);
     return ok;
}

static int take_auth(struct jf_session *s, cJSON *j)
{
     const cJSON *u = cJSON_GetObjectItemCaseSensitive(j, "User");
     COPY(s->token, jstr(j, "AccessToken"));
     COPY(s->user_id, jstr(u, "Id"));
     COPY(s->user_name, jstr(u, "Name"));
     if (jstr(j, "ServerId")[0]) COPY(s->server_id, jstr(j, "ServerId"));
     cJSON_Delete(j);
     if (!s->token[0] || !s->user_id[0]) { set_err("sign-in answer had no token"); return -1; }
     return 0;
}

int jf_qc_finish(struct jf_session *s, const char *secret)
{
     cJSON *b = cJSON_CreateObject();
     cJSON_AddStringToObject(b, "Secret", secret);
     char *body = cJSON_PrintUnformatted(b);
     cJSON_Delete(b);
     cJSON *j = call(s, "POST", "/Users/AuthenticateWithQuickConnect", body, NULL);
     free(body);
     return j ? take_auth(s, j) : -1;
}

int jf_login_password(struct jf_session *s, const char *user, const char *pw)
{
     cJSON *b = cJSON_CreateObject();
     cJSON_AddStringToObject(b, "Username", user);
     cJSON_AddStringToObject(b, "Pw", pw ? pw : "");
     char *body = cJSON_PrintUnformatted(b);
     cJSON_Delete(b);
     cJSON *j = call(s, "POST", "/Users/AuthenticateByName", body, NULL);
     free(body);
     return j ? take_auth(s, j) : -1;
}

void jf_logout(struct jf_session *s)
{
     if (s->token[0]) cJSON_Delete(call(s, "POST", "/Sessions/Logout", NULL, NULL));
     s->token[0] = s->user_id[0] = s->user_name[0] = 0;
}

/* ---- items -------------------------------------------------------------- */

static enum jf_type type_of(const char *t)
{
     static const struct { const char *n; enum jf_type t; } M[] = {
          { "CollectionFolder", JF_VIEW }, { "UserView", JF_VIEW }, { "Folder", JF_FOLDER },
          { "Movie", JF_MOVIE }, { "Series", JF_SERIES }, { "Season", JF_SEASON },
          { "Episode", JF_EPISODE }, { "BoxSet", JF_BOXSET }, { "Video", JF_VIDEO },
          { "MusicVideo", JF_MUSICVIDEO }, { "Audio", JF_AUDIO }, { "MusicAlbum", JF_ALBUM },
          { "Playlist", JF_PLAYLIST },
     };
     for (size_t i = 0; i < sizeof M / sizeof M[0]; i++)
          if (strcmp(M[i].n, t) == 0) return M[i].t;
     return JF_OTHER;
}

static void parse_item(const cJSON *o, struct jf_item *it)
{
     memset(it, 0, sizeof *it);
     COPY(it->id, jstr(o, "Id"));
     COPY(it->name, jstr(o, "Name"));
     COPY(it->type_name, jstr(o, "Type"));
     it->type = type_of(it->type_name);
     COPY(it->collection, jstr(o, "CollectionType"));
     COPY(it->series_id, jstr(o, "SeriesId"));
     COPY(it->series_name, jstr(o, "SeriesName"));
     COPY(it->season_id, jstr(o, "SeasonId"));
     it->season = (int)jnum(o, "ParentIndexNumber", -1);
     it->episode = (int)jnum(o, "IndexNumber", -1);
     it->year = (int)jnum(o, "ProductionYear", 0);
     it->runtime_ticks = (int64_t)jnum(o, "RunTimeTicks", 0);
     it->child_count = (int)jnum(o, "ChildCount", 0);
     it->rating = (float)jnum(o, "CommunityRating", 0);
     COPY(it->official_rating, jstr(o, "OfficialRating"));
     it->aspect = (float)jnum(o, "PrimaryImageAspectRatio", 0);
     const cJSON *ud = cJSON_GetObjectItemCaseSensitive(o, "UserData");
     if (ud) {
          it->position_ticks = (int64_t)jnum(ud, "PlaybackPositionTicks", 0);
          it->played = jbool(ud, "Played");
          it->unplayed = (int)jnum(ud, "UnplayedItemCount", 0);
     }
     const cJSON *tags = cJSON_GetObjectItemCaseSensitive(o, "ImageTags");
     COPY(it->primary_tag, jstr(tags, "Primary"));
     COPY(it->thumb_tag, jstr(tags, "Thumb"));
     const cJSON *bd = cJSON_GetObjectItemCaseSensitive(o, "BackdropImageTags");
     if (cJSON_IsArray(bd) && cJSON_GetArraySize(bd) > 0 && cJSON_IsString(bd->child))
          COPY(it->backdrop_tag, bd->child->valuestring);
     COPY(it->parent_backdrop_id, jstr(o, "ParentBackdropItemId"));
     const cJSON *pbd = cJSON_GetObjectItemCaseSensitive(o, "ParentBackdropImageTags");
     if (cJSON_IsArray(pbd) && cJSON_GetArraySize(pbd) > 0 && cJSON_IsString(pbd->child))
          COPY(it->parent_backdrop_tag, pbd->child->valuestring);
     COPY(it->parent_thumb_id, jstr(o, "ParentThumbItemId"));
     COPY(it->parent_thumb_tag, jstr(o, "ParentThumbImageTag"));
     COPY(it->series_primary_tag, jstr(o, "SeriesPrimaryImageTag"));
     const char *ov = jstr(o, "Overview");
     it->overview = ov[0] ? strdup(ov) : NULL;
}

/* a list answer: {"Items":[...],"TotalRecordCount":n} or a bare array */
static int parse_list(cJSON *j, struct jf_list *out)
{
     memset(out, 0, sizeof *out);
     if (!j) return -1;
     const cJSON *arr = cJSON_IsArray(j) ? j : cJSON_GetObjectItemCaseSensitive(j, "Items");
     int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : 0;
     out->items = calloc(n ? n : 1, sizeof *out->items);
     if (!out->items) { cJSON_Delete(j); set_err("out of memory"); return -1; }
     const cJSON *e;
     cJSON_ArrayForEach(e, arr) parse_item(e, &out->items[out->n++]);
     out->total = cJSON_IsArray(j) ? out->n : (int)jnum(j, "TotalRecordCount", out->n);
     cJSON_Delete(j);
     return 0;
}

void jf_list_free(struct jf_list *l)
{
     for (int i = 0; i < l->n; i++) free(l->items[i].overview);
     free(l->items);
     memset(l, 0, sizeof *l);
}

static int get_list(struct jf_session *s, struct jf_list *out, const char *fmt, ...)
{
     char path[JF_URL_MAX];
     va_list ap;
     va_start(ap, fmt);
     vsnprintf(path, sizeof path, fmt, ap);
     va_end(ap);
     return parse_list(call(s, "GET", path, NULL, NULL), out);
}

int jf_views(struct jf_session *s, struct jf_list *out)
{
     return get_list(s, out, "/UserViews?userId=%s", s->user_id);
}

int jf_resume(struct jf_session *s, int limit, struct jf_list *out)
{
     return get_list(s, out, "/UserItems/Resume?userId=%s&limit=%d&mediaTypes=Video"
                     "&enableImageTypes=Primary,Backdrop,Thumb&fields=" ITEM_FIELDS,
                     s->user_id, limit);
}

int jf_next_up(struct jf_session *s, int limit, struct jf_list *out)
{
     return get_list(s, out, "/Shows/NextUp?userId=%s&limit=%d&fields=" ITEM_FIELDS,
                     s->user_id, limit);
}

int jf_latest(struct jf_session *s, const char *parent_id, int limit, struct jf_list *out)
{
     return get_list(s, out, "/Items/Latest?userId=%s&parentId=%s&limit=%d&fields=" ITEM_FIELDS,
                     s->user_id, parent_id, limit);
}

int jf_children(struct jf_session *s, const char *parent_id, int start, int limit,
                struct jf_list *out)
{
     return get_list(s, out, "/Items?userId=%s&parentId=%s&startIndex=%d&limit=%d"
                     "&sortBy=SortName&sortOrder=Ascending&fields=" ITEM_FIELDS,
                     s->user_id, parent_id, start, limit);
}

int jf_seasons(struct jf_session *s, const char *series_id, struct jf_list *out)
{
     return get_list(s, out, "/Shows/%s/Seasons?userId=%s&fields=" ITEM_FIELDS,
                     series_id, s->user_id);
}

int jf_episodes(struct jf_session *s, const char *series_id, const char *season_id,
                struct jf_list *out)
{
     return get_list(s, out, "/Shows/%s/Episodes?userId=%s&seasonId=%s&fields=" ITEM_FIELDS,
                     series_id, s->user_id, season_id);
}

int jf_item(struct jf_session *s, const char *id, struct jf_item *out)
{
     char path[256];
     snprintf(path, sizeof path, "/Items/%s?userId=%s", id, s->user_id);
     cJSON *j = call(s, "GET", path, NULL, NULL);
     if (!j) return -1;
     parse_item(j, out);
     cJSON_Delete(j);
     return 0;
}

void jf_image_url(const struct jf_session *s, const char *id, const char *kind,
                  const char *tag, int max_w, int max_h, char *out)
{
     /* fillWidth/fillHeight: the server crops to the box, so the grid needs
      * no scaling for the common case; quality 85 JPEG */
     snprintf(out, JF_URL_MAX, "%s/Items/%s/Images/%s?fillWidth=%d&fillHeight=%d&quality=85%s%s",
              s->server, id, kind, max_w, max_h, tag && tag[0] ? "&tag=" : "", tag ? tag : "");
}

int jf_subtitle(struct jf_session *s, const char *item_id, const char *media_source_id,
                int index, char **data, size_t *len)
{
     char url[JF_URL_MAX];
     snprintf(url, sizeof url, "%s/Videos/%s/%s/Subtitles/%d/0/Stream.srt",
              s->server, item_id, media_source_id, index);
     return jf_fetch(s, url, data, len);
}

int jf_fetch(struct jf_session *s, const char *url, char **data, size_t *len)
{
     char auth[400], e[128];
     auth_header(s, auth, sizeof auth);
     const char *hdr[] = { auth, NULL };
     struct http_buf b;
     long st;
     *data = NULL; *len = 0;
     if (http_request("GET", url, hdr, NULL, 20000, &b, &st, e) != 0) { set_err("%s", e); return -1; }
     if (st != 200) { set_err("HTTP %ld", st); http_buf_free(&b); return -1; }
     *data = b.data;
     *len = b.len;
     return 0;
}

/* ---- playback ----------------------------------------------------------- */

/*
 * What this TV decodes in hardware: the caps of the vendor's GStreamer
 * videoessink/audioessink (docs/findings/07-jellyfin.md). Text subtitles
 * come as separate files we draw ourselves; image subtitles are burned in
 * by the server (transcode). Transcodes are HLS with MPEG-TS segments
 * (hlsdemux is on the TV).
 */
static const char DEVICE_PROFILE[] =
"{\"Name\":\"vibe-ui (Hisense MT9602)\","
"\"MaxStreamingBitrate\":120000000,\"MaxStaticBitrate\":200000000,"
"\"MusicStreamingTranscodingBitrate\":384000,"
"\"DirectPlayProfiles\":["
 "{\"Type\":\"Video\",\"Container\":\"mkv,webm\","
  "\"VideoCodec\":\"h264,hevc,vp9,av1,vp8,mpeg2video,mpeg4\","
  "\"AudioCodec\":\"aac,mp3,mp2,ac3,eac3,dts,flac,opus,vorbis,pcm_s16le,pcm_s24le\"},"
 "{\"Type\":\"Video\",\"Container\":\"mp4,m4v,mov\","
  "\"VideoCodec\":\"h264,hevc,av1,mpeg4\",\"AudioCodec\":\"aac,mp3,ac3,eac3,flac,opus\"},"
 "{\"Type\":\"Video\",\"Container\":\"ts,mpegts,m2ts\","
  "\"VideoCodec\":\"h264,hevc,mpeg2video\",\"AudioCodec\":\"aac,mp3,mp2,ac3,eac3,dts\"},"
 "{\"Type\":\"Audio\",\"Container\":\"mp3,flac,aac,m4a,ogg,opus,wav\"}],"
"\"TranscodingProfiles\":["
 "{\"Type\":\"Video\",\"Container\":\"ts\",\"Protocol\":\"hls\",\"Context\":\"Streaming\","
  "\"VideoCodec\":\"h264,hevc\",\"AudioCodec\":\"aac,ac3,eac3\",\"MaxAudioChannels\":\"6\","
  "\"MinSegments\":1,\"BreakOnNonKeyFrames\":true},"
 "{\"Type\":\"Audio\",\"Container\":\"mp3\",\"Protocol\":\"http\",\"Context\":\"Streaming\","
  "\"AudioCodec\":\"mp3\"}],"
"\"ContainerProfiles\":[],\"CodecProfiles\":[],"
"\"SubtitleProfiles\":["
 "{\"Format\":\"srt\",\"Method\":\"External\"},{\"Format\":\"subrip\",\"Method\":\"External\"},"
 "{\"Format\":\"vtt\",\"Method\":\"External\"},{\"Format\":\"ass\",\"Method\":\"External\"},"
 "{\"Format\":\"ssa\",\"Method\":\"External\"},"
 "{\"Format\":\"pgssub\",\"Method\":\"Encode\"},{\"Format\":\"dvdsub\",\"Method\":\"Encode\"},"
 "{\"Format\":\"dvbsub\",\"Method\":\"Encode\"}]}";

static int playback_once(struct jf_session *s, const char *item_id, int64_t start_ticks,
                         int audio_index, int subtitle_index, struct jf_play *out, int force)
{
     memset(out, 0, sizeof *out);
     cJSON *b = cJSON_CreateObject();
     cJSON_AddItemToObject(b, "DeviceProfile", cJSON_Parse(DEVICE_PROFILE));
     cJSON_AddStringToObject(b, "UserId", s->user_id);
     cJSON_AddNumberToObject(b, "StartTimeTicks", (double)start_ticks);
     cJSON_AddNumberToObject(b, "MaxStreamingBitrate", 120000000);
     if (audio_index >= 0) cJSON_AddNumberToObject(b, "AudioStreamIndex", audio_index);
     if (subtitle_index != JF_SUB_DEFAULT)
          cJSON_AddNumberToObject(b, "SubtitleStreamIndex", subtitle_index);
     /* tests: $JF_FORCE_TRANSCODE=1, or the file
      * /var/local/vibe/jf.force-transcode, asks for the server's conversion
      * even when the file would play as it is */
     int force_tc = force || (getenv("JF_FORCE_TRANSCODE") && getenv("JF_FORCE_TRANSCODE")[0] == '1') ||
                    access("/var/local/vibe/jf.force-transcode", F_OK) == 0;
     cJSON_AddBoolToObject(b, "EnableDirectPlay", !force_tc);
     cJSON_AddBoolToObject(b, "EnableDirectStream", !force_tc);
     cJSON_AddBoolToObject(b, "EnableTranscoding", 1);
     cJSON_AddBoolToObject(b, "AllowVideoStreamCopy", 1);
     cJSON_AddBoolToObject(b, "AllowAudioStreamCopy", 1);
     cJSON_AddBoolToObject(b, "AutoOpenLiveStream", 1);
     char *body = cJSON_PrintUnformatted(b);
     cJSON_Delete(b);
     char path[256];
     snprintf(path, sizeof path, "/Items/%s/PlaybackInfo?userId=%s", item_id, s->user_id);
     cJSON *j = call(s, "POST", path, body, NULL);
     free(body);
     if (!j) return -1;

     const cJSON *srcs = cJSON_GetObjectItemCaseSensitive(j, "MediaSources");
     const cJSON *src = cJSON_IsArray(srcs) ? srcs->child : NULL;
     if (!src) {
          set_err("no playable source (%s)", jstr(j, "ErrorCode"));
          cJSON_Delete(j);
          return -1;
     }
     COPY(out->play_session_id, jstr(j, "PlaySessionId"));
     COPY(out->media_source_id, jstr(src, "Id"));
     COPY(out->container, jstr(src, "Container"));
     out->runtime_ticks = (int64_t)jnum(src, "RunTimeTicks", 0);
     out->audio_index = (int)jnum(src, "DefaultAudioStreamIndex", -1);
     out->default_audio = out->audio_index;
     out->subtitle_index = (int)jnum(src, "DefaultSubtitleStreamIndex", -1);
     if (audio_index >= 0) out->audio_index = audio_index;
     if (subtitle_index != JF_SUB_DEFAULT) out->subtitle_index = subtitle_index;

     const cJSON *ms;
     cJSON_ArrayForEach(ms, cJSON_GetObjectItemCaseSensitive(src, "MediaStreams")) {
          if (out->nstreams >= JF_MAX_STREAMS) break;
          struct jf_stream *st = &out->streams[out->nstreams++];
          st->index = (int)jnum(ms, "Index", -1);
          COPY(st->type, jstr(ms, "Type"));
          COPY(st->codec, jstr(ms, "Codec"));
          COPY(st->lang, jstr(ms, "Language"));
          COPY(st->title, jstr(ms, "DisplayTitle"));
          st->is_default = jbool(ms, "IsDefault");
          st->is_external = jbool(ms, "IsExternal");
          st->is_text = jbool(ms, "IsTextSubtitleStream");
          st->channels = (int)jnum(ms, "Channels", 0);
     }

     const char *tc = jstr(src, "TranscodingUrl");
     if (jbool(src, "SupportsDirectPlay") || (jbool(src, "SupportsDirectStream") && !tc[0])) {
          /* the file as it is: the TV seeks with HTTP ranges */
          out->method = jbool(src, "SupportsDirectPlay") ? JF_DIRECT_PLAY : JF_DIRECT_STREAM;
          snprintf(out->url, sizeof out->url,
                   "%s/Videos/%s/stream?static=true&mediaSourceId=%s&playSessionId=%s&api_key=%s",
                   s->server, item_id, out->media_source_id, out->play_session_id, s->token);
     } else if (tc[0]) {
          /* relative, carries its own ApiKey */
          out->method = JF_TRANSCODE;
          snprintf(out->url, sizeof out->url, "%s%s", s->server, tc);
     } else {
          set_err("server offered no way to play this");
          cJSON_Delete(j);
          return -1;
     }
     cJSON_Delete(j);
     return 0;
}

/* The TV's player takes the file's default audio track; another track
 * comes from the server's remux (video copied) with that track only */
int jf_playback(struct jf_session *s, const char *item_id, int64_t start_ticks,
                int audio_index, int subtitle_index, struct jf_play *out)
{
     int rc = playback_once(s, item_id, start_ticks, audio_index, subtitle_index, out, 0);
     if (rc == 0 && audio_index >= 0 && out->method != JF_TRANSCODE && audio_index != out->default_audio)
          rc = playback_once(s, item_id, start_ticks, audio_index, subtitle_index, out, 1);
     return rc;
}

static const char *method_name(int m)
{
     return m == JF_DIRECT_PLAY ? "DirectPlay" : m == JF_DIRECT_STREAM ? "DirectStream" : "Transcode";
}

int jf_report(struct jf_session *s, const char *item_id, const struct jf_play *p,
              int event, int64_t position_ticks, int paused)
{
     static const char *const PATHS[] = {
          "/Sessions/Playing", "/Sessions/Playing/Progress", "/Sessions/Playing/Stopped"
     };
     if (event < 0 || event > 2) return -1;
     cJSON *b = cJSON_CreateObject();
     cJSON_AddStringToObject(b, "ItemId", item_id);
     cJSON_AddStringToObject(b, "MediaSourceId", p->media_source_id);
     cJSON_AddStringToObject(b, "PlaySessionId", p->play_session_id);
     cJSON_AddNumberToObject(b, "PositionTicks", (double)position_ticks);
     cJSON_AddStringToObject(b, "PlayMethod", method_name(p->method));
     cJSON_AddBoolToObject(b, "CanSeek", 1);
     cJSON_AddBoolToObject(b, "IsPaused", paused);
     if (p->audio_index >= 0) cJSON_AddNumberToObject(b, "AudioStreamIndex", p->audio_index);
     if (p->subtitle_index >= 0) cJSON_AddNumberToObject(b, "SubtitleStreamIndex", p->subtitle_index);
     char *body = cJSON_PrintUnformatted(b);
     cJSON_Delete(b);
     cJSON *j = call(s, "POST", PATHS[event], body, NULL);
     free(body);
     if (!j) return -1;
     cJSON_Delete(j);
     return 0;
}

int jf_set_played(struct jf_session *s, const char *item_id, int played)
{
     char path[256];
     snprintf(path, sizeof path, "/UserPlayedItems/%s?userId=%s", item_id, s->user_id);
     cJSON *j = call(s, played ? "POST" : "DELETE", path, NULL, NULL);
     if (!j) return -1;
     cJSON_Delete(j);
     return 0;
}

void jf_stop_transcode(struct jf_session *s, const struct jf_play *p)
{
     if (p->method != JF_TRANSCODE) return;
     char path[256];
     snprintf(path, sizeof path, "/Videos/ActiveEncodings?deviceId=%s&playSessionId=%s",
              s->device_id, p->play_session_id);
     cJSON_Delete(call(s, "DELETE", path, NULL, NULL));
}
