#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "task.h"
#include "timer.h"
#include "fb.h"
#include "gui.h"
#include "gui_draw.h"
#include "gui_win.h"

/*
 * libwm — layer 3: the desktop + window manager (graphics/graphics.md). Owns the
 * screen, a back buffer for flicker-free compositing, the window list (Z-order),
 * focus/drag state, and the mouse pointer. Each frame it pulls the mouse from the
 * USB driver, routes clicks/drags to windows (via libwin), composites desktop ->
 * windows -> cursor into the back buffer, and presents it.
 */

extern "C" void usb_mouse_get(int *x, int *y, int *buttons);
extern "C" const uint32_t vibeos_logo[];
extern "C" const int vibeos_logo_w, vibeos_logo_h;

#define C_DESKTOP   RGB(0x20,0x60,0x80)
#define CURSOR_KEY  RGB(0xFF,0x00,0xFF)     /* magenta = transparent */
#define CUR_W 11
#define CUR_H 17

/* Cursor sprite: 'X' outline, 'O' fill, '.' transparent. Hotspot at (0,0). */
static const char *cursor_xpm[CUR_H] = {
    "X..........",
    "XX.........",
    "XOX........",
    "XOOX.......",
    "XOOOX......",
    "XOOOOX.....",
    "XOOOOOX....",
    "XOOOOOOX...",
    "XOOOOOOOX..",
    "XOOOOOOOOX.",
    "XOOOOOXXXXX",
    "XOOXOOX....",
    "XOX.XOOX...",
    "XX..XOOX...",
    "X....XOOX..",
    ".....XOOX..",
    "......XX...",
};

static surface_t g_screen, g_scene, g_cursor;  /* g_scene = desktop+windows, no cursor */
static window_t *g_zorder[8];
static int       g_nwin;
static int       g_scene_dirty = 1;

/* demo state */
static window_t *g_demo;
static widget_t *g_counter_label;
static widget_t *g_textbox;
static int       g_clicks;
static char      g_counter_text[24];

static surface_t make_surface(int w, int h) {
    surface_t s; s.w = w; s.h = h; s.stride = w;
    uint64_t pages = ((uint64_t)w * h * 4 + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pa = pmm_alloc_pages(pages);
    s.pixels = pa ? (uint32_t *)phys_to_virt(pa) : nullptr;
    return s;
}

static void build_cursor(void) {
    g_cursor = make_surface(CUR_W, CUR_H);
    for (int y = 0; y < CUR_H; y++)
        for (int x = 0; x < CUR_W; x++) {
            char c = cursor_xpm[y][x];
            uint32_t v = (c == 'X') ? RGB(0,0,0) : (c == 'O') ? RGB(0xFF,0xFF,0xFF) : CURSOR_KEY;
            g_cursor.pixels[y * CUR_W + x] = v;
        }
}

static window_t *window_at(int sx, int sy) {
    for (int i = g_nwin - 1; i >= 0; i--) {        /* top-most first */
        window_t *w = g_zorder[i];
        if (sx >= w->frame.x && sx < w->frame.x + w->frame.w &&
            sy >= w->frame.y && sy < w->frame.y + w->frame.h)
            return w;
    }
    return nullptr;
}

static void raise_window(window_t *w) {
    int idx = -1;
    for (int i = 0; i < g_nwin; i++) if (g_zorder[i] == w) idx = i;
    if (idx < 0 || idx == g_nwin - 1) return;
    for (int i = idx; i < g_nwin - 1; i++) g_zorder[i] = g_zorder[i + 1];
    g_zorder[g_nwin - 1] = w;
}

/* Composite the cursor-less scene (desktop + windows) into g_scene. Heavy, so
   only done when the scene actually changes (window moved / raised / dirty). */
static void scene_compose(void) {
    draw_clear(&g_scene, C_DESKTOP);
    /* project logo, centered in the upper third of the desktop */
    draw_blit_alpha(&g_scene, vibeos_logo, vibeos_logo_w, vibeos_logo_h,
                    (g_scene.w - vibeos_logo_w) / 2, g_scene.h / 4 - vibeos_logo_h / 2);
    draw_text(&g_scene, 8, 8, "VIBEOS DESKTOP", RGB(0xFF,0xFF,0xFF), 0, 1);
    for (int i = 0; i < g_nwin; i++) {
        window_t *w = g_zorder[i];
        if (w->dirty) win_paint(w);
        draw_blit(&g_scene, &w->surface, w->frame.x, w->frame.y);
    }
    g_scene_dirty = 0;
}

/* Copy a scene rectangle to the screen (repaints scene, erasing any cursor). */
static void present_rect(int x, int y, int w, int h) {
    draw_copy_rect(&g_screen, &g_scene, x, y, w, h, x, y);
}

static void on_click_button(widget_t *wd, window_t *w) {
    (void)wd; (void)w;
    g_clicks++;
    int v = g_clicks, i = 0; char tmp[12];
    if (!v) tmp[i++] = '0';
    while (v) { tmp[i++] = (char)('0' + v % 10); v /= 10; }
    int o = 0; const char *p = "CLICKS: ";
    while (*p) g_counter_text[o++] = *p++;
    while (i) g_counter_text[o++] = tmp[--i];
    g_counter_text[o] = '\0';
    for (int k = 0; g_counter_text[k]; k++) g_counter_label->label[k] = g_counter_text[k];
    g_counter_label->label[o] = '\0';
    g_demo->dirty = 1;
    kprintf("[gui] button clicked (count=%d)\n", g_clicks);
}

/* WM input state (file scope so a self-test can drive the same handler). */
static int       g_pb;
static window_t *g_dragging; static int g_gx, g_gy;
static widget_t *g_pressed;  static window_t *g_pressed_win;
static widget_t *g_focus_text; static window_t *g_focus_win;   /* focused textbox */

/* Keyboard queue: the USB keyboard driver pushes characters here (any CPU) when
   a textbox is focused; the WM worker drains them. SPSC ring. */
#define KEYQ_N 64
static char              g_keyq[KEYQ_N];
static volatile uint32_t g_kq_head, g_kq_tail;

extern "C" int gui_wants_keyboard(void) { return g_focus_text != nullptr; }

extern "C" void gui_input_key(char c) {
    uint32_t n = (g_kq_head + 1) % KEYQ_N;
    if (n == g_kq_tail) return;
    g_keyq[g_kq_head] = c;
    __asm__ volatile("" ::: "memory");
    g_kq_head = n;
}

/* Deliver queued keystrokes to the focused textbox (WM-worker context). */
static void wm_drain_keys(void) {
    while (g_kq_tail != g_kq_head) {
        char c = g_keyq[g_kq_tail];
        g_kq_tail = (g_kq_tail + 1) % KEYQ_N;
        widget_t *t = g_focus_text;
        if (!t) continue;
        if (c == '\b' || c == 0x7F) { if (t->len > 0) t->label[--t->len] = '\0'; }
        else if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F && t->len < 38) {
            t->label[t->len++] = c; t->label[t->len] = '\0';
        }
        if (g_focus_win) g_focus_win->dirty = 1;
        g_scene_dirty = 1;
    }
}

