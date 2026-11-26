/*
 * vibe-jf: command-line Jellyfin client for testing jf.c on the PC (WSL) or
 * the TV, without the UI.
 *
 *   vibe-jf [-s STATE] discover
 *   vibe-jf [-s STATE] server URL          check the address, store it
 *   vibe-jf [-s STATE] qc                  Quick Connect: prints a code, waits
 *                                          for approval (5 min), stores token
 *   vibe-jf [-s STATE] login USER          password from $JF_PW
 *   vibe-jf [-s STATE] logout
 *   vibe-jf [-s STATE] views | resume | nextup
 *   vibe-jf [-s STATE] latest PARENT | ls PARENT [START] | seasons SERIES
 *   vibe-jf [-s STATE] episodes SERIES SEASON | item ID
 *   vibe-jf [-s STATE] play ID [START_SECONDS]   negotiate, print the URL
 *   vibe-jf [-s STATE] image ID OUT.jpg [W H]
 *   vibe-jf [-s STATE] sub ID INDEX            a text track as SRT: count, first cues
 *
 * STATE defaults to /var/local/vibe/state/jellyfin (the TV) or $JF_STATE.
 */
#include "jf.h"
#include "jfsub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *state = "/var/local/vibe/state/jellyfin";
static struct jf_session S;

static int fail(const char *what)
{
     fprintf(stderr, "%s: %s\n", what, jf_error());
     return 1;
}

static void fmt_ticks(int64_t t, char *out, size_t n)
{
     long s = (long)(t / 10000000);
     snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
}

static void print_list(struct jf_list *l)
{
     for (int i = 0; i < l->n; i++) {
          struct jf_item *it = &l->items[i];
          char rt[16] = "", pos[16] = "";
          if (it->runtime_ticks) fmt_ticks(it->runtime_ticks, rt, sizeof rt);
          if (it->position_ticks) fmt_ticks(it->position_ticks, pos, sizeof pos);
          printf("%s  %-12s %-40.40s", it->id, it->type_name, it->name);
          if (it->series_name[0]) printf("  [%s S%dE%d]", it->series_name, it->season, it->episode);
          if (it->year) printf("  %d", it->year);
          if (rt[0]) printf("  %s", rt);
          if (pos[0]) printf("  at %s", pos);
          if (it->played) printf("  played");
          if (it->collection[0]) printf("  (%s)", it->collection);
          printf("%s\n", it->primary_tag[0] ? "  [img]" : "");
     }
     printf("-- %d of %d\n", l->n, l->total);
     jf_list_free(l);
}

static int need_login(void)
{
     if (!S.server[0]) { fprintf(stderr, "no server: run `vibe-jf server URL`\n"); return 1; }
     if (!S.token[0]) { fprintf(stderr, "not signed in: run `vibe-jf qc`\n"); return 1; }
     return 0;
}

