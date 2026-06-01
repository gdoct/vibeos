/* guiwm — the VibeOS userspace window manager (GUI phase 2).
 *
 * The one process that owns the screen. It mmaps /dev/fb0, grabs /dev/input,
 * and listens on loopback TCP (127.0.0.1:GUI_PORT) for clients. Each client
 * connection is one window: the client ships pixel frames, the server draws the
 * window chrome around them, composites the desktop (wallpaper + logo + windows
 * in Z-order + a taskbar), and tracks the mouse pointer. Input is routed to the
 * window under the cursor (window-local coordinates), title bars drag, and the
 * focused window gets the keyboard.
 *
 * Compositing strategy mirrors the old in-kernel WM: a back buffer holds the
 * scene WITHOUT the cursor; the cursor is drawn straight onto the framebuffer
 * after presenting, so moving it only repaints the small rects it leaves/enters
 * (no full recompose), while real scene changes set a dirty flag and recompose. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libgfx.h"
#include "gui_proto.h"

extern char **environ;

extern const uint32_t vibeos_logo[];
extern const int vibeos_logo_w, vibeos_logo_h;

#define MAXWIN     16
#define TITLEBAR_H 22
#define BORDER     2
#define TASKBAR_H  24
#define GRIP       16           /* resize handle in the bottom-right corner */
#define MIN_W      120          /* minimum window content size */
#define MIN_H      80

#define C_DESKTOP   GFX_RGB(0x20,0x60,0x80)
#define C_TITLE     GFX_RGB(0x2a,0x52,0x86)
#define C_TITLE_FG  GFX_RGB(0xff,0xff,0xff)
#define C_TITLE_BLUR GFX_RGB(0x50,0x58,0x60)
#define C_BORDER    GFX_RGB(0x10,0x18,0x20)
#define C_TASKBAR   GFX_RGB(0x18,0x2a,0x3a)
#define C_TASK_FG   GFX_RGB(0xc0,0xd0,0xe0)
#define C_CLOSE     GFX_RGB(0xc0,0x40,0x30)

typedef struct {
    int used, fd;
    uint32_t wid;
    int x, y, w, h;             /* content origin + size */
    uint32_t *pix;              /* w*h content pixels */
    char title[48];
    /* incremental non-blocking reader: rbuf holds one in-flight message
       (header + body); rneed is the current target length, rgot the bytes so
       far. We don't trust poll() to report per-socket readability, so each
       window is drained with non-blocking reads every frame. */
    uint8_t *rbuf;
    int rgot, rneed, rcap;
    /* one pending outgoing event message — input is sent atomically and dropped
       (not queued unbounded) when the client falls behind, so the compositor
       never blocks on a slow client. */
    uint8_t obuf[64];
    int olen, ooff;
    int rz_pending;             /* owe the client a GE_RESIZE for the current w/h */
} win_t;

static win_t      g_win[MAXWIN];
static int        g_z[MAXWIN], g_nz;     /* indices bottom..top */
static int        g_focus = -1;
static uint32_t   g_next_wid = 1;

static gfx_surface_t g_screen;           /* the mmap'd framebuffer */
static gfx_surface_t g_back;             /* scene back buffer (no cursor) */
static int        g_mx, g_my;            /* cursor position */
static int        g_buttons;             /* last button bitmask */
static int        g_dirty = 1;           /* scene needs a recompose */

/* drag / resize state (window index, or -1) */
static int        g_drag = -1, g_drag_dx, g_drag_dy;
static int        g_resize = -1;

/* ---- launcher ---- */
/* Apps the taskbar can start. The server must NOT fork() itself once it has
   mmap'd the framebuffer (forking a device mapping would corrupt the PMM), so a
   tiny helper process — forked BEFORE the framebuffer is mapped — does the
   fork/exec on our behalf. The server just writes the binary path down a pipe. */
