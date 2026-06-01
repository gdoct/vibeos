/* gterm — a VibeOS terminal emulator (GUI phase 2).
 *
 * A graphical VT for the existing serial shell. gterm is an ordinary GUI client:
 * it opens one window via the WM, then fork+execs /bin/sh with its stdin/stdout
 * wired to a pair of pipes. The terminal owns the screen side of those pipes:
 *
 *   keystrokes (GE_KEY)  --local line edit + echo-->  write() to sh's stdin
 *   sh + child output    --read() from sh's stdout-->  scrollback grid -> window
 *
 * Because there is no pty, gterm does the job a tty driver normally would: it
 * line-buffers and echoes input locally, and only ships a whole line (+'\n') to
 * the shell on Enter — matching /bin/sh, which read()s one line per command.
 *
 * The window is a character grid (g_cols x g_vis_rows) over a ring of scrollback
 * lines, with a draggable scrollbar to page back through history. The grid is
 * derived from the window's pixel size and recomputed on GE_RESIZE, so the
 * window is freely resizable. Output and typing always snap the view to the
 * bottom (latest), like a vtty.
 *
 * Rendering pushes only what changed: redraw() repaints the whole local surface
 * (cheap), then commit_damage() diffs against the last committed frame and ships
 * just the changed scanline band. A keystroke touches one text row (~tens of KB)
 * instead of the whole window (~hundreds of KB) — the difference between instant
 * and multi-second echo over the loopback frame channel.
 *
 * Build: dropped into user/musl, it is picked up by the GUI client rule in the
 * Makefile (links libgfx + gui_client) and installed as /bin/gterm. Launch it
 * from guiwm's taskbar ("TERMINAL") or run /bin/gterm under the WM.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

#include "gui_client.h"

extern char **environ;

/* ---- grid geometry (pixels derive from the 8x8 glyph) ---- */
#define CELL_W    8                 /* GFX_GLYPH_W */
#define GLYPH_H   8                 /* GFX_GLYPH_H */
#define LINE_H    11                /* glyph + leading */
#define MARGIN_X  6
#define MARGIN_Y  6
#define SB_GAP    4                 /* gap between text area and scrollbar */
#define SB_W      12                /* scrollbar width */
#define TRACK_Y   MARGIN_Y

/* default (initial) window: an 80x26 grid, matching the classic size */
#define DEF_COLS  80
#define DEF_ROWS  26
#define DEF_W     (MARGIN_X + DEF_COLS*CELL_W + SB_GAP + SB_W + MARGIN_X)
#define DEF_H     (MARGIN_Y + DEF_ROWS*LINE_H + MARGIN_Y)

/* ---- colors ---- */
#define C_BG      GFX_RGB(0x10,0x12,0x18)
#define C_FG      GFX_RGB(0xc8,0xd0,0xd8)
#define C_CURSOR  GFX_RGB(0x40,0xe0,0x70)
#define C_SB_TRK  GFX_RGB(0x1c,0x20,0x28)
#define C_SB_THM  GFX_RGB(0x46,0x56,0x68)

/* ---- scrollback: a ring of fixed-width lines ---- */
#define MAXLINES  500
#define MAX_COLS  256               /* storage width; g_cols clamps to this */
static char sb[MAXLINES][MAX_COLS];
static int  sb_len[MAXLINES];       /* used cells in each line */
static int  g_top   = 0;            /* physical index of logical line 0 */
static int  g_count = 1;            /* number of logical lines (>=1) */
static int  g_col   = 0;            /* cursor column on the last line */
static int  g_scroll = 0;           /* lines scrolled up from the bottom */

/* ---- live geometry, recomputed from the window's pixel size ---- */
static int  g_cols     = DEF_COLS;
static int  g_vis_rows = DEF_ROWS;
static int  g_sb_x     = MARGIN_X + DEF_COLS*CELL_W + SB_GAP;   /* scrollbar x */
static int  g_track_h  = DEF_ROWS*LINE_H;                       /* scrollbar track */

/* Derive the grid + scrollbar layout from a window content size of w x h. */
static void recompute_geometry(int w, int h) {
    int text_w = w - 2*MARGIN_X - SB_GAP - SB_W;
    int text_h = h - 2*MARGIN_Y;
    g_cols = text_w / CELL_W;
    if (g_cols < 1) g_cols = 1;
    if (g_cols > MAX_COLS) g_cols = MAX_COLS;
    g_vis_rows = text_h / LINE_H;
    if (g_vis_rows < 1) g_vis_rows = 1;
    g_sb_x    = w - MARGIN_X - SB_W;
    g_track_h = g_vis_rows * LINE_H;
}

static int phys(int logical) { return (g_top + logical) % MAXLINES; }

static void clear_line(int p) { sb_len[p] = 0; memset(sb[p], ' ', MAX_COLS); }

static void term_newline(void) {
    if (g_count < MAXLINES) {
        g_count++;
    } else {
        g_top = (g_top + 1) % MAXLINES;     /* drop the oldest line */
    }
    clear_line(phys(g_count - 1));
    g_col = 0;
}

