#ifndef VIBEOS_GUI_WIN_H
#define VIBEOS_GUI_WIN_H

#include "gui_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libwin — layer 2 of the GUI (graphics/graphics.md). Windows + controls: a
 * window owns a backing surface and a small widget list, knows how to paint its
 * chrome (title bar, border) and controls into that surface, and hit-tests
 * window-local points. No global/desktop state lives here (that is libwm).
 */

#define TITLEBAR_H  18
#define BORDER       2

struct window;
typedef struct widget {
    int      type;                 /* W_LABEL / W_BUTTON */
    rect_t   bounds;               /* relative to the window content origin */
    char     label[32];
    int      pressed;              /* button visual state */
    void   (*on_click)(struct widget *, struct window *);
} widget_t;

enum { W_LABEL = 0, W_BUTTON = 1 };

typedef struct window {
    surface_t surface;             /* backing store (owns pixels) */
    rect_t    frame;               /* screen position + size */
    char      title[48];
    widget_t  widgets[8];
    int       nwidgets;
    int       dirty;               /* needs repaint */
    void     *user;
} window_t;

window_t *win_create(int x, int y, int w, int h, const char *title);
widget_t *win_add_button(window_t *w, int x, int y, int bw, int bh, const char *label,
                         void (*cb)(widget_t *, window_t *));
widget_t *win_add_label(window_t *w, int x, int y, const char *text);

void      win_paint(window_t *w);                 /* render chrome + widgets into surface */
int       win_titlebar_hit(window_t *w, int lx, int ly);   /* window-local point in title bar? */
widget_t *win_widget_at(window_t *w, int lx, int ly);      /* widget under a window-local point */

#ifdef __cplusplus
}
#endif

#endif
