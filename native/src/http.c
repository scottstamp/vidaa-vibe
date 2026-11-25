/*
 * HTTP over the TV's libcurl (dlopen). The option numbers are libcurl's
 * stable ABI values (curl/curl.h: CURLOPTTYPE_LONG 0, OBJECTPOINT 10000,
 * FUNCTIONPOINT 20000; CURLINFO_LONG 0x200000).
 */
#include "http.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void CURL;
struct curl_slist;

#define O_WRITEDATA        10001L
#define O_URL              10002L
#define O_POSTFIELDS       10015L
#define O_HTTPHEADER       10023L
#define O_CUSTOMREQUEST    10036L
#define O_ERRORBUFFER      10010L
#define O_WRITEFUNCTION    20011L
#define O_POST                47L
#define O_FOLLOWLOCATION      52L
#define O_POSTFIELDSIZE       60L
#define O_NOSIGNAL            99L
#define O_TIMEOUT_MS         155L
#define O_CONNECTTIMEOUT_MS  156L
#define O_ACCEPT_ENCODING  10102L
#define I_RESPONSE_CODE  0x200002L
#define CURL_GLOBAL_ALL 3L

static struct {
     void *h;
     int   (*global_init)(long);
     CURL *(*easy_init)(void);
     int   (*setopt)(CURL *, long, ...);
     int   (*perform)(CURL *);
     int   (*getinfo)(CURL *, long, ...);
     void  (*cleanup)(CURL *);
     struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
     void  (*slist_free_all)(struct curl_slist *);
     const char *(*strerror)(int);
} C;

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int init_rc = -1;

static void load(void)
{
     /* the TV: /usr/local/lib (curl 7.x); then whatever the loader finds */
     static const char *const paths[] = {
          "/usr/local/lib/libcurl.so.4", "/vendor/lib/libcurl.so.4", "libcurl.so.4", NULL
     };
     for (int i = 0; paths[i] && !C.h; i++)
          C.h = dlopen(paths[i], RTLD_NOW | RTLD_LOCAL);
     if (!C.h) { fprintf(stderr, "http: no libcurl: %s\n", dlerror()); return; }
#define SYM(f, n) if (!(*(void **)&C.f = dlsym(C.h, n))) { fprintf(stderr, "http: no %s\n", n); return; }
     SYM(global_init, "curl_global_init");
     SYM(easy_init, "curl_easy_init");
     SYM(setopt, "curl_easy_setopt");
     SYM(perform, "curl_easy_perform");
     SYM(getinfo, "curl_easy_getinfo");
     SYM(cleanup, "curl_easy_cleanup");
     SYM(slist_append, "curl_slist_append");
     SYM(slist_free_all, "curl_slist_free_all");
     SYM(strerror, "curl_easy_strerror");
#undef SYM
     if (C.global_init(CURL_GLOBAL_ALL) != 0) { fprintf(stderr, "http: curl_global_init failed\n"); return; }
     init_rc = 0;
}

int http_init(void)
{
     pthread_once(&once, load);
     return init_rc;
}

static size_t on_data(char *p, size_t sz, size_t n, void *ud)
{
     struct http_buf *b = ud;
     size_t add = sz * n;
     char *d = realloc(b->data, b->len + add + 1);
     if (!d) return 0;
     memcpy(d + b->len, p, add);
     b->data = d;
     b->len += add;
     d[b->len] = 0;
     return add;
}

int http_request(const char *method, const char *url, const char *const *headers,
                 const char *body, long timeout_ms,
                 struct http_buf *out, long *status, char *err)
{
     char ebuf[128] = "";
     *status = 0;
     out->data = NULL; out->len = 0;
     if (http_init() != 0) { if (err) snprintf(err, 128, "libcurl not available"); return -1; }
     CURL *e = C.easy_init();
     if (!e) { if (err) snprintf(err, 128, "curl_easy_init failed"); return -1; }
     struct curl_slist *hl = NULL;
     for (int i = 0; headers && headers[i]; i++) hl = C.slist_append(hl, headers[i]);

     C.setopt(e, O_URL, url);
     C.setopt(e, O_NOSIGNAL, 1L);
     C.setopt(e, O_FOLLOWLOCATION, 1L);
     C.setopt(e, O_CONNECTTIMEOUT_MS, 4000L);
     C.setopt(e, O_TIMEOUT_MS, timeout_ms > 0 ? timeout_ms : 15000L);
     C.setopt(e, O_ACCEPT_ENCODING, "");           /* gzip if built in */
     C.setopt(e, O_WRITEFUNCTION, on_data);
     C.setopt(e, O_WRITEDATA, out);
     C.setopt(e, O_ERRORBUFFER, ebuf);
     if (hl) C.setopt(e, O_HTTPHEADER, hl);
     if (strcmp(method, "POST") == 0) {
          C.setopt(e, O_POST, 1L);
          C.setopt(e, O_POSTFIELDS, body ? body : "");
          C.setopt(e, O_POSTFIELDSIZE, (long)(body ? strlen(body) : 0));
     } else if (strcmp(method, "GET") != 0) {
          C.setopt(e, O_CUSTOMREQUEST, method);
          if (body) {
               C.setopt(e, O_POSTFIELDS, body);
               C.setopt(e, O_POSTFIELDSIZE, (long)strlen(body));
          }
     }
     int rc = C.perform(e);
     if (rc == 0) C.getinfo(e, I_RESPONSE_CODE, status);
     else if (err) snprintf(err, 128, "%s", ebuf[0] ? ebuf : C.strerror(rc));
     C.cleanup(e);
     if (hl) C.slist_free_all(hl);
     if (rc != 0) { http_buf_free(out); return -1; }
     if (!out->data) { out->data = calloc(1, 1); out->len = 0; }
     return 0;
}

void http_buf_free(struct http_buf *b)
{
     free(b->data);
     b->data = NULL;
     b->len = 0;
}

char *http_escape(const char *s, char *out, size_t n)
{
     static const char hex[] = "0123456789ABCDEF";
     size_t o = 0;
     for (; *s && o + 4 < n; s++) {
          unsigned char ch = (unsigned char)*s;
          if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.' || ch == '~') {
               out[o++] = (char)ch;
          } else {
               out[o++] = '%';
               out[o++] = hex[ch >> 4];
               out[o++] = hex[ch & 15];
          }
     }
     out[o] = 0;
     return out;
}
