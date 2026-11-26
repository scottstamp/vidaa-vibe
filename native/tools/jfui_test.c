/*
 * jfui_test: drives the Jellyfin panel (jfui.c) without DirectFB, for tests
 * over SSH that show nothing on screen. Shapes go to a RAM canvas; text is
 * printed instead of drawn, one block per frame.
 *
 *   JF_STATE=/tmp/jfstate jfui_test STEP...
 * STEP: up down left right ok back  - a key
 *       wait:S                      - run S seconds (results, polls)
 * Never run a step that starts playback unless the owner is watching.
 * Signing out is refused here (JFUI_TEST): it revokes the token on the
 * server, and a test runs with a copy of the UI's session (2026-11-26 it
 * did, and the owner had to sign in again).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/common.h"
#include "../src/jfui.h"

static int frame;

static void t_text(void *ctx, IDirectFBFont *f, int x, int y, int h, uint32_t c,
                   const char *s, int align)
{
     printf("  [%4d,%4d] %s\n", x, y, s);
}
static void t_focus(struct canvas *c, int x, int y, int w, int h, int r)
{
     gfx_rrect(c, x, y, w, h, r, 0xff4f7cff);
     printf("  focus at y=%d\n", y);
}
static void t_hints(void *ctx, struct canvas *c, int cx, int y, const char *const *p, int n) {}
static void t_sheet(void *ctx, struct canvas *c, int x, int y, int w, int h, const char *t) {}

static struct canvas cv;

static void draw(double now)
{
     printf("--- frame %d (%.1f s)\n", ++frame, now);
     gfx_clear(&cv);
     jfui_draw(&cv, now);
     jfui_draw(NULL, now);
}

static void run(double secs)
{
     double end = vibe_now_s() + secs;
     while (vibe_now_s() < end) {
          double now = vibe_now_s();
          if (jfui_tick(now)) draw(now);
          usleep(100000);
     }
}

int main(int argc, char **argv)
{
     static const struct { const char *n; int k; } K[] = {
          { "up", DIKS_CURSOR_UP }, { "down", DIKS_CURSOR_DOWN }, { "left", DIKS_CURSOR_LEFT },
          { "right", DIKS_CURSOR_RIGHT }, { "ok", DIKS_OK }, { "back", DIKS_BACK },
     };
     setvbuf(stdout, NULL, _IOLBF, 0);
     setenv("JFUI_TEST", "1", 1);     /* the panel refuses to sign out (revokes the UI's token) */
     cv.w = 1920; cv.h = 1080; cv.pitch = 1920;
     cv.px = calloc((size_t)cv.w * cv.h, 4);
     struct jfui_host h = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, 1920, 1080, NULL,
                            t_text, t_focus, t_hints, t_sheet };
     jfui_open(&h, vibe_now_s());
     run(0.3);
     for (int i = 1; i < argc; i++) {
          if (!strncmp(argv[i], "wait:", 5)) { run(atof(argv[i] + 5)); continue; }
          int k = 0;
          for (size_t j = 0; j < sizeof K / sizeof K[0]; j++) if (!strcmp(argv[i], K[j].n)) k = K[j].k;
          if (!k) { fprintf(stderr, "unknown step %s\n", argv[i]); return 2; }
          printf("=== key %s\n", argv[i]);
          if (!jfui_key(k, 0, vibe_now_s())) { printf("=== panel closed\n"); break; }
          run(0.3);
     }
     jfui_close(vibe_now_s());
     return 0;
}
