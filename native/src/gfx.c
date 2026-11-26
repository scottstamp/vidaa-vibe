/* Software shapes for vibe-ui. See gfx.h. */
#include "gfx.h"

#include <math.h>
#include <string.h>

uint32_t gfx_with_alpha(uint32_t c, int a)
{
     return (c & 0x00ffffff) | (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a) << 24;
}

/* x / 255 for 0 <= x <= 65535, exact, without a division */
static inline int div255(int x)
{
     return (x + 1 + (x >> 8)) >> 8;
}

uint32_t gfx_mix(uint32_t c0, uint32_t c1, int t)
{
     uint32_t out = 0;
     for (int s = 0; s < 32; s += 8) {
          int a = c0 >> s & 0xff, b = c1 >> s & 0xff;
          out |= (uint32_t)div255(a * (255 - t) + b * t) << s;
     }
     return out;
}

/* composite src (with extra coverage cov 0..255) over *d, non-premultiplied.
 * Hot: one division per blended pixel (was five; the keyboard's frame went
 * from ~370 ms of shapes). */
static inline void over(uint32_t *d, uint32_t src, int cov)
{
     int sa = div255((int)(src >> 24) * cov);
     if (sa <= 0)
          return;
     uint32_t dst = *d;
     int da = dst >> 24;
     if (sa >= 255 || da == 0) {
          *d = (src & 0x00ffffff) | (uint32_t)sa << 24;
          return;
     }
     /* out_a = sa + da(1-sa); out_c = (sc sa + dc da (1-sa)) / out_a */
     int wd = div255(da * (255 - sa));
     int oa = sa + wd;
     /* sc*sa + dc*wd <= 255*oa, so the product stays below 2^24 */
     uint32_t k = (1u << 16) / (uint32_t)oa;
     uint32_t r = 0;
     for (int s = 0; s < 24; s += 8) {
          uint32_t sc = src >> s & 0xff, dc = dst >> s & 0xff;
          uint32_t v = ((sc * sa + dc * wd) * k + 0x8000) >> 16;
          r |= (v > 255 ? 255 : v) << s;
     }
     *d = r | (uint32_t)oa << 24;
}

/* the pixels a primitive may touch: the box [x, x+w) x [y, y+h) cut to the
 * canvas and its clip. Returns 0 if nothing is left. */
static int bounds(const struct canvas *c, int x, int y, int w, int h,
                  int *x0, int *y0, int *x1, int *y1)
{
     int bx0 = 0, by0 = 0, bx1 = c->w, by1 = c->h;
     if (c->clipped) {
          if (c->clip_x0 > bx0) bx0 = c->clip_x0;
          if (c->clip_y0 > by0) by0 = c->clip_y0;
          if (c->clip_x1 < bx1) bx1 = c->clip_x1;
          if (c->clip_y1 < by1) by1 = c->clip_y1;
     }
     *x0 = x > bx0 ? x : bx0;
     *y0 = y > by0 ? y : by0;
     *x1 = x + w < bx1 ? x + w : bx1;
     *y1 = y + h < by1 ? y + h : by1;
     return *x0 < *x1 && *y0 < *y1;
}

void gfx_clear(struct canvas *c)
{
     int x0, y0, x1, y1;
     if (!bounds(c, 0, 0, c->w, c->h, &x0, &y0, &x1, &y1))
          return;
     for (int y = y0; y < y1; y++)
          memset(c->px + (size_t)y * c->pitch + x0, 0, (size_t)(x1 - x0) * 4);
}

