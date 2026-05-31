#include "kernel.h"
#include "gui_draw.h"

/* libdraw — see gui_draw.h. Pure rendering into surfaces; clipped per-primitive. */

extern "C" const uint8_t font8x8[128][8];

static inline uint32_t *px(surface_t *s, int x, int y) { return &s->pixels[(uint32_t)y * s->stride + x]; }

void draw_pixel(surface_t *s, int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    *px(s, x, y) = color;
}

void draw_hline(surface_t *s, int x, int y, int w, uint32_t color) {
    if (y < 0 || y >= s->h) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > s->w) w = s->w - x;
    uint32_t *p = px(s, x, y);
    for (int i = 0; i < w; i++) p[i] = color;
}

void draw_vline(surface_t *s, int x, int y, int h, uint32_t color) {
    if (x < 0 || x >= s->w) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > s->h) h = s->h - y;
    for (int i = 0; i < h; i++) *px(s, x, y + i) = color;
}

void draw_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *p = px(s, x, y + j);
        for (int i = 0; i < w; i++) p[i] = color;
    }
}

void draw_clear(surface_t *s, uint32_t color) { draw_fill_rect(s, 0, 0, s->w, s->h, color); }

void draw_rect(surface_t *s, int x, int y, int w, int h, uint32_t color) {
    draw_hline(s, x, y, w, color);
    draw_hline(s, x, y + h - 1, w, color);
    draw_vline(s, x, y, h, color);
    draw_vline(s, x + w - 1, y, h, color);
}

void draw_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2;
    for (;;) {
        draw_pixel(s, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}

void draw_blit(surface_t *dst, const surface_t *src, int dx, int dy) {
    for (int j = 0; j < src->h; j++) {
        int y = dy + j; if (y < 0 || y >= dst->h) continue;
        for (int i = 0; i < src->w; i++) {
            int x = dx + i; if (x < 0 || x >= dst->w) continue;
            dst->pixels[(uint32_t)y * dst->stride + x] = src->pixels[(uint32_t)j * src->stride + i];
        }
    }
}

void draw_blit_key(surface_t *dst, const surface_t *src, int dx, int dy, uint32_t key) {
    for (int j = 0; j < src->h; j++) {
        int y = dy + j; if (y < 0 || y >= dst->h) continue;
        for (int i = 0; i < src->w; i++) {
            int x = dx + i; if (x < 0 || x >= dst->w) continue;
            uint32_t c = src->pixels[(uint32_t)j * src->stride + i];
            if (c == key) continue;
            dst->pixels[(uint32_t)y * dst->stride + x] = c;
        }
    }
}

void draw_blit_alpha(surface_t *dst, const uint32_t *src, int sw, int sh, int dx, int dy) {
    for (int j = 0; j < sh; j++) {
        int y = dy + j; if (y < 0 || y >= dst->h) continue;
        for (int i = 0; i < sw; i++) {
            int x = dx + i; if (x < 0 || x >= dst->w) continue;
            uint32_t s = src[(uint32_t)j * sw + i];
            uint32_t a = s >> 24;
            if (a == 0) continue;
            uint32_t *d = &dst->pixels[(uint32_t)y * dst->stride + x];
            if (a == 255) { *d = s & 0xFFFFFF; continue; }
            uint32_t dv = *d, ia = 255 - a;
            uint32_t r = (((s >> 16) & 0xFF) * a + ((dv >> 16) & 0xFF) * ia) / 255;
            uint32_t g = (((s >> 8)  & 0xFF) * a + ((dv >> 8)  & 0xFF) * ia) / 255;
            uint32_t b = ((s & 0xFF) * a + (dv & 0xFF) * ia) / 255;
            *d = (r << 16) | (g << 8) | b;
        }
    }
}

void draw_copy_rect(surface_t *dst, const surface_t *src,
                    int sx, int sy, int w, int h, int dx, int dy) {
    for (int j = 0; j < h; j++) {
        int syy = sy + j, dyy = dy + j;
        if (syy < 0 || syy >= src->h || dyy < 0 || dyy >= dst->h) continue;
        for (int i = 0; i < w; i++) {
            int sxx = sx + i, dxx = dx + i;
            if (sxx < 0 || sxx >= src->w || dxx < 0 || dxx >= dst->w) continue;
            dst->pixels[(uint32_t)dyy * dst->stride + dxx] = src->pixels[(uint32_t)syy * src->stride + sxx];
        }
    }
}

void draw_char(surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg, int transparent) {
    unsigned char idx = (unsigned char)c;
    if (idx >= 'a' && idx <= 'z') idx -= 32;     /* font carries caps only */
    if (idx >= 128) idx = '?';
    const uint8_t *g = font8x8[idx];
    for (int row = 0; row < GLYPH_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < GLYPH_W; col++) {
            if (bits & (0x80 >> col)) draw_pixel(s, x + col, y + row, fg);
            else if (!transparent)    draw_pixel(s, x + col, y + row, bg);
        }
    }
}

void draw_text(surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int transparent) {
    int cx = x;
    for (; *str; str++) {
        if (*str == '\n') { y += GLYPH_H; cx = x; continue; }
        draw_char(s, cx, y, *str, fg, bg, transparent);
        cx += GLYPH_W;
    }
}