typedef struct { const char *label; const char *path; } app_t;
static const app_t g_apps[] = {
    { "TERMINAL",   "/bin/gterm"    },
    { "MANDELBROT", "/bin/gmandel"  },
    { "CLOCK",      "/bin/gclock"   },
    { "HELLO",      "/bin/guihello" },
};
#define N_APPS ((int)(sizeof g_apps / sizeof g_apps[0]))
static int g_app_x[N_APPS], g_app_w[N_APPS];   /* taskbar hit rects */
static int g_taskbar_winx;                      /* where the window list starts */
static int g_launch_fd = -1;                    /* write end of the helper pipe */

#define C_LAUNCH    GFX_RGB(0x2c,0x6e,0x4a)     /* launcher button (green) */
#define C_LAUNCH_FG GFX_RGB(0xe0,0xff,0xe8)

/* The helper loop: never maps the framebuffer, so its fork/exec is clean. Reads
   newline-terminated binary paths from `rfd` and spawns each. */
static void run_launcher_helper(int rfd) {
    char buf[128]; int len = 0;
    for (;;) {
        char c; int r = read(rfd, &c, 1);
        if (r <= 0) { if (r == 0) _exit(0); else continue; }
        if (c == '\n') {
            buf[len] = 0;
            if (len > 0) {
                pid_t pid = fork();
                if (pid == 0) {
                    close(rfd);
                    char *av[] = { buf, 0 };
                    execve(buf, av, environ);
                    _exit(127);
                }
                while (waitpid(-1, 0, WNOHANG) > 0) { }   /* reap finished apps */
            }
            len = 0;
        } else if (len < (int)sizeof buf - 1) {
            buf[len++] = c;
        }
    }
}

static void launch(const char *path) {
    if (g_launch_fd < 0) return;
    char line[128]; int n = 0;
    for (const char *p = path; *p && n < (int)sizeof line - 2; p++) line[n++] = *p;
    line[n++] = '\n';
    (void)!write(g_launch_fd, line, n);
}

/* ---- cursor sprite (11x17), '#'=black '.'=white ' '=transparent ---- */
#define CUR_W 11
#define CUR_H 17
static const char *cur_xpm[CUR_H] = {
    "#          ", "##         ", "#.#        ", "#..#       ",
    "#...#      ", "#....#     ", "#.....#    ", "#......#   ",
    "#.......#  ", "#........# ", "#.....#### ", "#..#..#    ",
    "#.# #..#   ", "##  #..#   ", "#    #..#  ", "     #..#  ",
    "      ##   ",
};

static void msleep(int ms) { struct timespec t = { ms/1000, (long)(ms%1000)*1000000L }; nanosleep(&t,0); }

/* ---- window helpers ---- */
static void outer_rect(win_t *w, int *ox, int *oy, int *ow, int *oh) {
    *ox = w->x - BORDER;
    *oy = w->y - TITLEBAR_H - BORDER;
    *ow = w->w + 2*BORDER;
    *oh = w->h + TITLEBAR_H + 2*BORDER;
}

static int win_at(int px, int py) {              /* topmost window under point */
    for (int i = g_nz - 1; i >= 0; i--) {
        win_t *w = &g_win[g_z[i]];
        int ox,oy,ow,oh; outer_rect(w,&ox,&oy,&ow,&oh);
        if (px >= ox && px < ox+ow && py >= oy && py < oy+oh) return g_z[i];
    }
    return -1;
}

static void raise_win(int idx) {
    int at = -1;
    for (int i = 0; i < g_nz; i++) if (g_z[i] == idx) { at = i; break; }
    if (at < 0) return;
    for (int i = at; i < g_nz-1; i++) g_z[i] = g_z[i+1];
    g_z[g_nz-1] = idx;
    g_focus = idx;
}

