/* Software shapes for vibe-ui. See gfx.h. */
#include "gfx.h"

#include <math.h>
#include <string.h>

uint32_t gfx_with_alpha(uint32_t c, int a)
{
     return (c & 0x00ffffff) | (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a) << 24;
}

uint32_t gfx_mix(uint32_t c0, uint32_t c1, int t)
{
     uint32_t out = 0;
     for (int s = 0; s < 32; s += 8) {
          int a = c0 >> s & 0xff, b = c1 >> s & 0xff;
          out |= (uint32_t)((a * (255 - t) + b * t) / 255) << s;
     }
     return out;
}

/* composite src (with extra coverage cov 0..255) over *d, non-premultiplied */
static inline void over(uint32_t *d, uint32_t src, int cov)
{
     int sa = (int)(src >> 24) * cov / 255;
     if (sa <= 0)
          return;
     uint32_t dst = *d;
     int da = dst >> 24;
     if (sa >= 255 || da == 0) {
          *d = (src & 0x00ffffff) | (uint32_t)sa << 24;
          if (sa >= 255 || da == 0)
               return;
     }
     /* out_a = sa + da(1-sa); out_c = (sc sa + dc da (1-sa)) / out_a */
     int inv = 255 - sa;
     int oa = sa + da * inv / 255;
     int wd = da * inv / 255;
     uint32_t r = 0;
     for (int s = 0; s < 24; s += 8) {
          int sc = src >> s & 0xff, dc = dst >> s & 0xff;
          r |= (uint32_t)((sc * sa + dc * wd) / oa) << s;
     }
     *d = r | (uint32_t)oa << 24;
}

void gfx_clear(struct canvas *c)
{
     for (int y = 0; y < c->h; y++)
          memset(c->px + (size_t)y * c->pitch, 0, (size_t)c->w * 4);
}

void gfx_fill_vgradient_full(struct canvas *c, uint32_t top, uint32_t bottom)
{
     for (int y = 0; y < c->h; y++) {
          uint32_t col = gfx_mix(top, bottom, y * 255 / (c->h - 1));
          uint32_t *row = c->px + (size_t)y * c->pitch;
          for (int x = 0; x < c->w; x++)
               row[x] = col;
     }
}

/* coverage of pixel (px,py) for a rounded rect, 0..255 (AA at the corners) */
static inline int coverage(int px, int py, int x, int y, int w, int h, int r)
{
     float cx, cy;
     if (px < x + r)            cx = x + r - 0.5f;
     else if (px >= x + w - r)  cx = x + w - r - 0.5f;
     else                       return 255;
     if (py < y + r)            cy = y + r - 0.5f;
     else if (py >= y + h - r)  cy = y + h - r - 0.5f;
     else                       return 255;
     float dx = px - cx, dy = py - cy;
     float d = sqrtf(dx * dx + dy * dy) - (r - 0.5f);
     if (d <= -0.5f) return 255;
     if (d >= 0.5f)  return 0;
     return (int)((0.5f - d) * 255.0f);
}

static void rrect_impl(struct canvas *c, int x, int y, int w, int h, int r,
                       uint32_t col0, uint32_t col1, int hgrad)
{
     if (w <= 0 || h <= 0)
          return;
     if (r * 2 > w) r = w / 2;
     if (r * 2 > h) r = h / 2;
     int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
     int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
     for (int py = y0; py < y1; py++) {
          uint32_t *row = c->px + (size_t)py * c->pitch;
          int in_corner_band = py < y + r || py >= y + h - r;
          for (int px = x0; px < x1; px++) {
               uint32_t col = hgrad ? gfx_mix(col0, col1, (px - x) * 255 / (w > 1 ? w - 1 : 1)) : col0;
               int cov = 255;
               if (in_corner_band && (px < x + r || px >= x + w - r))
                    cov = coverage(px, py, x, y, w, h, r);
               if (cov)
                    over(&row[px], col, cov);
          }
     }
}

void gfx_rrect(struct canvas *c, int x, int y, int w, int h, int r, uint32_t colour)
{
     rrect_impl(c, x, y, w, h, r, colour, colour, 0);
}

void gfx_rrect_hgrad(struct canvas *c, int x, int y, int w, int h, int r,
                     uint32_t left, uint32_t right)
{
     rrect_impl(c, x, y, w, h, r, left, right, 1);
}

void gfx_shadow(struct canvas *c, int x, int y, int w, int h, int r, int spread, uint32_t colour)
{
     /* concentric rings, alpha falling off quadratically */
     int a0 = colour >> 24;
     for (int i = spread; i >= 1; i--) {
          float t = (float)i / (float)spread;            /* 1 outside .. 0 inside */
          int a = (int)(a0 * (1.0f - t) * (1.0f - t) / 3.0f);
          if (a <= 0)
               continue;
          gfx_rrect_outline(c, x - i, y - i, w + 2 * i, h + 2 * i, r + i, 1, gfx_with_alpha(colour, a));
     }
}

void gfx_rrect_outline(struct canvas *c, int x, int y, int w, int h, int r, int t, uint32_t colour)
{
     if (w <= 0 || h <= 0)
          return;
     if (r * 2 > w) r = w / 2;
     if (r * 2 > h) r = h / 2;
     int ri = r - t < 0 ? 0 : r - t;
     int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
     int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
     int band = r > t ? r : t;          /* only rows/columns near an edge matter */
     for (int py = y0; py < y1; py++) {
          uint32_t *row = c->px + (size_t)py * c->pitch;
          int edge_row = py < y + t || py >= y + h - t;
          int full = py < y + band || py >= y + h - band;
          for (int px = x0; px < x1; px++) {
               if (!full && px >= x + band && px < x + w - band) {
                    px = x + w - band - 1;   /* skip the hollow middle */
                    continue;
               }
               int outer = coverage(px, py, x, y, w, h, r);
               if (!outer)
                    continue;
               int inner = 0;
               if (!edge_row && px >= x + t && px < x + w - t)
                    inner = coverage(px, py, x + t, y + t, w - 2 * t, h - 2 * t, ri);
               int cov = outer - inner;
               if (cov > 0)
                    over(&row[px], colour, cov);
          }
     }
}
