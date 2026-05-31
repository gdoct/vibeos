/* libgfx — the userspace drawing primitives shared by the GUI server and its
 * clients (gui/client). A direct port of gui/core's libdraw: a surface is a
 * width x height pixel buffer (0x00RRGGBB), every primitive is clipped to the
 * surface, and nothing here knows about windows, input, or the framebuffer —
 * the server wraps the mmap'd /dev/fb0 in a surface, a client wraps a malloc'd
 * window buffer in one, and both draw the same way. */
#ifndef VIBEOS_LIBGFX_H
#define VIBEOS_LIBGFX_H

#include <stdint.h>

typedef struct gfx_surface {
    uint32_t *px;
    int w, h, stride;       /* stride in pixels */
} gfx_surface_t;

#define GFX_RGB(r,g,b) (((uint32_t)(r)<<16)|((uint32_t)(g)<<8)|(uint32_t)(b))
#define GFX_GLYPH_W 8
#define GFX_GLYPH_H 8

/* Wrap an existing pixel buffer (no allocation). */
gfx_surface_t gfx_wrap(uint32_t *px, int w, int h, int stride);

/* Allocate a w*h surface (malloc); px==NULL on failure. gfx_free releases it. */
gfx_surface_t gfx_alloc(int w, int h);
void          gfx_free(gfx_surface_t *s);

void gfx_clear(gfx_surface_t *s, uint32_t color);
void gfx_fill_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_rect(gfx_surface_t *s, int x, int y, int w, int h, uint32_t color);
void gfx_hline(gfx_surface_t *s, int x, int y, int w, uint32_t color);
void gfx_vline(gfx_surface_t *s, int x, int y, int h, uint32_t color);
void gfx_pixel(gfx_surface_t *s, int x, int y, uint32_t color);

/* Blit a raw pixel block (src is sw*sh, tightly packed) at (dx,dy). */
void gfx_blit(gfx_surface_t *dst, const uint32_t *src, int sw, int sh, int dx, int dy);
/* Blit with a color key (key pixels are skipped — transparency). */
void gfx_blit_key(gfx_surface_t *dst, const uint32_t *src, int sw, int sh,
                  int dx, int dy, uint32_t key);
/* Alpha-blit a 0xAARRGGBB block over dst (per-pixel alpha). */
void gfx_blit_alpha(gfx_surface_t *dst, const uint32_t *src, int sw, int sh, int dx, int dy);

void gfx_char(gfx_surface_t *s, int x, int y, char c, uint32_t fg);
void gfx_text(gfx_surface_t *s, int x, int y, const char *str, uint32_t fg);
int  gfx_text_w(const char *str);   /* pixel width of a string */

#endif