static int add_window(int fd, int w, int h, const char *title) {
    int idx = -1;
    for (int i = 0; i < MAXWIN; i++) if (!g_win[i].used) { idx = i; break; }
    if (idx < 0) return -1;
    win_t *win = &g_win[idx];
    win->used = 1; win->fd = fd; win->wid = g_next_wid++;
    win->w = w; win->h = h;
    /* cascade placement */
    win->x = 80 + g_nz*40;
    win->y = TITLEBAR_H + BORDER + 60 + g_nz*40;
    win->pix = (uint32_t *)calloc((size_t)w*h, 4);
    win->rcap = w*h*4 + 64;
    win->rbuf = (uint8_t *)malloc(win->rcap);
    win->rgot = 0; win->rneed = sizeof(gmsg_hdr_t);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);   /* drained non-blocking */
    strncpy(win->title, title, sizeof win->title - 1);
    win->title[sizeof win->title - 1] = 0;
    g_z[g_nz++] = idx;
    g_focus = idx;
    g_dirty = 1;
    return idx;
}

static void remove_window(int idx) {
    win_t *w = &g_win[idx];
    if (!w->used) return;
    if (w->pix) free(w->pix);
    if (w->rbuf) free(w->rbuf);
    if (w->fd >= 0) close(w->fd);
    memset(w, 0, sizeof *w); w->fd = -1;
    int at = -1;
    for (int i = 0; i < g_nz; i++) if (g_z[i] == idx) { at = i; break; }
    if (at >= 0) { for (int i = at; i < g_nz-1; i++) g_z[i] = g_z[i+1]; g_nz--; }
    g_focus = g_nz ? g_z[g_nz-1] : -1;
    g_dirty = 1;
}

/* Resize a window's content to nw x nh. Grows the server-side pixel + receive
   buffers (preserving the old content top-left and any in-flight frame bytes —
   the rbuf state machine must stay aligned, so we never shrink it), and flags the
   client to re-render at the new size. Stale frames at the old size are dropped
   by apply_message's size check. */
static void do_resize(int idx, int nw, int nh) {
    win_t *w = &g_win[idx];
    if (nw < MIN_W) nw = MIN_W;
    if (nh < MIN_H) nh = MIN_H;
    if (nw == w->w && nh == w->h) return;

    uint32_t *np = (uint32_t *)calloc((size_t)nw * nh, 4);
    if (!np) return;
    if (w->pix) {
        int cw = nw < w->w ? nw : w->w, ch = nh < w->h ? nh : w->h;
        for (int y = 0; y < ch; y++)
            memcpy(np + (size_t)y * nw, w->pix + (size_t)y * w->w, (size_t)cw * 4);
        free(w->pix);
    }
    w->pix = np;
    int need = nw * nh * 4 + 64;
    if (need > w->rcap) {                          /* grow only — keep alignment */
        uint8_t *nb = (uint8_t *)realloc(w->rbuf, need);
        if (nb) { w->rbuf = nb; w->rcap = need; }
    }
    w->w = nw; w->h = nh;
    w->rz_pending = 1;                             /* delivered from the main loop */
    g_dirty = 1;
}

/* ---- compositing ---- */
static void draw_window(win_t *w, int focused) {
    int ox,oy,ow,oh; outer_rect(w,&ox,&oy,&ow,&oh);
    gfx_fill_rect(&g_back, ox, oy, ow, oh, C_BORDER);               /* border */
    gfx_fill_rect(&g_back, w->x, w->y - TITLEBAR_H, w->w, TITLEBAR_H,
                  focused ? C_TITLE : C_TITLE_BLUR);                 /* title bar */
    gfx_text(&g_back, w->x + 6, w->y - TITLEBAR_H + 7, w->title, C_TITLE_FG);
    /* close box at the right of the title bar */
    gfx_fill_rect(&g_back, w->x + w->w - 16, w->y - TITLEBAR_H + 4, 12, 12, C_CLOSE);
    gfx_text(&g_back, w->x + w->w - 14, w->y - TITLEBAR_H + 6, "X", C_TITLE_FG);
    if (w->pix) gfx_blit(&g_back, w->pix, w->w, w->h, w->x, w->y);   /* content */
    /* resize grip: a few diagonal lines in the bottom-right corner */
    int gx = w->x + w->w, gy = w->y + w->h;
    for (int d = 4; d < GRIP; d += 4)
        for (int t = 0; t < 2; t++)
            gfx_pixel(&g_back, gx - d + t, gy - 2, GFX_RGB(0xc0,0xc8,0xd0)),
            gfx_pixel(&g_back, gx - 2, gy - d + t, GFX_RGB(0xc0,0xc8,0xd0));
}