int main(int argc, char **argv)
{
     int a = 1;
     if (getenv("JF_STATE")) state = getenv("JF_STATE");
     if (a + 1 < argc && strcmp(argv[a], "-s") == 0) { state = argv[a + 1]; a += 2; }
     if (a >= argc) { fprintf(stderr, "usage: see the top of vibe-jf.c\n"); return 2; }
     const char *cmd = argv[a++];
     jf_session_load(&S, state);           /* missing file is fine */
     jf_session_init(&S);
     struct jf_list l;

     if (strcmp(cmd, "discover") == 0) {
          struct jf_server sv[16];
          int n = jf_discover(sv, 16, 2000);
          for (int i = 0; i < n; i++) printf("%s  %s  %s\n", sv[i].address, sv[i].name, sv[i].id);
          if (!n) fprintf(stderr, "none found %s\n", jf_error());
          return n ? 0 : 1;
     }
     if (strcmp(cmd, "server") == 0 && a < argc) {
          struct jf_session t = S;
          snprintf(t.server, sizeof t.server, "%s", argv[a]);
          if (jf_public_info(&t) != 0) return fail("server");
          if (strcmp(t.server_id, S.server_id) != 0) t.token[0] = t.user_id[0] = t.user_name[0] = 0;
          S = t;
          printf("%s: %s (%s)\n", S.server, S.server_name, S.server_id);
          return jf_session_save(&S, state) ? fail("save") : 0;
     }
     if (strcmp(cmd, "qc") == 0) {
          if (!S.server[0]) return fail("no server");
          if (!jf_qc_enabled(&S)) { fprintf(stderr, "Quick Connect is off on this server\n"); return 1; }
          char code[16], secret[128];
          if (jf_qc_initiate(&S, code, secret) != 0) return fail("qc");
          printf("Quick Connect code: %s (approve it in another Jellyfin app)\n", code);
          fflush(stdout);
          for (int i = 0; i < 150; i++) {
               int r = jf_qc_poll(&S, secret);
               if (r < 0) return fail("qc poll");
               if (r == 1) {
                    if (jf_qc_finish(&S, secret) != 0) return fail("qc finish");
                    printf("signed in as %s\n", S.user_name);
                    return jf_session_save(&S, state) ? fail("save") : 0;
               }
               sleep(2);
          }
          fprintf(stderr, "timed out\n");
          return 1;
     }
     if (strcmp(cmd, "login") == 0 && a < argc) {
          if (jf_login_password(&S, argv[a], getenv("JF_PW")) != 0) return fail("login");
          printf("signed in as %s\n", S.user_name);
          return jf_session_save(&S, state) ? fail("save") : 0;
     }
     if (need_login()) return 1;
     if (strcmp(cmd, "logout") == 0) { jf_logout(&S); return jf_session_save(&S, state); }
     if (strcmp(cmd, "views") == 0)  { if (jf_views(&S, &l)) return fail(cmd); print_list(&l); return 0; }
     if (strcmp(cmd, "resume") == 0) { if (jf_resume(&S, 20, &l)) return fail(cmd); print_list(&l); return 0; }
     if (strcmp(cmd, "nextup") == 0) { if (jf_next_up(&S, 20, &l)) return fail(cmd); print_list(&l); return 0; }
     if (strcmp(cmd, "latest") == 0 && a < argc) {
          if (jf_latest(&S, argv[a], 20, &l)) return fail(cmd);
          print_list(&l); return 0;
     }
     if (strcmp(cmd, "ls") == 0 && a < argc) {
          if (jf_children(&S, argv[a], a + 1 < argc ? atoi(argv[a + 1]) : 0, 100, &l)) return fail(cmd);
          print_list(&l); return 0;
     }
     if (strcmp(cmd, "seasons") == 0 && a < argc) {
          if (jf_seasons(&S, argv[a], &l)) return fail(cmd);
          print_list(&l); return 0;
     }
     if (strcmp(cmd, "episodes") == 0 && a + 1 < argc) {
          if (jf_episodes(&S, argv[a], argv[a + 1], &l)) return fail(cmd);
          print_list(&l); return 0;
     }
     if (strcmp(cmd, "item") == 0 && a < argc) {
          struct jf_item it;
          if (jf_item(&S, argv[a], &it)) return fail(cmd);
          printf("%s %s %s\n%s\n", it.id, it.type_name, it.name, it.overview ? it.overview : "");
          free(it.overview);
          return 0;
     }
     if (strcmp(cmd, "play") == 0 && a < argc) {
          static struct jf_play p;
          int64_t start = a + 1 < argc ? (int64_t)atoll(argv[a + 1]) * 10000000 : 0;
          if (jf_playback(&S, argv[a], start, -1, JF_SUB_DEFAULT, &p)) return fail(cmd);
          static const char *const M[] = { "direct play", "direct stream", "transcode" };
          printf("method: %s\ncontainer: %s\nsession: %s\nurl: %s\n", M[p.method], p.container,
                 p.play_session_id, p.url);
          for (int i = 0; i < p.nstreams; i++) {
               struct jf_stream *st = &p.streams[i];
               printf("  #%d %-8s %-8s %-4s %s%s%s\n", st->index, st->type, st->codec, st->lang,
                      st->title, st->is_default ? " (default)" : "", st->is_external ? " (external)" : "");
          }
          printf("audio %d, subtitles %d\n", p.audio_index, p.subtitle_index);
          return 0;
     }
     if (strcmp(cmd, "sub") == 0 && a + 1 < argc) {
          static struct jf_play p;
          if (jf_playback(&S, argv[a], 0, -1, JF_SUB_DEFAULT, &p)) return fail(cmd);
          char *d;
          size_t n;
          if (jf_subtitle(&S, argv[a], p.media_source_id, atoi(argv[a + 1]), &d, &n)) return fail(cmd);
          struct jf_subs subs;
          jf_subs_parse(d, n, &subs);
          printf("%zu bytes, %d cues\n", n, subs.n);
          for (int i = 0; i < subs.n && i < 5; i++)
               printf("  %d-%d %s\n", subs.c[i].start_ms, subs.c[i].end_ms, subs.c[i].text);
          jf_subs_free(&subs);
          free(d);
          return 0;
     }
     if (strcmp(cmd, "image") == 0 && a + 1 < argc) {
          char url[JF_URL_MAX], *d;
          size_t n;
          int w = a + 3 < argc ? atoi(argv[a + 2]) : 300, h = a + 3 < argc ? atoi(argv[a + 3]) : 450;
          jf_image_url(&S, argv[a], "Primary", "", w, h, url);
          if (jf_fetch(&S, url, &d, &n)) return fail(cmd);
          FILE *f = fopen(argv[a + 1], "wb");
          if (!f) { perror(argv[a + 1]); free(d); return 1; }
          fwrite(d, 1, n, f);
          fclose(f);
          free(d);
          printf("%zu bytes\n", n);
          return 0;
     }
     fprintf(stderr, "unknown command %s\n", cmd);
     return 2;
}