static void set_focus(widget_t *t, window_t *w) {
    if (g_focus_text == t) return;
    if (g_focus_text) { g_focus_text->focused = 0; if (g_focus_win) g_focus_win->dirty = 1; }
    g_focus_text = t; g_focus_win = w;
    if (t) { t->focused = 1; if (w) w->dirty = 1; }
}

/* Process one (x, y, buttons) sample: routing (raise/focus), drag, and clicks.
   Sets g_scene_dirty when the scene changes. The single source of WM input
   behaviour — driven each frame by the real mouse, and by the self-test. */
static void wm_input(int x, int y, int b) {
    int down = (b & 1) && !(g_pb & 1);
    int up   = !(b & 1) && (g_pb & 1);

    if (down) {
        window_t *w = window_at(x, y);
        widget_t *newfocus = nullptr;
        if (w) {
            raise_window(w); g_scene_dirty = 1;
            int lx = x - w->frame.x, ly = y - w->frame.y;
            widget_t *wd = win_widget_at(w, lx, ly);
            if (wd && wd->type == W_TEXTBOX) { newfocus = wd; }
            else if (wd && wd->type == W_BUTTON) { wd->pressed = 1; w->dirty = 1; g_pressed = wd; g_pressed_win = w; }
            else if (win_titlebar_hit(w, lx, ly)) { g_dragging = w; g_gx = lx; g_gy = ly; }
        }
        set_focus(newfocus, w);                  /* clicking elsewhere unfocuses the textbox */
    }
    if (g_dragging && (b & 1)) { g_dragging->frame.x = x - g_gx; g_dragging->frame.y = y - g_gy; g_scene_dirty = 1; }
    if (up) {
        g_dragging = nullptr;
        if (g_pressed) {
            g_pressed->pressed = 0; g_pressed_win->dirty = 1;
            int lx = x - g_pressed_win->frame.x, ly = y - g_pressed_win->frame.y;
            if (win_widget_at(g_pressed_win, lx, ly) == g_pressed && g_pressed->on_click)
                g_pressed->on_click(g_pressed, g_pressed_win);
            g_pressed = nullptr;
        }
    }
    for (int i = 0; i < g_nwin; i++) if (g_zorder[i]->dirty) g_scene_dirty = 1;
    g_pb = b;
}

/* Deterministic self-test (no real input): synthesize a title-bar drag and a
   button click against wm_input, logging the outcome to serial. Proves the
   routing/drag/click logic independently of QEMU input injection. */