void gfx_fill_vgradient_full(struct canvas *c, uint32_t top, uint32_t bottom)
{
     int x0, y0, x1, y1;
     if (!bounds(c, 0, 0, c->w, c->h, &x0, &y0, &x1, &y1))
          return;
     for (int y = y0; y < y1; y++) {
          uint32_t col = gfx_mix(top, bottom, y * 255 / (c->h - 1));
          uint32_t *row = c->px + (size_t)y * c->pitch;
          for (int x = x0; x < x1; x++)
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
     int x0, y0, x1, y1;
     if (!bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
          return;
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

void gfx_image(struct canvas *c, int x, int y, int w, int h, int r,
               const uint32_t *src, int src_pitch, int alpha)
{
     if (w <= 0 || h <= 0 || !src || alpha <= 0)
          return;
     if (r * 2 > w) r = w / 2;
     if (r * 2 > h) r = h / 2;
     int x0, y0, x1, y1;
     if (!bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
          return;
     for (int py = y0; py < y1; py++) {
          uint32_t *row = c->px + (size_t)py * c->pitch;
          const uint32_t *s = src + (size_t)(py - y) * src_pitch - x;
          int in_corner_band = py < y + r || py >= y + h - r;
          for (int px = x0; px < x1; px++) {
               int cov = alpha;
               if (in_corner_band && (px < x + r || px >= x + w - r))
                    cov = coverage(px, py, x, y, w, h, r) * alpha / 255;
               if (cov >= 255 && (s[px] >> 24) == 0xff) row[px] = s[px];
               else if (cov) over(&row[px], s[px], cov);
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
     /* Alpha falls off quadratically with the distance d from the rounded
      * rect, out to spread: a0 (1 - d/spread)^2 / 3, the curve the old
      * concentric-ring version drew one ring per pixel of spread. One pass
      * over the shadow band, the rect's inside skipped. */
     if (w <= 0 || h <= 0 || spread <= 0)
          return;
     if (r * 2 > w) r = w / 2;
     if (r * 2 > h) r = h / 2;
     int a0 = colour >> 24;
     int X0, Y0, X1, Y1;
     if (!bounds(c, x - spread, y - spread, w + 2 * spread, h + 2 * spread, &X0, &Y0, &X1, &Y1))
          return;
     float lx = x + r, rx = x + w - r, ty = y + r, by = y + h - r;
     /* alpha by whole-pixel distance, computed once */
     int lut[256];
     int n = spread < 255 ? spread : 255;
     for (int i = 0; i <= n; i++) {
          float t = (float)i / (float)spread;
          lut[i] = (int)(a0 * (1.0f - t) * (1.0f - t) / 3.0f);
     }
     uint32_t rgb = colour & 0x00ffffff;
     for (int py = Y0; py < Y1; py++) {
          uint32_t *row = c->px + (size_t)py * c->pitch;
          float fy = py + 0.5f;
          float dy = fy < ty ? ty - fy : fy > by ? fy - by : 0.0f;
          /* inside the rect on this row: [skip0, skip1) */
          int skip0 = -1, skip1 = -1;       /* -1: nothing to skip */
          if (py >= y && py < y + h) {
               if (py >= y + r && py < y + h - r) { skip0 = x; skip1 = x + w; }
               else { skip0 = x + r; skip1 = x + w - r; }
               if (skip1 <= skip0) skip0 = skip1 = -1;
          }
          for (int px = X0; px < X1; px++) {
               if (px == skip0) { px = skip1 - 1; continue; }   /* skip1 > skip0 */
               float fx = px + 0.5f;
               float dx = fx < lx ? lx - fx : fx > rx ? fx - rx : 0.0f;
               float d = (dx == 0.0f) ? dy - r : (dy == 0.0f) ? dx - r : sqrtf(dx * dx + dy * dy) - r;
               if (d < 0.5f)
                    continue;               /* inside the rounded rect */
               int i = (int)(d + 0.5f);
               if (i > n)
                    continue;
               if (lut[i] > 0)
                    over(&row[px], rgb | (uint32_t)lut[i] << 24, 255);
          }
     }
}

void gfx_rrect_outline(struct canvas *c, int x, int y, int w, int h, int r, int t, uint32_t colour)
{
     if (w <= 0 || h <= 0)
          return;
     if (r * 2 > w) r = w / 2;
     if (r * 2 > h) r = h / 2;
     int ri = r - t < 0 ? 0 : r - t;
     int x0, y0, x1, y1;
     if (!bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
          return;
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
