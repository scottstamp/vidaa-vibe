/*
 * The YouTube helper's API, for the panel's worker thread: see yt.h.
 */
#define _GNU_SOURCE
#include "yt.h"
#include "http.h"
#include "../third_party/cjson/cJSON.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char base[200] = YT_DEFAULT_BASE;
static int  maxh = 1080;
static char err[200];          /* the worker's (one thread): no thread-locals here */

static void set_err(const char *fmt, ...)
{
     va_list ap;
     va_start(ap, fmt);
     vsnprintf(err, sizeof err, fmt, ap);
     va_end(ap);
}

const char *yt_error(void) { return err; }
const char *yt_base(void) { return base; }
int yt_maxh(void) { return maxh; }

void yt_config_load(void)
{
     FILE *f = fopen(getenv("YT_STATE") ? getenv("YT_STATE") : YT_STATE_FILE, "r");
     if (!f) return;
     char line[300];
     while (fgets(line, sizeof line, f)) {
          line[strcspn(line, "\r\n")] = 0;
          if (!strncmp(line, "helper=", 7) && !strncmp(line + 7, "http://", 7))
               snprintf(base, sizeof base, "%s", line + 7);
          else if (!strncmp(line, "maxh=", 5) && atoi(line + 5) >= 144)
               maxh = atoi(line + 5);
     }
     fclose(f);
     size_t l = strlen(base);
     while (l && base[l - 1] == '/') base[--l] = 0;
}

/* GET/POST base+path -> parsed JSON (caller deletes), NULL on failure */
static cJSON *call(const char *method, const char *path, long timeout_ms)
{
     char url[1024], e[160];
     snprintf(url, sizeof url, "%s%s", base, path);
     struct http_buf b;
     long st = 0;
     if (http_request(method, url, NULL, NULL, timeout_ms, &b, &st, e) != 0) {
          set_err("helper unreachable (%s)", e);
          return NULL;
     }
     cJSON *j = cJSON_Parse(b.data ? b.data : "");
     http_buf_free(&b);
     if (st != 200) {
          const cJSON *m = j ? cJSON_GetObjectItemCaseSensitive(j, "error") : NULL;
          set_err("%s", cJSON_IsString(m) ? m->valuestring : "helper error");
          if (!cJSON_IsString(m)) set_err("helper: HTTP %ld", st);
          cJSON_Delete(j);
          return NULL;
     }
     if (!j) set_err("helper: bad reply");
     return j;
}

static const char *str(const cJSON *o, const char *k)
{
     const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
     return cJSON_IsString(v) ? v->valuestring : "";
}

static double num(const cJSON *o, const char *k)
{
     const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
     return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int flag(const cJSON *o, const char *k)
{
     return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, k));
}

static void item_of(const cJSON *o, struct yt_item *it)
{
     memset(it, 0, sizeof *it);
     snprintf(it->id, sizeof it->id, "%s", str(o, "id"));
     snprintf(it->title, sizeof it->title, "%s", str(o, "title"));
     snprintf(it->channel, sizeof it->channel, "%s", str(o, "channel"));
     snprintf(it->channel_id, sizeof it->channel_id, "%s", str(o, "channel_id"));
     snprintf(it->published, sizeof it->published, "%s", str(o, "published"));
     it->duration = (int)num(o, "duration");
     it->views = (long long)num(o, "views");
     it->live = flag(o, "live");
     it->position = num(o, "position");
}

static int list_of(const cJSON *arr, struct yt_list *out)
{
     out->items = NULL;
     out->n = 0;
     int n = cJSON_GetArraySize(arr);
     if (n <= 0) return 0;
     out->items = calloc((size_t)n, sizeof *out->items);
     if (!out->items) { set_err("out of memory"); return -1; }
     const cJSON *o;
     cJSON_ArrayForEach(o, arr) {
          if (strlen(str(o, "id")) != 11) continue;
          item_of(o, &out->items[out->n++]);
     }
     return 0;
}

int yt_home(struct yt_home *out)
{
     memset(out, 0, sizeof *out);
     cJSON *j = call("GET", "/api/home", 60000);
     if (!j) return -1;
     const cJSON *s;
     cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "shelves")) {
          if (out->n >= YT_MAX_SHELVES) break;
          struct yt_shelf *h = &out->sh[out->n];
          snprintf(h->title, sizeof h->title, "%s", str(s, "title"));
          snprintf(h->channel_id, sizeof h->channel_id, "%s", str(s, "channel_id"));
          list_of(cJSON_GetObjectItemCaseSensitive(s, "items"), &h->l);
          if (h->l.n) out->n++;
          else yt_list_free(&h->l);
     }
     cJSON_Delete(j);
     return 0;
}