static void wm_selftest(void) {
    if (!g_demo) return;
    rect_t f0 = g_demo->frame;
    int tx = f0.x + 40, ty = f0.y + 6;             /* a point on the title bar */
    wm_input(tx, ty, 1);                           /* press */
    wm_input(tx - 120, ty - 80, 1);                /* drag */
    wm_input(tx - 120, ty - 80, 0);                /* release */
    kprintf("[gui] selftest: drag moved window (%d,%d) -> (%d,%d)\n",
            f0.x, f0.y, g_demo->frame.x, g_demo->frame.y);

    rect_t f = g_demo->frame;                        /* button center on the moved window */
    int bx = f.x + BORDER + 12 + 55, by = f.y + TITLEBAR_H + 48 + 11;
    int before = g_clicks;
    wm_input(bx, by, 1);                           /* press button */
    wm_input(bx, by, 0);                           /* release -> click */
    kprintf("[gui] selftest: button click %s (count %d -> %d)\n",
            g_clicks == before + 1 ? "OK" : "FAILED", before, g_clicks);

    /* focus the textbox and type into it (the keyboard path) */
    if (g_textbox) {
        int kx = f.x + BORDER + 12 + 4, ky = f.y + TITLEBAR_H + 28 + 4;
        wm_input(kx, ky, 1); wm_input(kx, ky, 0);  /* click the textbox -> focus */
        const char *typed = "hello";
        for (const char *p = typed; *p; p++) gui_input_key(*p);
        wm_drain_keys();
        kprintf("[gui] selftest: typed into textbox -> \"%s\" (%s)\n",
                g_textbox->label,
                (g_textbox->len == 5 && g_textbox->label[0] == 'h') ? "OK" : "FAILED");
        set_focus(nullptr, nullptr);
        g_textbox->label[0] = '\0'; g_textbox->len = 0;
    }

    g_demo->frame = f0;                              /* restore for interactive use */
    g_scene_dirty = 1;
}

static void wm_worker(void *arg) {
    (void)arg;
    int mx = g_screen.w / 2, my = g_screen.h / 2;

    wm_selftest();                                  /* verify drag/click logic on serial first */

    scene_compose();
    draw_blit(&g_screen, &g_scene, 0, 0);           /* one full present at startup */
    draw_blit_key(&g_screen, &g_cursor, mx, my, CURSOR_KEY);

    /* verify the logo blitted into the scene (read back composited pixels) */
    {
        int lx0 = (g_scene.w - vibeos_logo_w) / 2, ly0 = g_scene.h / 4 - vibeos_logo_h / 2;
        int nonbg = 0;
        for (int y = 0; y < vibeos_logo_h; y += 4)
            for (int x = 0; x < vibeos_logo_w; x += 4)
                if (g_scene.pixels[(uint32_t)(ly0 + y) * g_scene.stride + (lx0 + x)] != C_DESKTOP)
                    nonbg++;
        kprintf("[gui] selftest: logo rendered (%d non-desktop px in logo region) %s\n",
                nonbg, nonbg > 100 ? "OK" : "FAILED");
    }

    for (;;) {
        int x, y, b;
        usb_mouse_get(&x, &y, &b);
        if (x < 0) x = 0;
        if (x >= g_screen.w) x = g_screen.w - 1;
        if (y < 0) y = 0;
        if (y >= g_screen.h) y = g_screen.h - 1;

        wm_input(x, y, b);
        wm_drain_keys();                             /* deliver typed keys to the focused textbox */

        int moved = (x != mx || y != my);
        if (g_scene_dirty) {                         /* scene changed: recompose + full present */
            scene_compose();
            draw_blit(&g_screen, &g_scene, 0, 0);
            draw_blit_key(&g_screen, &g_cursor, x, y, CURSOR_KEY);
        } else if (moved) {                          /* cheap: just move the pointer */
            present_rect(mx, my, CUR_W, CUR_H);      /* erase old cursor from scene */
            draw_blit_key(&g_screen, &g_cursor, x, y, CURSOR_KEY);
        }

        mx = x; my = y;
        ksleep_ms(16);                               /* ~60 Hz */
    }
}

void gui_init(void) {
    fb_device_t *fb = fb_get();
    if (!fb) { kprintf("[gui] no framebuffer; GUI disabled\n"); return; }

    g_screen.pixels = (uint32_t *)fb->base;
    g_screen.w = (int)fb->width;
    g_screen.h = (int)fb->height;
    g_screen.stride = (int)(fb->pitch / 4);

    g_scene = make_surface(g_screen.w, g_screen.h);
    if (!g_scene.pixels) { kprintf("[gui] scene buffer alloc failed\n"); return; }
    build_cursor();

    /* a demo window: a label + a button that counts clicks */
    g_demo = win_create(g_screen.w / 2 - 130, g_screen.h / 2 - 80, 260, 150, "VibeOS Window");
    if (g_demo) {
        win_add_label(g_demo, 12, 10, "DRAG TITLE BAR. CLICK BOX + TYPE:");
        g_textbox = win_add_textbox(g_demo, 12, 28, 230, 18);
        win_add_button(g_demo, 12, 56, 110, 22, "CLICK ME", on_click_button);
        g_counter_label = win_add_label(g_demo, 130, 62, "CLICKS: 0");
        g_zorder[g_nwin++] = g_demo;
    }

    kprintf("[gui] %dx%d framebuffer; window manager up\n", g_screen.w, g_screen.h);
    task_create("guiwm", wm_worker, nullptr);
}
