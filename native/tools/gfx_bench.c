#define _GNU_SOURCE
/* Times vibe-ui's software primitives on the TV's CPU (no DirectFB).
 * Build: see native/build.sh (gfx_bench). Run: /var/local/vibe/gfx_bench */
#include "../src/gfx.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ms(void)
{
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define T(label, code)                                                       \
     do {                                                                    \
          double t0 = now_ms();                                              \
          code;                                                              \
          printf("%-34s %7.1f ms\n", label, now_ms() - t0);                  \
     } while (0)

int main(void)
{
     struct canvas c = { calloc(1920 * 1080, 4), 1920, 1920, 1080 };
     if (!c.px) return 1;
     int x0 = 1920 - 40 - 760, y0 = 40, w = 760, h = 1000;
     T("clear", gfx_clear(&c));
     T("sheet shadow (spread 28)", gfx_shadow(&c, x0, y0, w, h, 36, 28, 0x90000000));
     T("sheet fill (on transparent)", gfx_rrect(&c, x0, y0, w, h, 36, 0xf50f121a));
     T("sheet outline", gfx_rrect_outline(&c, x0, y0, w, h, 36, 1, 0x26ffffff));
     T("50 keys 61x84 (over panel)",
       for (int i = 0; i < 50; i++)
            gfx_rrect(&c, x0 + 28 + (i % 10) * 71, 338 + (i / 10) * 94, 61, 84, 16, 0xeb1c2130));
     T("focus shadow (spread 22)", gfx_shadow(&c, x0 + 28, 338, 61, 84, 16, 22, 0xa06470ff));
     T("focus gradient fill", gfx_rrect_hgrad(&c, x0 + 28, 338, 61, 84, 16, 0xff4f7cff, 0xff8a5cff));
     T("field fill + outline",
       gfx_rrect(&c, x0 + 28, 190, 704, 100, 24, 0xeb1c2130);
       gfx_rrect_outline(&c, x0 + 28, 190, 704, 100, 24, 2, 0xff4f7cff));
     return 0;
}
