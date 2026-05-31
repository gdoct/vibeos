/* libgfx implementation — see libgfx.h. Pure pixel math; clipped to the surface
 * bounds. Ported from gui/core/src/gui_draw.c for userspace (malloc surfaces). */
#include "libgfx.h"
#include <stdlib.h>
#include <string.h>

extern const uint8_t font8x8[128][8];   /* font8x8_u.c */

gfx_surface_t gfx_wrap(uint32_t *px, int w, int h, int stride) {
    gfx_surface_t s = { px, w, h, stride };
    return s;
}

gfx_surface_t gfx_alloc(int w, int h) {
    gfx_surface_t s = { 0, w, h, w };
    s.px = (uint32_t *)malloc((size_t)w * h * 4);
    return s;
}

void gfx_free(gfx_surface_t *s) { if (s && s->px) { free(s->px); s->px = 0; } }

void gfx_pixel(gfx_surface_t *s, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    s->px[(size_t)y * s->stride + x] = color;
}

void gfx_hline(gfx_surface_t *s, int x, int y, int w, uint32_t color) {
    if (y < 0 || y >= s->h) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > s->w) w = s->w - x;
    uint32_t *p = s->px + (size_t)y * s->stride + x;
    for (int i = 0; i < w; i++) p[i] = color;
}

void gfx_vline(gfx_surface_t *s, int x, int y, int h, uint32_t color) {
    if (x < 0 || x >= s->w) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > s->h) h = s->h - y;
    uint32_t *p = s->px + (size_t)y * s->stride + x;
    for (int i = 0; i < h; i++) p[(size_t)i * s->stride] = color;
}

void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        uint32_t *p = s->px + (size_t)(y + row) * s->stride + x;
        for (int col = 0; col < w; col++) p[col] = color;
    }
}

void gfx_clear(gfx_surface_t *s, uint32_t color) { gfx_fill_rect(s, 0, 0, s->w, s->h, color); }

void gfx_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color) {
    gfx_hline(s, x, y, w, color);
    gfx_hline(s, x, y + h - 1, w, color);
    gfx_vline(s, x, y, h, color);
    gfx_vline(s, x + w - 1, y, h, color);
}

void gfx_blit(gfx_surface_t *dst, const uint32_t *src, int sw, int sh, int dx, int dy) {
    for (int row = 0; row < sh; row++) {
        int y = dy + row;
        if (y < 0 || y >= dst->h) continue;
        const uint32_t *srow = src + (size_t)row * sw;
        uint32_t *drow = dst->px + (size_t)y * dst->stride;
        for (int col = 0; col < sw; col++) {
            int x = dx + col;
            if (x < 0 || x >= dst->w) continue;
            drow[x] = srow[col];
        }
    }
}

void gfx_blit_key(gfx_surface_t *dst, const uint32_t *src, int sw, int sh,
                  int dx, int dy, uint32_t key) {
    for (int row = 0; row < sh; row++) {
        int y = dy + row;
        if (y < 0 || y >= dst->h) continue;
        const uint32_t *srow = src + (size_t)row * sw;
        uint32_t *drow = dst->px + (size_t)y * dst->stride;
        for (int col = 0; col < sw; col++) {
            int x = dx + col;
            if (x < 0 || x >= dst->w) continue;
            uint32_t s = srow[col];
            if (s != key) drow[x] = s;
        }
    }
}

void gfx_blit_alpha(gfx_surface_t *dst, const uint32_t *src, int sw, int sh, int dx, int dy) {
    for (int row = 0; row < sh; row++) {
        int y = dy + row;
        if (y < 0 || y >= dst->h) continue;
        const uint32_t *srow = src + (size_t)row * sw;
        uint32_t *drow = dst->px + (size_t)y * dst->stride;
        for (int col = 0; col < sw; col++) {
            int x = dx + col;
            if (x < 0 || x >= dst->w) continue;
            uint32_t s = srow[col];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            if (a == 255) { drow[x] = s & 0xFFFFFF; continue; }
            uint32_t d = drow[x];
            uint32_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
            uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t r = (sr * a + dr * (255 - a)) / 255;
            uint32_t g = (sg * a + dg * (255 - a)) / 255;
            uint32_t b = (sb * a + db * (255 - a)) / 255;
            drow[x] = (r << 16) | (g << 8) | b;
        }
    }
}

void gfx_char(gfx_surface_t *s, int x, int y, char c, uint32_t fg) {
    unsigned char idx = (unsigned char)c;
    if (idx >= 'a' && idx <= 'z') idx -= 32;
    if (idx >= 128) idx = '?';
    const uint8_t *glyph = font8x8[idx];
    for (int row = 0; row < GFX_GLYPH_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < GFX_GLYPH_W; col++)
            if (bits & (0x80 >> col)) gfx_pixel(s, x + col, y + row, fg);
    }
}

void gfx_text(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg) {
    int cx = x;
    for (; *str; str++) {
        if (*str == '\n') { y += GFX_GLYPH_H; cx = x; continue; }
        gfx_char(s, cx, y, *str, fg);
        cx += GFX_GLYPH_W;
    }
}

int gfx_text_w(const char *str) {
    int n = 0; for (; *str; str++) n++;
    return n * GFX_GLYPH_W;
}
