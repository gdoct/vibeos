#include "kernel.h"
#include "pmm.h"
#include "fb.h"
#include "device.h"

extern "C" const uint8_t font8x8[128][8];

/* Storage for the primary framebuffer. One per system today. */
static fb_device_t g_fb;

uint32_t fb_rgb(const fb_device_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->format == FB_FORMAT_RGB8) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    /* BGR8: 0x00RRGGBB packed dword, byte 0 = B at low address. Same
       integer layout when written as a 32-bit LE store. */
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t *fb_pixel_addr(fb_device_t *fb, uint32_t x, uint32_t y) {
    return (uint32_t *)(fb->base + (uint64_t)y * fb->pitch + (uint64_t)x * 4);
}

static void fb_put_pixel(fb_device_t *fb, uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb->width || y >= fb->height) return;
    *fb_pixel_addr(fb, x, y) = color;
}

static void fb_fill_rect(fb_device_t *fb, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t color) {
    if (x >= fb->width || y >= fb->height) return;
    if (x + w > fb->width)  w = fb->width  - x;
    if (y + h > fb->height) h = fb->height - y;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t *p = fb_pixel_addr(fb, x, y + row);
        for (uint32_t col = 0; col < w; col++) p[col] = color;
    }
}

static void fb_draw_char(fb_device_t *fb, uint32_t x, uint32_t y,
                         char c, uint32_t fg, uint32_t bg) {
    /* Fold lowercase → uppercase; this font only carries caps. */
    unsigned char idx = (unsigned char)c;
    if (idx >= 'a' && idx <= 'z') idx -= 32;
    if (idx >= 128) idx = '?';

    const uint8_t *glyph = font8x8[idx];
    for (uint32_t row = 0; row < FB_FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FB_FONT_W; col++) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            if (x + col < fb->width && y + row < fb->height)
                *fb_pixel_addr(fb, x + col, y + row) = color;
        }
    }
}

static void fb_draw_text(fb_device_t *fb, uint32_t x, uint32_t y,
                         const char *s, uint32_t fg, uint32_t bg) {
    uint32_t cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += FB_FONT_H; cx = x; continue; }
        fb_draw_char(fb, cx, y, *s, fg, bg);
        cx += FB_FONT_W;
    }
}

fb_device_t *fb_init(const FramebufferInfo *info) {
    g_fb.dev.name = "fb0";
    g_fb.dev.cls  = DEV_FRAMEBUFFER;
    g_fb.base     = (uint8_t *)(uintptr_t)info->base;
    g_fb.width    = info->width;
    g_fb.height   = info->height;
    g_fb.pitch    = info->pitch;
    g_fb.format   = info->format;

    g_fb.put_pixel = fb_put_pixel;
    g_fb.fill_rect = fb_fill_rect;
    g_fb.draw_char = fb_draw_char;
    g_fb.draw_text = fb_draw_text;

    device_register(&g_fb.dev);
    return &g_fb;
}
