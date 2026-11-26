/*
 * Jellyfin images: see jfimg.h.
 *
 * One fetch thread. The UI asks for the images it would draw; the thread
 * takes the most recently asked-for one first, so scrolling fast fetches
 * what is on screen now, not what was a second ago. Requests nobody asked
 * for again within STALE_S are dropped. Cache: MAX_ENT entries and
 * MAX_BYTES of pixels, least recently used out first; only the UI thread
 * adds or evicts, so a pointer jfimg_get returned stays good for the frame.
 */
#define _GNU_SOURCE
#include "jfimg.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
/* no thread-locals: with zig they pull in ld-linux-armhf.so.3 (__tls_get_addr),
 * which this TV does not have; only one thread decodes */
#define STBI_NO_THREAD_LOCALS
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

#define MAX_ENT   48
#define MAX_BYTES (24 * 1024 * 1024)
#define STALE_S   8.0
#define RETRY_S   60.0

enum { E_FREE, E_QUEUED, E_LOADING, E_READY, E_FAILED };

struct ent {
     int  state;
     char id[JF_ID], kind[16], tag[JF_ID];
     struct jfimg img;
     double asked;              /* last time the UI wanted it */
     double done;               /* when it failed (retry later) */
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond = PTHREAD_COND_INITIALIZER;
static struct ent E[MAX_ENT];
static size_t bytes;
static int changed, thread_up;
static struct jf_session sess;
static int have_sess;

void jfimg_session(const struct jf_session *s)
{
     pthread_mutex_lock(&lock);
     sess = *s;
     have_sess = s->server[0] && s->token[0];
     pthread_mutex_unlock(&lock);
}

int jfimg_changed(void)
{
     pthread_mutex_lock(&lock);
     int c = changed;
     changed = 0;
     pthread_mutex_unlock(&lock);
     return c;
}

/* bilinear, RGBA in -> ARGB out */
static uint32_t *scale(const unsigned char *src, int sw, int sh, int dw, int dh)
{
     uint32_t *out = malloc((size_t)dw * dh * 4);
     if (!out) return NULL;
     for (int y = 0; y < dh; y++) {
          float fy = (y + 0.5f) * sh / dh - 0.5f;
          if (fy < 0) fy = 0;
          int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
          float ty = fy - y0;
          for (int x = 0; x < dw; x++) {
               float fx = (x + 0.5f) * sw / dw - 0.5f;
               if (fx < 0) fx = 0;
               int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
               float tx = fx - x0;
               const unsigned char *a = src + ((size_t)y0 * sw + x0) * 4, *b = src + ((size_t)y0 * sw + x1) * 4;
               const unsigned char *c = src + ((size_t)y1 * sw + x0) * 4, *d = src + ((size_t)y1 * sw + x1) * 4;
               uint32_t px = 0xff000000u;
               for (int k = 0; k < 3; k++) {
                    float v = (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty;
                    px |= (uint32_t)(v + 0.5f) << (16 - 8 * k);
               }
               out[(size_t)y * dw + x] = px;
          }
     }
     return out;
}

static uint32_t *decode(const char *data, size_t len, int dw, int dh)
{
     int w, h, n;
     unsigned char *rgba = stbi_load_from_memory((const unsigned char *)data, (int)len, &w, &h, &n, 4);
     if (!rgba) return NULL;
     uint32_t *out;
     if (w == dw && h == dh) {
          out = malloc((size_t)w * h * 4);
          if (out)
               for (size_t i = 0; i < (size_t)w * h; i++)
                    out[i] = 0xff000000u | (uint32_t)rgba[i * 4] << 16 | (uint32_t)rgba[i * 4 + 1] << 8 | rgba[i * 4 + 2];
     } else {
          out = scale(rgba, w, h, dw, dh);
     }
     stbi_image_free(rgba);
     return out;
}

static void *fetcher(void *arg)
{
     (void)arg;
     for (;;) {
          pthread_mutex_lock(&lock);
          struct ent *e = NULL;
          for (;;) {
               double now = vibe_now_s();
               e = NULL;
               for (int i = 0; i < MAX_ENT; i++) {
                    if (E[i].state != E_QUEUED) continue;
                    if (now - E[i].asked > STALE_S) { E[i].state = E_FREE; continue; }
                    if (!e || E[i].asked > e->asked) e = &E[i];
               }
               if (e && have_sess) break;
               pthread_cond_wait(&cond, &lock);
          }
          e->state = E_LOADING;
          struct jf_session s = sess;
          char id[JF_ID], kind[16], tag[JF_ID];
          int w = e->img.w, h = e->img.h;
          snprintf(id, sizeof id, "%s", e->id);
          snprintf(kind, sizeof kind, "%s", e->kind);
          snprintf(tag, sizeof tag, "%s", e->tag);
          pthread_mutex_unlock(&lock);

          char url[JF_URL_MAX], *data = NULL;
          size_t len = 0;
          uint32_t *px = NULL;
          jf_image_url(&s, id, kind, tag, w, h, url);
          if (jf_fetch(&s, url, &data, &len) == 0)
               px = decode(data, len, w, h);
          else
               fprintf(stderr, "jellyfin: image %s/%s: %s\n", id, kind, jf_error());
          free(data);

          pthread_mutex_lock(&lock);
          /* the entry is ours while LOADING: the UI never evicts those */
          e->img.px = px;
          e->state = px ? E_READY : E_FAILED;
          e->done = vibe_now_s();
          if (px) bytes += (size_t)w * h * 4;
          changed = 1;
          pthread_mutex_unlock(&lock);
     }
     return NULL;
}

/* UI thread, lock held: make room for one more entry of `need` bytes */
static struct ent *slot(size_t need)
{
     for (;;) {
          struct ent *free_e = NULL, *lru = NULL;
          for (int i = 0; i < MAX_ENT; i++) {
               if (E[i].state == E_FREE) { if (!free_e) free_e = &E[i]; continue; }
               if (E[i].state == E_LOADING || E[i].state == E_QUEUED) continue;
               if (!lru || E[i].asked < lru->asked) lru = &E[i];
          }
          if (free_e && bytes + need <= MAX_BYTES) return free_e;
          if (!lru) return free_e;              /* all busy: no eviction possible */
          if (lru->img.px) bytes -= (size_t)lru->img.w * lru->img.h * 4;
          free(lru->img.px);
          memset(lru, 0, sizeof *lru);
     }
}

const struct jfimg *jfimg_get(const char *id, const char *kind, const char *tag, int w, int h)
{
     if (!id || !id[0] || w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
     double now = vibe_now_s();
     const struct jfimg *out = NULL;
     pthread_mutex_lock(&lock);
     if (!thread_up) {
          pthread_t t;
          if (pthread_create(&t, NULL, fetcher, NULL) == 0) { pthread_detach(t); thread_up = 1; }
     }
     struct ent *e = NULL;
     for (int i = 0; i < MAX_ENT; i++)
          if (E[i].state != E_FREE && E[i].img.w == w && E[i].img.h == h &&
              !strcmp(E[i].id, id) && !strcmp(E[i].kind, kind)) { e = &E[i]; break; }
     if (e) {
          e->asked = now;
          if (e->state == E_READY) out = &e->img;
          else if (e->state == E_FAILED && now - e->done > RETRY_S) { e->state = E_QUEUED; pthread_cond_signal(&cond); }
     } else if ((e = slot((size_t)w * h * 4)) != NULL) {
          memset(e, 0, sizeof *e);
          snprintf(e->id, sizeof e->id, "%s", id);
          snprintf(e->kind, sizeof e->kind, "%s", kind);
          snprintf(e->tag, sizeof e->tag, "%s", tag ? tag : "");
          e->img.w = w;
          e->img.h = h;
          e->asked = now;
          e->state = E_QUEUED;
          pthread_cond_signal(&cond);
     }
     pthread_mutex_unlock(&lock);
     return out;
}
