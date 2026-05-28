#ifndef MYOS_FB_H
#define MYOS_FB_H

#include <stdint.h>
#include "device.h"
#include "../../boot/include/bootinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fb_device {
    device_t  dev;

    /* Linear framebuffer geometry, copied from BootInfo. */
    uint8_t  *base;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;        /* bytes per scanline */
    uint32_t  format;       /* FB_FORMAT_BGR8 / RGB8 */

    /* Ops. All coordinates are in pixels; colors are packed via fb_rgb. */
    void (*put_pixel)(struct fb_device *, uint32_t x, uint32_t y, uint32_t color);
    void (*fill_rect)(struct fb_device *, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
    void (*draw_char)(struct fb_device *, uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
    void (*draw_text)(struct fb_device *, uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);
} fb_device_t;

/* Initialise the primary framebuffer from BootInfo and register it. */
fb_device_t *fb_init(const FramebufferInfo *info);

/* Pack an RGB triple into the format of the given device. */
uint32_t fb_rgb(const fb_device_t *fb, uint8_t r, uint8_t g, uint8_t b);

/* Font geometry exposed so callers can lay out text. */
#define FB_FONT_W 8
#define FB_FONT_H 8

#ifdef __cplusplus
}
#endif

#endif
