#ifndef VIBE_GFX_H
#define VIBE_GFX_H
/*
 * Small software renderer for vibe-ui's shapes: anti-aliased rounded
 * rectangles, gradients, soft shadows. It draws into a locked ARGB surface
 * (non-premultiplied, the format the window uses), compositing "over" the
 * existing pixels. Text is left to DirectFB.
 */
#include <stdint.h>

struct canvas {
     uint32_t *px;
     int pitch;          /* in pixels */
     int w, h;
};

/* colour helpers: 0xAARRGGBB */
static inline uint32_t argb(int a, int r, int g, int b)
{
     return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}
uint32_t gfx_with_alpha(uint32_t c, int a);
uint32_t gfx_mix(uint32_t c0, uint32_t c1, int t /* 0..255 */);

void gfx_clear(struct canvas *c);

/* vertical gradient over the whole canvas, written (not blended) */
void gfx_fill_vgradient_full(struct canvas *c, uint32_t top, uint32_t bottom);

/* rounded rectangle, anti-aliased, blended over; r = corner radius */
void gfx_rrect(struct canvas *c, int x, int y, int w, int h, int r, uint32_t colour);

/* rounded rectangle with a horizontal gradient */
void gfx_rrect_hgrad(struct canvas *c, int x, int y, int w, int h, int r,
                     uint32_t left, uint32_t right);

/* soft shadow / glow around a rounded rectangle: `spread` px, colour's alpha
 * is the strength at the edge, fading out */
void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int r, int spread, uint32_t colour);

/* rounded-rectangle outline of width t */
void gfx_rrect_outline(struct canvas *c, int x, int y, int w, int h, int r, int t, uint32_t colour);

#endif