static void compose(void) {
    gfx_clear(&g_back, C_DESKTOP);
    /* logo + label, upper third */
    gfx_blit_alpha(&g_back, vibeos_logo, vibeos_logo_w, vibeos_logo_h,
                   (g_back.w - vibeos_logo_w)/2, g_back.h/6);
    gfx_text(&g_back, 16, 12, "VIBEOS DESKTOP  (userspace WM)", GFX_RGB(0xa0,0xc0,0xe0));
    /* windows bottom..top */
    for (int i = 0; i < g_nz; i++)
        draw_window(&g_win[g_z[i]], g_z[i] == g_focus);
    /* taskbar */
    int ty = g_back.h - TASKBAR_H;
    gfx_fill_rect(&g_back, 0, ty, g_back.w, TASKBAR_H, C_TASKBAR);
    int tx = 8;
    /* launcher buttons (start apps) */
    for (int i = 0; i < N_APPS; i++) {
        int bw = gfx_text_w(g_apps[i].label) + 16;
        gfx_fill_rect(&g_back, tx, ty + 3, bw, TASKBAR_H - 6, C_LAUNCH);
        gfx_rect(&g_back, tx, ty + 3, bw, TASKBAR_H - 6, C_LAUNCH_FG);
        gfx_text(&g_back, tx + 8, ty + 8, g_apps[i].label, C_LAUNCH_FG);
        g_app_x[i] = tx; g_app_w[i] = bw;
        tx += bw + 6;
    }
    /* separator, then the open-window list */
    tx += 6; gfx_vline(&g_back, tx - 8, ty + 3, TASKBAR_H - 6, GFX_RGB(0x40,0x50,0x60));
    g_taskbar_winx = tx;
    for (int i = 0; i < g_nz; i++) {
        win_t *w = &g_win[g_z[i]];
        int bw = gfx_text_w(w->title) + 16;
        gfx_fill_rect(&g_back, tx, ty + 3, bw, TASKBAR_H - 6,
                      g_z[i] == g_focus ? C_TITLE : C_TITLE_BLUR);
        gfx_text(&g_back, tx + 8, ty + 8, w->title, C_TASK_FG);
        tx += bw + 8;
    }
}

/* copy a back-buffer rect to the framebuffer (clipped) */
static void present_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_screen.w) w = g_screen.w - x;
    if (y + h > g_screen.h) h = g_screen.h - y;
    for (int row = 0; row < h; row++)
        memcpy(g_screen.px + (size_t)(y+row)*g_screen.stride + x,
               g_back.px   + (size_t)(y+row)*g_back.stride   + x, (size_t)w*4);
}

static void draw_cursor(void) {
    for (int r = 0; r < CUR_H; r++)
        for (int c = 0; c < CUR_W; c++) {
            char ch = cur_xpm[r][c];
            if (ch == ' ') continue;
            gfx_pixel(&g_screen, g_mx + c, g_my + r,
                      ch == '#' ? GFX_RGB(0,0,0) : GFX_RGB(0xff,0xff,0xff));
        }
}

/* ---- client protocol ---- */
static int read_full(int fd, void *buf, int n) {
    uint8_t *p = buf; int got = 0;
    while (got < n) { int r = read(fd, p+got, n-got); if (r <= 0) return -1; got += r; }
    return 0;
}
static int write_full(int fd, const void *buf, int n) {
    const uint8_t *p = buf; int put = 0;
    while (put < n) { int r = write(fd, p+put, n-put); if (r <= 0) return -1; put += r; }
    return 0;
}

