/* libgfx implementation — see libgfx.h. Pure pixel math; clipped to the surface
 * bounds. Ported from gui/core/src/gui_draw.c for userspace (malloc surfaces). */
#include "libgfx.h"
#include <stdlib.h>
#include <string.h>

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

/* ---- text: antialiased glyph atlases (see gfx_font.h) ---- */

static const gfx_font_t *g_font = &gfx_font_chicago;
static gfx_font_size_t   g_size = GFX_FONT_NORMAL;

static const gfx_face_t *face(void) { return &g_font->faces[g_size]; }

void gfx_set_font(const gfx_font_t *f) { if (f) g_font = f; }
void gfx_set_size(gfx_font_size_t sz)  { if (sz < GFX_FONT_NSIZES) g_size = sz; }

int gfx_line_h(void)      { return face()->line_h; }
int gfx_font_ascent(void) { return face()->ascent; }
int gfx_cell_w(void)      { return face()->cell_w; }

/* Advance width of one ASCII byte in the active face (cell_w for misses). */
static int glyph_advance(const gfx_face_t *f, unsigned char uc) {
    if (uc < f->first || uc > f->last) return f->cell_w;
    return f->glyphs[uc - f->first].advance;
}

void gfx_char(gfx_surface_t *s, int x, int y, char c, uint32_t fg) {
    const gfx_face_t *f = face();
    unsigned char uc = (unsigned char)c;
    if (uc < f->first || uc > f->last) return;        /* no glyph (incl. space) */
    const gfx_glyph_t *g = &f->glyphs[uc - f->first];
    const uint8_t *a = f->alpha + g->off;
    int bx = x + g->xoff;
    int by = y + f->ascent + g->yoff;                 /* atlas is baseline-relative */
    uint32_t fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fb = fg & 0xFF;

    for (int row = 0; row < g->h; row++) {
        int py = by + row;
        if (py < 0 || py >= s->h) continue;
        const uint8_t *arow = a + (size_t)row * g->w;
        uint32_t *drow = s->px + (size_t)py * s->stride;
        for (int col = 0; col < g->w; col++) {
            uint32_t cov = arow[col];
            if (!cov) continue;
            int px = bx + col;
            if (px < 0 || px >= s->w) continue;
            if (cov == 255) { drow[px] = fg & 0xFFFFFF; continue; }
            uint32_t d = drow[px];
            uint32_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
            uint32_t r = (fr * cov + dr * (255 - cov)) / 255;
            uint32_t gg = (fgc * cov + dg * (255 - cov)) / 255;
            uint32_t b = (fb * cov + db * (255 - cov)) / 255;
            drow[px] = (r << 16) | (gg << 8) | b;
        }
    }
}

void gfx_text(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg) {
    const gfx_face_t *f = face();
    int cx = x;
    for (; *str; str++) {
        if (*str == '\n') { y += f->line_h; cx = x; continue; }
        gfx_char(s, cx, y, *str, fg);
        cx += glyph_advance(f, (unsigned char)*str);
    }
}

int gfx_text_w(const char *str) {
    const gfx_face_t *f = face();
    int w = 0, line = 0;
    for (; *str; str++) {
        if (*str == '\n') { if (line > w) w = line; line = 0; continue; }
        line += glyph_advance(f, (unsigned char)*str);
    }
    return line > w ? line : w;
}