static void put_cell(char c) {
    if (g_col >= g_cols) term_newline();    /* wrap at the current width */
    int p = phys(g_count - 1);
    sb[p][g_col] = c;
    if (g_col + 1 > sb_len[p]) sb_len[p] = g_col + 1;
    g_col++;
}

/* Feed one output byte through the terminal, handling the control chars a
   line-oriented shell actually emits. */
static void term_putc(char c) {
    switch (c) {
    case '\n': term_newline();              break;
    case '\r': g_col = 0;                    break;
    case '\b': if (g_col > 0) g_col--;       break;
    case '\t': { int n = (g_col + 8) & ~7;
                 while (g_col < n && g_col < g_cols) put_cell(' '); } break;
    default:
        if ((unsigned char)c >= 32 && (unsigned char)c < 127) put_cell(c);
        break;                              /* drop other control bytes */
    }
}

static void term_write(const char *buf, int n) {
    for (int i = 0; i < n; i++) term_putc(buf[i]);
    g_scroll = 0;                            /* output snaps view to bottom */
}

/* Erase the most recently echoed input char from the current line. */
static void echo_backspace(void) {
    if (g_col <= 0) return;
    g_col--;
    int p = phys(g_count - 1);
    sb[p][g_col] = ' ';
    if (g_col + 1 == sb_len[p]) sb_len[p] = g_col;
}

/* ---- rendering ---- */
static int max_scroll(void) {
    return g_count > g_vis_rows ? g_count - g_vis_rows : 0;
}

static void draw_scrollbar(gfx_surface_t *s) {
    gfx_fill_rect(s, g_sb_x, TRACK_Y, SB_W, g_track_h, C_SB_TRK);
    int ms = max_scroll();
    int thumb_h = g_track_h, thumb_y = TRACK_Y;
    if (g_count > g_vis_rows) {
        thumb_h = g_track_h * g_vis_rows / g_count;
        if (thumb_h < 16) thumb_h = 16;
        int top_logical = (g_count - 1 - g_scroll) - (g_vis_rows - 1);
        if (top_logical < 0) top_logical = 0;
        thumb_y = TRACK_Y + (ms ? (g_track_h - thumb_h) * top_logical / ms : 0);
    }
    gfx_fill_rect(s, g_sb_x + 1, thumb_y, SB_W - 2, thumb_h, C_SB_THM);
}

static void redraw(gfx_surface_t *s) {
    gfx_clear(s, C_BG);

    if (g_scroll > max_scroll()) g_scroll = max_scroll();
    int bottom = g_count - 1 - g_scroll;
    int top    = bottom - (g_vis_rows - 1);

    for (int r = 0; r < g_vis_rows; r++) {
        int li = top + r;
        if (li < 0 || li >= g_count) continue;
        int p = phys(li), y = MARGIN_Y + r * LINE_H;
        int n = sb_len[p] < g_cols ? sb_len[p] : g_cols;
        for (int c = 0; c < n; c++)
            if (sb[p][c] != ' ')
                gfx_char(s, MARGIN_X + c * CELL_W, y, sb[p][c], C_FG);
    }

    /* cursor: only meaningful at the bottom of the scrollback */
    if (g_scroll == 0) {
        int row = (g_count - 1) - top;
        int cc = g_col < g_cols ? g_col : g_cols - 1;
        if (row >= 0 && row < g_vis_rows)
            gfx_fill_rect(s, MARGIN_X + cc * CELL_W,
                          MARGIN_Y + row * LINE_H + GLYPH_H, CELL_W - 1, 2, C_CURSOR);
    }

    draw_scrollbar(s);
}

/* Compare the freshly rendered surface against the last committed frame (prev)
   and ship only the changed scanline band; then sync prev. Sends nothing when
   nothing changed. Full-width bands keep the diff cheap — typing touches one
   text row, so a keystroke ships ~one LINE_H-tall strip instead of the window. */
static void commit_damage(gui_conn_t *c, gfx_surface_t *s, gfx_surface_t *prev) {
    int y0 = -1, y1 = -1;
    for (int y = 0; y < s->h; y++) {
        const void *sr = s->px    + (size_t)y * s->stride;
        const void *pr = prev->px + (size_t)y * prev->stride;
        if (memcmp(sr, pr, (size_t)s->w * 4) != 0) {
            if (y0 < 0) y0 = y;
            y1 = y;
        }
    }
    if (y0 < 0) return;                          /* nothing changed */
    gc_commit_rect(c, s, 0, y0, s->w, y1 - y0 + 1);
    for (int y = y0; y <= y1; y++)
        memcpy(prev->px + (size_t)y * prev->stride,
               s->px    + (size_t)y * s->stride, (size_t)s->w * 4);
}