/* Flush a window's pending outgoing event (non-blocking). */
static void flush_out(win_t *w) {
    while (w->ooff < w->olen) {
        int r = write(w->fd, w->obuf + w->ooff, w->olen - w->ooff);
        if (r <= 0) return;                      /* EAGAIN / error: try later */
        w->ooff += r;
    }
    w->olen = w->ooff = 0;
}

static void send_input(win_t *w, int ev, int x, int y, int buttons, int key) {
    flush_out(w);
    if (w->olen) return;                          /* previous event still in flight: drop */
    gmsg_hdr_t hdr = { GEVT_INPUT, sizeof(gevt_input_t) };
    gevt_input_t e = { (uint32_t)ev, x, y, (uint32_t)buttons, (uint32_t)key };
    memcpy(w->obuf, &hdr, sizeof hdr);
    memcpy(w->obuf + sizeof hdr, &e, sizeof e);
    w->olen = sizeof hdr + sizeof e; w->ooff = 0;
    flush_out(w);
}

/* Act on one fully-received message sitting in w->rbuf (header + body). */
static void apply_message(win_t *w) {
    gmsg_hdr_t *hdr = (gmsg_hdr_t *)w->rbuf;
    if (hdr->type == GMSG_FRAME) {
        gmsg_frame_t *fr = (gmsg_frame_t *)(w->rbuf + sizeof *hdr);
        uint32_t *src = (uint32_t *)(w->rbuf + sizeof *hdr + sizeof *fr);
        /* Blit the damage rect (fr->x,fr->y,fr->w,fr->h), supplied tightly
           packed, into the window's pixel buffer. Clip to the current size so a
           stale frame at the old size (after a resize) writes only what fits and
           never runs past w->pix. */
        int dx = (int)fr->x, dy = (int)fr->y, fw = (int)fr->w, fh = (int)fr->h;
        if (w->pix && dx >= 0 && dy >= 0 && fw > 0 && fh > 0) {
            int cw = fw, ch = fh;
            if (dx + cw > w->w) cw = w->w - dx;
            if (dy + ch > w->h) ch = w->h - dy;
            for (int row = 0; row < ch; row++)
                memcpy(w->pix + (size_t)(dy + row) * w->w + dx,
                       src + (size_t)row * fw, (size_t)cw * 4);
        }
        g_dirty = 1;
    }
    /* GMSG_CLOSE is handled by the caller (returns drop); others ignored. */
}

/* Drain a window's socket without blocking; returns 0 ok, -1 to drop window.
   Reassembles messages across reads via the per-window rbuf state machine. */
static int pump_client(int idx) {
    win_t *w = &g_win[idx];
    for (;;) {
        if (w->rneed > w->rcap) return -1;          /* oversized: protocol error */
        int want = w->rneed - w->rgot;
        int r = read(w->fd, w->rbuf + w->rgot, want);
        if (r == 0) return -1;                       /* peer closed */
        if (r < 0) return 0;                          /* EAGAIN: nothing more now */
        w->rgot += r;
        if (w->rgot < w->rneed) continue;             /* need more bytes */
        gmsg_hdr_t *hdr = (gmsg_hdr_t *)w->rbuf;
        if (w->rneed == (int)sizeof(gmsg_hdr_t)) {    /* header complete */
            if (hdr->type == GMSG_CLOSE) return -1;
            w->rneed = sizeof(gmsg_hdr_t) + hdr->len; /* now read the body */
            if (hdr->len == 0) { w->rgot = 0; w->rneed = sizeof(gmsg_hdr_t); }
        } else {                                      /* body complete */
            apply_message(w);
            w->rgot = 0; w->rneed = sizeof(gmsg_hdr_t);
        }
    }
}