int yt_search(const char *q, int n, struct yt_list *out)
{
     char esc[400], path[500];
     snprintf(path, sizeof path, "/api/search?n=%d&q=%s", n, http_escape(q, esc, sizeof esc));
     cJSON *j = call("GET", path, 30000);
     if (!j) { out->items = NULL; out->n = 0; return -1; }
     int r = list_of(cJSON_GetObjectItemCaseSensitive(j, "items"), out);
     cJSON_Delete(j);
     return r;
}

int yt_history(int n, struct yt_list *out)
{
     char path[64];
     snprintf(path, sizeof path, "/api/history?n=%d", n);
     cJSON *j = call("GET", path, 15000);
     if (!j) { out->items = NULL; out->n = 0; return -1; }
     int r = list_of(cJSON_GetObjectItemCaseSensitive(j, "items"), out);
     cJSON_Delete(j);
     return r;
}

int yt_channel(const char *id, int n, struct yt_channel *out)
{
     memset(out, 0, sizeof *out);
     char path[128];
     snprintf(path, sizeof path, "/api/channel?n=%d&id=%s", n, id);
     cJSON *j = call("GET", path, 45000);
     if (!j) return -1;
     snprintf(out->id, sizeof out->id, "%s", str(j, "id"));
     snprintf(out->title, sizeof out->title, "%s", str(j, "title"));
     out->followed = flag(j, "followed");
     int r = list_of(cJSON_GetObjectItemCaseSensitive(j, "items"), &out->l);
     cJSON_Delete(j);
     return r;
}

int yt_auto_skip(const char *c)
{
     return !strcmp(c, "sponsor") || !strcmp(c, "selfpromo") || !strcmp(c, "interaction") ||
            !strcmp(c, "music_offtopic");
}

const char *yt_cat_name(const char *c)
{
     static const char *const map[][2] = {
          { "sponsor", "Sponsor" }, { "selfpromo", "Self-promotion" },
          { "interaction", "Interaction reminder" }, { "intro", "Intro" }, { "outro", "Outro" },
          { "preview", "Preview" }, { "music_offtopic", "Non-music section" },
          { "filler", "Filler" }, { "poi_highlight", "Highlight" },
     };
     for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
          if (!strcmp(c, map[i][0])) return map[i][1];
     return c;
}

int yt_video(const char *id, struct yt_video *out)
{
     memset(out, 0, sizeof *out);
     char path[64];
     snprintf(path, sizeof path, "/api/video?id=%s", id);
     cJSON *j = call("GET", path, 60000);
     if (!j) return -1;
     item_of(j, &out->it);
     const char *d = str(j, "description");
     out->description = d[0] ? strdup(d) : NULL;
     out->followed = flag(j, "followed");
     const cJSON *s;
     cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "segments")) {
          if (out->nsegs >= YT_MAX_SEGS) break;
          if (strcmp(str(s, "action"), "skip") && strcmp(str(s, "category"), "poi_highlight")) continue;
          struct yt_seg *g = &out->segs[out->nsegs++];
          g->start = num(s, "start");
          g->end = num(s, "end");
          snprintf(g->category, sizeof g->category, "%s", str(s, "category"));
          g->skip = yt_auto_skip(g->category) && g->end > g->start + 0.5;
     }
     cJSON_Delete(j);
     return 0;
}

int yt_play(const char *id, int mh, struct yt_play *out)
{
     memset(out, 0, sizeof *out);
     char path[80];
     snprintf(path, sizeof path, "/api/play?id=%s&maxh=%d", id, mh);
     cJSON *j = call("GET", path, 60000);
     if (!j) return -1;
     snprintf(out->url, sizeof out->url, "%s", str(j, "url"));
     snprintf(out->mime, sizeof out->mime, "%s", str(j, "mime"));
     out->duration = (int)num(j, "duration");
     out->live = flag(j, "live");
     cJSON_Delete(j);
     if (!out->url[0]) { set_err("no stream"); return -1; }
     return 0;
}

int yt_progress(const char *id, double pos, double dur)
{
     char path[96];
     snprintf(path, sizeof path, "/api/progress?id=%s&pos=%.1f&dur=%.1f", id, pos, dur);
     cJSON *j = call("POST", path, 10000);
     cJSON_Delete(j);
     return j ? 0 : -1;
}

int yt_follow(const char *cid, int on)
{
     char path[96];
     snprintf(path, sizeof path, "/api/follow?id=%s&on=%d", cid, on ? 1 : 0);
     cJSON *j = call("POST", path, 45000);
     cJSON_Delete(j);
     return j ? 0 : -1;
}

void yt_thumb_url(const char *id, char size, char *out, int n)
{
     snprintf(out, (size_t)n, "%s/thumb/%s.jpg?s=%c", base, id, size);
}

void yt_list_free(struct yt_list *l)
{
     free(l->items);
     l->items = NULL;
     l->n = 0;
}

void yt_home_free(struct yt_home *h)
{
     for (int i = 0; i < h->n; i++) yt_list_free(&h->sh[i].l);
     h->n = 0;
}

void yt_video_free(struct yt_video *v)
{
     free(v->description);
     v->description = NULL;
}
