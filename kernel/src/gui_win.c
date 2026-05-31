#include "kernel.h"
#include "pmm.h"
#include "paging.h"
#include "gui_win.h"

/* libwin — windows + controls. See gui_win.h. */

/* palette */
#define C_TITLE_ACTIVE   RGB(0x20,0x40,0x90)
#define C_TITLE_TEXT     RGB(0xFF,0xFF,0xFF)
#define C_BORDER         RGB(0x10,0x10,0x10)
#define C_FACE           RGB(0xC8,0xC8,0xC8)
#define C_BTN            RGB(0xDC,0xDC,0xDC)
#define C_BTN_DOWN       RGB(0xA8,0xA8,0xA8)
#define C_BTN_HI         RGB(0xFF,0xFF,0xFF)
#define C_BTN_LO         RGB(0x70,0x70,0x70)
#define C_TEXT           RGB(0x00,0x00,0x00)

static window_t g_winpool[8];
static int      g_winpool_n;

static uint32_t *alloc_pixels(int w, int h) {
    uint64_t bytes = (uint64_t)w * h * 4;
    uint64_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t pa = pmm_alloc_pages(pages);
    if (!pa) return nullptr;
    return (uint32_t *)phys_to_virt(pa);
}

window_t *win_create(int x, int y, int w, int h, const char *title) {
    if (g_winpool_n >= 8) return nullptr;
    window_t *win = &g_winpool[g_winpool_n++];
    kmemset(win, 0, sizeof *win);
    win->frame = (rect_t){ x, y, w, h };
    win->surface.pixels = alloc_pixels(w, h);
    win->surface.w = w; win->surface.h = h; win->surface.stride = w;
    if (!win->surface.pixels) { g_winpool_n--; return nullptr; }
    int i = 0; for (; title[i] && i < (int)sizeof win->title - 1; i++) win->title[i] = title[i];
    win->title[i] = '\0';
    win->dirty = 1;
    return win;
}

static widget_t *add_widget(window_t *w, int type) {
    if (w->nwidgets >= 8) return nullptr;
    widget_t *wd = &w->widgets[w->nwidgets++];
    kmemset(wd, 0, sizeof *wd);
    wd->type = type;
    return wd;
}

widget_t *win_add_button(window_t *w, int x, int y, int bw, int bh, const char *label,
                         void (*cb)(widget_t *, window_t *)) {
    widget_t *wd = add_widget(w, W_BUTTON);
    if (!wd) return nullptr;
    wd->bounds = (rect_t){ x, y, bw, bh };
    int i = 0; for (; label[i] && i < 31; i++) wd->label[i] = label[i]; wd->label[i] = '\0';
    wd->on_click = cb;
    w->dirty = 1;
    return wd;
}

widget_t *win_add_label(window_t *w, int x, int y, const char *text) {
    widget_t *wd = add_widget(w, W_LABEL);
    if (!wd) return nullptr;
    wd->bounds = (rect_t){ x, y, 0, 0 };
    int i = 0; for (; text[i] && i < 39; i++) wd->label[i] = text[i]; wd->label[i] = '\0';
    w->dirty = 1;
    return wd;
}

widget_t *win_add_textbox(window_t *w, int x, int y, int bw, int bh) {
    widget_t *wd = add_widget(w, W_TEXTBOX);
    if (!wd) return nullptr;
    wd->bounds = (rect_t){ x, y, bw, bh };
    wd->label[0] = '\0'; wd->len = 0;
    w->dirty = 1;
    return wd;
}

/* content origin within the window surface */
static inline int content_x(void) { return BORDER; }
static inline int content_y(void) { return TITLEBAR_H; }

static void paint_button(surface_t *s, widget_t *b) {
    int x = content_x() + b->bounds.x, y = content_y() + b->bounds.y;
    int w = b->bounds.w, h = b->bounds.h;
    draw_fill_rect(s, x, y, w, h, b->pressed ? C_BTN_DOWN : C_BTN);
    /* 3D bevel */
    draw_hline(s, x, y, w, b->pressed ? C_BTN_LO : C_BTN_HI);
    draw_vline(s, x, y, h, b->pressed ? C_BTN_LO : C_BTN_HI);
    draw_hline(s, x, y + h - 1, w, b->pressed ? C_BTN_HI : C_BTN_LO);
    draw_vline(s, x + w - 1, y, h, b->pressed ? C_BTN_HI : C_BTN_LO);
    int tw = (int)kstrlen(b->label) * GLYPH_W;
    int tx = x + (w - tw) / 2 + (b->pressed ? 1 : 0);
    int ty = y + (h - GLYPH_H) / 2 + (b->pressed ? 1 : 0);
    draw_text(s, tx, ty, b->label, C_TEXT, 0, 1);
}

static void paint_textbox(surface_t *s, widget_t *t) {
    int x = content_x() + t->bounds.x, y = content_y() + t->bounds.y;
    int w = t->bounds.w, h = t->bounds.h;
    draw_fill_rect(s, x, y, w, h, RGB(0xFF,0xFF,0xFF));        /* white field */
    draw_hline(s, x, y, w, C_BTN_LO); draw_vline(s, x, y, h, C_BTN_LO);   /* sunken */
    draw_hline(s, x, y + h - 1, w, C_BTN_HI); draw_vline(s, x + w - 1, y, h, C_BTN_HI);
    int tx = x + 4, ty = y + (h - GLYPH_H) / 2;
    draw_text(s, tx, ty, t->label, C_TEXT, 0, 1);
    if (t->focused) {                                          /* caret after the text */
        int cx = tx + t->len * GLYPH_W;
        draw_vline(s, cx, ty - 1, GLYPH_H + 2, C_TEXT);
    }
}

void win_paint(window_t *w) {
    surface_t *s = &w->surface;
    draw_clear(s, C_FACE);
    /* title bar */
    draw_fill_rect(s, 0, 0, s->w, TITLEBAR_H, C_TITLE_ACTIVE);
    draw_text(s, 6, (TITLEBAR_H - GLYPH_H) / 2, w->title, C_TITLE_TEXT, 0, 1);
    /* outer border */
    draw_rect(s, 0, 0, s->w, s->h, C_BORDER);
    /* widgets */
    for (int i = 0; i < w->nwidgets; i++) {
        widget_t *wd = &w->widgets[i];
        if (wd->type == W_BUTTON) paint_button(s, wd);
        else if (wd->type == W_TEXTBOX) paint_textbox(s, wd);
        else draw_text(s, content_x() + wd->bounds.x, content_y() + wd->bounds.y,
                       wd->label, C_TEXT, 0, 1);
    }
    w->dirty = 0;
}

int win_titlebar_hit(window_t *w, int lx, int ly) {
    return ly >= 0 && ly < TITLEBAR_H && lx >= 0 && lx < w->surface.w;
}

widget_t *win_widget_at(window_t *w, int lx, int ly) {
    int cx = lx - content_x(), cy = ly - content_y();
    for (int i = w->nwidgets - 1; i >= 0; i--) {
        widget_t *wd = &w->widgets[i];
        if (wd->type != W_BUTTON && wd->type != W_TEXTBOX) continue;  /* interactive only */
        if (cx >= wd->bounds.x && cx < wd->bounds.x + wd->bounds.w &&
            cy >= wd->bounds.y && cy < wd->bounds.y + wd->bounds.h)
            return wd;
    }
    return nullptr;
}
