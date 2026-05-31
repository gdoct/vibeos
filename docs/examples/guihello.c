/*
 * guihello — a minimal VibeOS GUI application (skeleton / starting template).
 *
 * Every VibeOS GUI app is an ordinary musl program that:
 *   1. connects to the window manager and asks for a window  (gc_open)
 *   2. draws into a pixel surface with libgfx                (gfx_*)
 *   3. ships the surface to the WM to display                (gc_commit)
 *   4. loops reading input events                            (gc_poll)
 *
 * The WM (/bin/guiwm) owns the screen, the cursor, and the window chrome
 * (title bar, border, close box); your app only draws the *content* and reacts
 * to events whose coordinates are window-local.
 *
 * Build (from the repo root):
 *   ./toolchain/x86_64-vibeos-musl-gcc -static -no-pie -O2 -Igui/client/include \
 *       -o guihello docs/examples/guihello.c \
 *       gui/client/src/libgfx.c gui/client/src/font8x8_u.c gui/client/src/gui_logo_u.c \
 *       gui/client/src/gui_client.c
 * (host musl-gcc works too). Put the binary in /bin on the VibeFS image, then
 * launch it from the serial shell — or wire it into guiwm's launcher table.
 */
#include <stdio.h>
#include <unistd.h>

#include "gui_client.h"   /* gc_open / gc_commit / gc_poll / gc_close          */
#include "libgfx.h"       /* surfaces + drawing primitives (pulled in already) */

#define W 320             /* window content size, in pixels */
#define H 200

/* Redraw the whole window into the surface. Called whenever state changes. */
static void redraw(gfx_surface_t *s, int clicks, char last_key) {
    gfx_clear(s, GFX_RGB(0x1e, 0x24, 0x30));                 /* background      */
    gfx_text(s, 12, 12, "HELLO FROM VIBEOS", GFX_RGB(0x80, 0xd0, 0xff));

    /* a "button": a filled rect with a label. Hit-tested in the event loop. */
    gfx_fill_rect(s, 12, 44, 120, 28, GFX_RGB(0x2c, 0x6e, 0x4a));
    gfx_rect(s, 12, 44, 120, 28, GFX_RGB(0xe0, 0xff, 0xe8));
    gfx_text(s, 24, 52, "CLICK ME", GFX_RGB(0xe0, 0xff, 0xe8));

    char line[48];
    snprintf(line, sizeof line, "CLICKS: %d", clicks);
    gfx_text(s, 12, 92, line, GFX_RGB(0xff, 0xff, 0xff));

    snprintf(line, sizeof line, "LAST KEY: %c", last_key ? last_key : ' ');
    gfx_text(s, 12, 112, line, GFX_RGB(0xc0, 0xc0, 0xc0));

    gfx_text(s, 12, H - 20, "PRESS Q TO QUIT", GFX_RGB(0x80, 0x90, 0xa0));
}

/* Is (x,y) inside the button rect drawn above? */
static int in_button(int x, int y) {
    return x >= 12 && x < 12 + 120 && y >= 44 && y < 44 + 28;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Connect + create a window. NULL means the WM isn't running. */
    gui_conn_t *c = gc_open(W, H, "GUIHELLO");
    if (!c) { printf("guihello: cannot reach the window manager\n"); return 1; }

    /* One backing surface we draw into and commit. */
    gfx_surface_t s = gfx_alloc(W, H);
    if (!s.px) { gc_close(c); return 1; }

    int clicks = 0;
    char last_key = 0;
    redraw(&s, clicks, last_key);
    gc_commit(c, &s);                       /* show the first frame */

    /* Event loop. gc_poll is non-blocking: 1 = got an event, 0 = none yet,
       -1 = the window was closed (the WM's close box, or the WM exited). */
    for (;;) {
        gevt_input_t ev;
        int r = gc_poll(c, &ev);
        if (r < 0) break;                   /* window closed */
        if (r > 0) {
            int dirty = 0;
            switch (ev.ev) {
            case GE_MOUSE_DOWN:             /* ev.x / ev.y are window-local */
                if (in_button(ev.x, ev.y)) { clicks++; dirty = 1; }
                break;
            case GE_KEY:
                if (ev.key == 'q') { gfx_free(&s); gc_close(c); return 0; }
                last_key = (char)ev.key; dirty = 1;
                break;
            default:                        /* GE_MOUSE_MOVE/UP/FOCUS/... ignored */
                break;
            }
            if (dirty) { redraw(&s, clicks, last_key); gc_commit(c, &s); }
        }
        usleep(15000);                      /* ~60 Hz; don't spin the CPU */
    }

    gfx_free(&s);
    gc_close(c);
    return 0;
}