/* Map a scrollbar y (window-local) to a scroll offset and apply it. */
static void scrollbar_to(int y) {
    int ms = max_scroll();
    if (ms <= 0) { g_scroll = 0; return; }
    int rel = y - TRACK_Y;
    if (rel < 0) rel = 0;
    if (rel > g_track_h) rel = g_track_h;
    int top_logical = rel * ms / g_track_h;        /* top of track = oldest */
    g_scroll = ms - top_logical;
    if (g_scroll < 0) g_scroll = 0;
    if (g_scroll > ms) g_scroll = ms;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    for (int i = 0; i < MAXLINES; i++) clear_line(i);

    /* Pipes: in[] carries our keystrokes to the shell's stdin; out[] carries
       the shell's (and its children's) stdout/stderr back to us. */
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) {
        printf("gterm: pipe failed\n"); return 1;
    }

    pid_t pid = fork();
    if (pid < 0) { printf("gterm: fork failed\n"); return 1; }
    if (pid == 0) {
        /* child -> /bin/sh with stdio on the pipes */
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]);
        close(out[0]); close(out[1]);
        char *av[] = { "/bin/sh", 0 };
        execve("/bin/sh", av, environ);
        _exit(127);
    }

    /* parent: keep the write-end of in[] and the read-end of out[] */
    close(in[0]); close(out[1]);
    int sh_in = in[1], sh_out = out[0];
    fcntl(sh_out, F_SETFL, fcntl(sh_out, F_GETFL, 0) | O_NONBLOCK);

    gui_conn_t *c = gc_open(DEF_W, DEF_H, "TERMINAL");
    if (!c) { printf("gterm: cannot reach the window manager\n"); return 1; }
    recompute_geometry(c->w, c->h);

    /* s = what we draw; prev = last frame the server has, for damage diffing. */
    gfx_surface_t s = gfx_alloc(c->w, c->h);
    gfx_surface_t prev = gfx_alloc(c->w, c->h);
    if (!s.px || !prev.px) { gc_close(c); return 1; }
    gfx_clear(&prev, 0);                /* != C_BG, so the first frame is full */

    char inbuf[512]; int inlen = 0;     /* current input line (pre-Enter) */
    int sb_drag = 0;                    /* dragging the scrollbar thumb */
    redraw(&s); commit_damage(c, &s, &prev);

    for (;;) {
        int dirty = 0;

        /* 1. drain the shell's output (non-blocking) */
        char rb[1024];
        for (int iter = 0; iter < 32; iter++) {
            int r = read(sh_out, rb, sizeof rb);
            if (r > 0)      { term_write(rb, r); dirty = 1; }
            else if (r == 0) { goto shell_gone; }       /* EOF: shell exited */
            else            break;                       /* EAGAIN */
        }

        /* 2. window + input events */
        gevt_input_t ev;
        int r;
        while ((r = gc_poll(c, &ev)) > 0) {
            if (ev.ev == GE_KEY) {
                unsigned k = ev.key;
                if (k == '\n' || k == '\r') {            /* submit the line */
                    term_putc('\n');
                    inbuf[inlen++] = '\n';
                    (void)!write(sh_in, inbuf, inlen);
                    inlen = 0; g_scroll = 0;
                } else if (k == 8 || k == 127) {         /* backspace */
                    if (inlen > 0) { inlen--; echo_backspace(); g_scroll = 0; }
                } else if (k >= 32 && k < 127) {         /* printable */
                    if (inlen < (int)sizeof inbuf - 2) {
                        inbuf[inlen++] = (char)k;
                        term_putc((char)k);
                        g_scroll = 0;
                    }
                }
                dirty = 1;
            } else if (ev.ev == GE_RESIZE) {
                /* WM resized us: re-fit the surfaces + grid and repaint whole. */
                int nw = c->w, nh = c->h;                /* gc_poll tracked these */
                gfx_free(&s);    s    = gfx_alloc(nw, nh);
                gfx_free(&prev); prev = gfx_alloc(nw, nh);
                if (!s.px || !prev.px) goto shell_gone;  /* OOM: bail cleanly */
                gfx_clear(&prev, 0);                     /* force a full frame */
                recompute_geometry(nw, nh);
                dirty = 1;
            } else if (ev.ev == GE_MOUSE_DOWN) {
                if (ev.x >= g_sb_x && ev.x < g_sb_x + SB_W) {
                    sb_drag = 1; scrollbar_to(ev.y); dirty = 1;
                }
            } else if (ev.ev == GE_MOUSE_MOVE) {
                if (sb_drag && (ev.buttons & 1)) { scrollbar_to(ev.y); dirty = 1; }
            } else if (ev.ev == GE_MOUSE_UP) {
                sb_drag = 0;
            }
        }
        if (r < 0) break;                                /* window closed */

        if (dirty) { redraw(&s); commit_damage(c, &s, &prev); }
        usleep(16000);                                   /* ~60 Hz */
    }

shell_gone:
    gfx_free(&s);
    gfx_free(&prev);
    gc_close(c);
    close(sh_in); close(sh_out);
    waitpid(pid, 0, WNOHANG);
    return 0;
}