/* ---- input routing ---- */
static void on_mouse(int x, int y, int buttons) {
    int old_mx = g_mx, old_my = g_my;
    g_mx = x; g_my = y;
    int pressed = buttons & ~g_buttons;      /* newly pressed bits */
    int released = g_buttons & ~buttons;
    g_buttons = buttons;

    if (g_resize >= 0) {
        if (released & 1) { g_resize = -1; }  /* drop */
        else {
            win_t *w = &g_win[g_resize];
            do_resize(g_resize, x - w->x, y - w->y);
        }
    } else if (g_drag >= 0) {
        if (released & 1) { g_drag = -1; }   /* drop */
        else {
            win_t *w = &g_win[g_drag];
            w->x = x - g_drag_dx; w->y = y - g_drag_dy;
            g_dirty = 1;
        }
    } else if (pressed & 1) {
        /* taskbar launcher: a click on an app button starts it */
        int ty = g_screen.h - TASKBAR_H;
        if (y >= ty) {
            for (int i = 0; i < N_APPS; i++)
                if (x >= g_app_x[i] && x < g_app_x[i] + g_app_w[i]) {
                    printf("[guiwm] launch %s\n", g_apps[i].path);
                    launch(g_apps[i].path);
                    return;
                }
        }
        int idx = win_at(x, y);
        if (idx >= 0) {
            raise_win(idx); g_dirty = 1;
            win_t *w = &g_win[idx];
            /* close box? */
            int cbx = w->x + w->w - 16, cby = w->y - TITLEBAR_H + 4;
            if (x >= cbx && x < cbx+12 && y >= cby && y < cby+12) {
                send_input(w, GE_KEY, 0, 0, 0, 0);   /* nudge; client may ignore */
                remove_window(idx);
                return;
            }
            if (x >= w->x + w->w - GRIP && y >= w->y + w->h - GRIP) {
                g_resize = idx;              /* bottom-right grip: start resize */
            } else if (y < w->y) {           /* title bar: start drag */
                g_drag = idx; g_drag_dx = x - w->x; g_drag_dy = y - w->y;
            } else {                          /* content: forward */
                send_input(w, GE_MOUSE_DOWN, x - w->x, y - w->y, buttons, 0);
            }
        }
    } else if (released & 1) {
        if (g_focus >= 0) {
            win_t *w = &g_win[g_focus];
            send_input(w, GE_MOUSE_UP, x - w->x, y - w->y, buttons, 0);
        }
    } else {                                  /* motion */
        if (g_focus >= 0) {
            win_t *w = &g_win[g_focus];
            if (x >= w->x && x < w->x+w->w && y >= w->y && y < w->y+w->h)
                send_input(w, GE_MOUSE_MOVE, x - w->x, y - w->y, buttons, 0);
        }
    }

    /* present: erase old cursor + draw new (scene recompose handled in main) */
    if (!g_dirty) {
        present_rect(old_mx, old_my, CUR_W, CUR_H);
        draw_cursor();
    }
}

