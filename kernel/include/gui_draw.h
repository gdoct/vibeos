#ifndef VIBEOS_GUI_DRAW_H
#define VIBEOS_GUI_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libdraw — layer 1 of the GUI (graphics/graphics.md). 2D primitives over a
 * surface: a w x h pixel buffer (0x00RRGGBB). The screen is just the surface
 * that wraps the framebuffer, so off-screen rendering / backing stores are free.
 * Every primitive is clipped to the surface bounds. No notion of windows here.
 */

typedef struct {
    uint32_t *pixels;      /* 0x00RRGGBB, row-major */
    int       w, h;        /* dimensions in pixels */
    int       stride;      /* pixels per row (>= w) */
} surface_t;

typedef struct { int x, y, w, h; } rect_t;

#define RGB(r, g, b)  (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

/* Glyph cell size (8x8 bitmap font). */
#define GLYPH_W 8
#define GLYPH_H 8

void draw_clear(surface_t *s, uint32_t color);
void draw_pixel(surface_t *s, int x, int y, uint32_t color);
void draw_hline(surface_t *s, int x, int y, int w, uint32_t color);
void draw_vline(surface_t *s, int x, int y, int h, uint32_t color);
void draw_fill_rect(surface_t *s, int x, int y, int w, int h, uint32_t color);
void draw_rect(surface_t *s, int x, int y, int w, int h, uint32_t color);   /* outline */
void draw_line(surface_t *s, int x0, int y0, int x1, int y1, uint32_t color);

/* Blit `src` onto `dst` at (dx,dy). _key skips pixels equal to `key` (1-bit
   transparency — used for the cursor sprite). */
void draw_blit(surface_t *dst, const surface_t *src, int dx, int dy);
void draw_blit_key(surface_t *dst, const surface_t *src, int dx, int dy, uint32_t key);
/* Alpha-blend `src` (0xAARRGGBB pixels) over `dst`, at (dx,dy), w x h pixels. */
void draw_blit_alpha(surface_t *dst, const uint32_t *src, int sw, int sh, int dx, int dy);
/* Copy a sub-rect of dst into another dst region (for cursor save/restore). */
void draw_copy_rect(surface_t *dst, const surface_t *src,
                    int sx, int sy, int w, int h, int dx, int dy);

void draw_char(surface_t *s, int x, int y, char c, uint32_t fg, uint32_t bg, int transparent);
void draw_text(surface_t *s, int x, int y, const char *str, uint32_t fg, uint32_t bg, int transparent);

#ifdef __cplusplus
}
#endif

#endif
