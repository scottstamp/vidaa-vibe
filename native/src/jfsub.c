/* SRT subtitles: see jfsub.h */
#define _GNU_SOURCE
#include "jfsub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* "HH:MM:SS,mmm" (or "."; hours optional, as WebVTT allows) -> ms; -1 bad */
static int parse_time(const char *p, const char **end)
{
     int v[4] = { 0 }, n = 0;
     while (n < 4) {
          if (*p < '0' || *p > '9') return -1;
          int x = 0;
          while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
          v[n++] = x;
          if (*p == ':' && n < 3) { p++; continue; }
          if ((*p == ',' || *p == '.') && n >= 2) { p++; continue; }
          break;
     }
     if (end) *end = p;
     if (n == 4) return ((v[0] * 60 + v[1]) * 60 + v[2]) * 1000 + v[3];
     if (n == 3) return (v[0] * 60 + v[1]) * 1000 + v[2];           /* MM:SS.mmm */
     return -1;
}

/* drop <i>, </b>, <font ...>, {\an8} and the like; keep the words */
static void strip_tags(char *s)
{
     char *w = s;
     for (char *r = s; *r; ) {
          if (*r == '<') { char *e = strchr(r, '>'); if (e) { r = e + 1; continue; } }
          if (*r == '{' && r[1] == '\\') { char *e = strchr(r, '}'); if (e) { r = e + 1; continue; } }
          *w++ = *r++;
     }
     *w = 0;
}

static int by_start(const void *a, const void *b)
{
     const struct jf_cue *x = a, *y = b;
     return x->start_ms - y->start_ms;
}

int jf_subs_parse(const char *data, size_t len, struct jf_subs *out)
{
     memset(out, 0, sizeof *out);
     char *buf = malloc(len + 1);
     if (!buf) return -1;
     memcpy(buf, data, len);
     buf[len] = 0;
     char *p = buf;
     if ((unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb && (unsigned char)p[2] == 0xbf) p += 3;
     /* lines, blank ones kept, "\r" dropped */
     int nl = 1;
     for (char *q = p; *q; q++) if (*q == '\n') nl++;
     char **L = malloc((size_t)nl * sizeof *L);
     if (!L) { free(buf); return -1; }
     nl = 0;
     for (char *q = p; q; ) {
          char *e = strchr(q, '\n');
          if (e) *e = 0;
          size_t l = strlen(q);
          if (l && q[l - 1] == '\r') q[l - 1] = 0;
          L[nl++] = q;
          q = e ? e + 1 : NULL;
     }
     int cap = 0;
     for (int i = 0; i < nl; i++) {
          char *arrow = strstr(L[i], "-->");
          if (!arrow) continue;
          const char *q = L[i];
          while (*q == ' ') q++;
          int st = parse_time(q, NULL);
          q = arrow + 3;
          while (*q == ' ') q++;
          int en = parse_time(q, NULL);
          char text[1024] = "";
          size_t tl = 0;
          int j = i + 1;
          for (; j < nl && L[j][0]; j++) {          /* the text: up to a blank line */
               if (strstr(L[j], "-->")) break;
               strip_tags(L[j]);
               char *t = L[j];                    /* trimmed: lines are centred */
               while (*t == ' ' || *t == '\t') t++;
               size_t tlen = strlen(t);
               while (tlen && (t[tlen - 1] == ' ' || t[tlen - 1] == '\t')) t[--tlen] = 0;
               if (!t[0]) continue;
               tl += (size_t)snprintf(text + tl, sizeof text - tl, "%s%s", tl ? "\n" : "", t);
               if (tl >= sizeof text - 1) tl = sizeof text - 1;
          }
          i = j - 1;
          if (st < 0 || en <= st || !text[0]) continue;
          if (out->n == cap) {
               cap = cap ? cap * 2 : 256;
               struct jf_cue *nc = realloc(out->c, (size_t)cap * sizeof *nc);
               if (!nc) break;
               out->c = nc;
          }
          out->c[out->n].start_ms = st;
          out->c[out->n].end_ms = en;
          out->c[out->n].text = strdup(text);
          if (out->c[out->n].text) out->n++;
     }
     free(L);
     free(buf);
     if (out->n > 1) qsort(out->c, (size_t)out->n, sizeof *out->c, by_start);
     return 0;
}

const struct jf_cue *jf_subs_at(const struct jf_subs *s, int ms)
{
     if (!s || !s->n) return NULL;
     int lo = 0, hi = s->n - 1, k = -1;
     while (lo <= hi) {                    /* the last cue starting at or before ms */
          int mid = (lo + hi) / 2;
          if (s->c[mid].start_ms <= ms) { k = mid; lo = mid + 1; } else hi = mid - 1;
     }
     /* overlapping cues: the latest that is still on */
     for (int i = k; i >= 0 && i > k - 8; i--)
          if (s->c[i].end_ms > ms) return &s->c[i];
     return NULL;
}

void jf_subs_free(struct jf_subs *s)
{
     for (int i = 0; i < s->n; i++) free(s->c[i].text);
     free(s->c);
     memset(s, 0, sizeof *s);
}