static void on_key(int code, int mods) {
    if (g_focus >= 0) send_input(&g_win[g_focus], GE_KEY, 0, 0, mods, code);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[guiwm] starting userspace window manager\n");

    /* Spawn the launcher helper BEFORE we map the framebuffer — it forks/execs
       apps so the compositor never has to fork() with a device mapping live. */
    int lp[2];
    if (pipe(lp) == 0) {
        pid_t pid = fork();
        if (pid == 0) { close(lp[1]); run_launcher_helper(lp[0]); _exit(0); }
        close(lp[0]); g_launch_fd = lp[1];
    }

    int fb = open("/dev/fb0", O_RDWR);
    if (fb < 0) { printf("[guiwm] cannot open /dev/fb0\n"); return 1; }
    gfb_info_t fi;
    if (read(fb, &fi, sizeof fi) != (int)sizeof fi) { printf("[guiwm] fbinfo read failed\n"); return 1; }
    uint32_t *fbpx = mmap(0, fi.size, PROT_READ|PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbpx == (void*)-1) { printf("[guiwm] mmap /dev/fb0 failed\n"); return 1; }
    g_screen = gfx_wrap(fbpx, fi.width, fi.height, fi.pitch/4);
    g_back = gfx_alloc(fi.width, fi.height);
    if (!g_back.px) { printf("[guiwm] back buffer alloc failed\n"); return 1; }
    printf("[guiwm] screen %ux%u; compositor up\n", fi.width, fi.height);

    int in = open("/dev/input", O_RDONLY);     /* grabs input from the TTY */
    if (in < 0) { printf("[guiwm] cannot open /dev/input\n"); return 1; }
    g_mx = fi.width/2; g_my = fi.height/2;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(GUI_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls,(struct sockaddr*)&a,sizeof a) || listen(ls, 8)) {
        printf("[guiwm] bind/listen :%d failed\n", GUI_PORT); return 1; }
    fcntl(ls, F_SETFL, fcntl(ls,F_GETFL,0) | O_NONBLOCK);
    printf("[guiwm] listening on 127.0.0.1:%d\n", GUI_PORT);

    for (int i = 0; i < MAXWIN; i++) g_win[i].fd = -1;

    /* first paint */
    compose(); present_rect(0,0,g_screen.w,g_screen.h); draw_cursor();
    g_dirty = 0;

    struct pollfd pfd[MAXWIN+1];
    for (;;) {
        int np = 0;
        pfd[np].fd = ls; pfd[np].events = POLLIN; np++;
        int map[MAXWIN+1]; map[0] = -1;
        for (int i = 0; i < MAXWIN; i++) if (g_win[i].used) {
            pfd[np].fd = g_win[i].fd; pfd[np].events = POLLIN; map[np] = i; np++;
        }
        poll(pfd, np, 15);                     /* 15ms: ~60Hz input cadence */

        /* new connections */
        if (pfd[0].revents & POLLIN) {
            int cs = accept(ls, 0, 0);
            if (cs >= 0) {
                gmsg_hdr_t hdr;
                if (!read_full(cs, &hdr, sizeof hdr) && hdr.type == GMSG_CREATE) {
                    gmsg_create_t cr;
                    if (!read_full(cs, &cr, sizeof cr)) {
                        int idx = add_window(cs, cr.w, cr.h, cr.title);
                        if (idx >= 0) {
                            gmsg_hdr_t rh = { GEVT_CREATED, sizeof(gevt_created_t) };
                            gevt_created_t cd = { g_win[idx].wid, (uint32_t)cr.w, (uint32_t)cr.h };
                            write_full(cs, &rh, sizeof rh);
                            write_full(cs, &cd, sizeof cd);
                            printf("[guiwm] window %u \"%s\" %dx%d\n", g_win[idx].wid, cr.title, cr.w, cr.h);
                        } else close(cs);
                    } else close(cs);
                } else close(cs);
            }
        }
        /* client messages — drain every window non-blocking (poll() on a
           connected socket isn't reliable yet, so we don't depend on it) */
        for (int i = 0; i < MAXWIN; i++) {
            if (!g_win[i].used) continue;
            flush_out(&g_win[i]);                  /* drain any pending event */
            /* deliver an owed resize once the channel is free, so the client
               always converges on the current size even if drags dropped some */
            if (g_win[i].rz_pending && g_win[i].olen == 0) {
                send_input(&g_win[i], GE_RESIZE, g_win[i].w, g_win[i].h, 0, 0);
                if (g_win[i].olen == 0) g_win[i].rz_pending = 0;   /* sent */
            }
            if (pump_client(i) < 0) {
                printf("[guiwm] window %u closed\n", g_win[i].wid);
                remove_window(i);
            }
        }
        (void)map;
        /* input events */
        gin_event_t ev[32];
        int n = read(in, ev, sizeof ev);
        for (int k = 0; k < n/(int)sizeof(gin_event_t); k++) {
            if (ev[k].type == GIN_MOUSE) on_mouse(ev[k].x, ev[k].y, ev[k].buttons);
            else if (ev[k].type == GIN_KEY) on_key(ev[k].code, ev[k].buttons);
        }
        /* present scene if it changed */
        if (g_dirty) {
            compose();
            present_rect(0, 0, g_screen.w, g_screen.h);
            draw_cursor();
            g_dirty = 0;
        }
    }
    return 0;
}
