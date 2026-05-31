/* gclock — a second VibeOS GUI client (phase 2): a live window. Shows a seconds
 * counter and a box bouncing inside the window, re-committing a few times a
 * second. Running it alongside gmandel demonstrates the WM compositing several
 * independent client processes, each in its own window. */
#include <stdio.h>
#include <unistd.h>
#include "gui_client.h"

#define W 240
#define H 140

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    gui_conn_t *c = gc_open(W, H, "CLOCK");
    if (!c) { printf("gclock: cannot reach the window manager\n"); return 1; }
    printf("gclock: window %u open\n", c->wid);

    gfx_surface_t s = gfx_alloc(W, H);
    if (!s.px) return 1;

    int bx = 10, by = 30, vx = 3, vy = 2, ticks = 0;
    for (;;) {
        gevt_input_t ev;
        int r = gc_poll(c, &ev);
        if (r < 0) break;
        if (r > 0 && ev.ev == GE_KEY && ev.key == 'q') break;

        gfx_clear(&s, GFX_RGB(0x12,0x16,0x22));
        char buf[32];
        int secs = ticks / 5;
        snprintf(buf, sizeof buf, "UPTIME %02d:%02d", secs/60, secs%60);
        gfx_text(&s, 10, 10, buf, GFX_RGB(0x80,0xf0,0xb0));
        gfx_rect(&s, bx, by, 24, 24, GFX_RGB(0xf0,0xd0,0x40));
        gfx_fill_rect(&s, bx+4, by+4, 16, 16, GFX_RGB(0xc0,0x60,0x30));
        bx += vx; by += vy;
        if (bx < 2 || bx > W-26) vx = -vx;
        if (by < 26 || by > H-26) vy = -vy;
        gc_commit(c, &s);
        ticks++;
        usleep(200000);   /* ~5 fps */
    }
    gfx_free(&s);
    gc_close(c);
    return 0;
}
