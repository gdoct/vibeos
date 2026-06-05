/* gmandel — a VibeOS GUI client: renders the Mandelbrot set into its
 * own window. Connects to /bin/guiwm over loopback TCP, creates a window, draws
 * the fractal into a libgfx surface, and commits it. Arrow-ish keys pan/zoom and
 * re-render; the WM's close box (or 'q') ends it. Proves process-per-window:
 * this is an ordinary musl program, separate from the compositor. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "gui_client.h"

#define W 300
#define H 220

static double cx = -0.6, cy = 0.0, scale = 3.2;

static uint32_t color(int it, int maxit) {
    if (it >= maxit) return GFX_RGB(0,0,0);
    /* smooth-ish palette */
    int t = it * 255 / maxit;
    int r = (t * 5) & 0xff, g = (t * 7 + 40) & 0xff, b = (t * 11 + 90) & 0xff;
    return GFX_RGB(r, g, b);
}

static void render(gfx_surface_t *s) {
    const int maxit = 90;
    int w = s->w, h = s->h;
    for (int py = 0; py < h; py++) {
        double y0 = cy + (py - h/2) * (scale / w);
        for (int px = 0; px < w; px++) {
            double x0 = cx + (px - w/2) * (scale / w);
            double x = 0, y = 0; int it = 0;
            while (x*x + y*y <= 4.0 && it < maxit) {
                double xt = x*x - y*y + x0;
                y = 2*x*y + y0; x = xt; it++;
            }
            s->px[(size_t)py * s->stride + px] = color(it, maxit);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    gui_conn_t *c = gc_open(W, H, "MANDELBROT");
    if (!c) { printf("gmandel: cannot reach the window manager\n"); return 1; }
    printf("gmandel: window %u open; rendering\n", c->wid);

    gfx_surface_t s = gfx_alloc(W, H);
    if (!s.px) { printf("gmandel: surface alloc failed\n"); return 1; }
    /* show something immediately, then render the (slow) fractal */
    gfx_clear(&s, GFX_RGB(0x18,0x10,0x28));
    gfx_text(&s, 10, 10, "RENDERING MANDELBROT...", GFX_RGB(0xc0,0xc0,0xff));
    gc_commit(c, &s);
    printf("gmandel: placeholder committed\n");
    render(&s);
    gc_commit(c, &s);
    printf("gmandel: committed fractal frame\n");

    for (;;) {
        gevt_input_t ev;
        int r, resized = 0, rerender = 0;
        /* Drain ALL pending events before acting. A resize drag delivers a burst
           of GE_RESIZE (one per WM frame); rendering the fractal for each is far
           too slow and the client would lag the whole drag behind. Coalesce them:
           gc_poll keeps c->w/c->h at the latest size, so we render just once. */
        while ((r = gc_poll(c, &ev)) > 0) {
            if (ev.ev == GE_RESIZE) resized = 1;     /* c->w/c->h already updated */
            else if (ev.ev == GE_KEY && (ev.buttons & GIN_PRESSED)) {  /* key-down only */
                switch (ev.key) {
                case 'q': gc_close(c); gfx_free(&s); return 0;
                case '=': case '+': scale *= 0.7; rerender = 1; break;
                case '-': scale /= 0.7;            rerender = 1; break;
                case 'w': cy -= scale*0.1;         rerender = 1; break;
                case 's': cy += scale*0.1;         rerender = 1; break;
                case 'a': cx -= scale*0.1;         rerender = 1; break;
                case 'd': cx += scale*0.1;         rerender = 1; break;
                default: break;
                }
            }
        }
        if (r < 0) { printf("gmandel: window closed\n"); break; }
        if (resized) {                               /* re-fit surface to final size */
            gfx_free(&s); s = gfx_alloc(c->w, c->h);
            if (!s.px) { printf("gmandel: surface alloc failed\n"); break; }
            rerender = 1;
        }
        if (rerender) { render(&s); gc_commit(c, &s); }
        usleep(20000);
    }
    gfx_free(&s);
    gc_close(c);
    return 0;
}
