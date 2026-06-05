/* gui_client — the client side of the GUI protocol (gui_proto.h). A small libc
 * over a loopback-TCP connection to /bin/guiwm: connect, create one window, push
 * frames, and poll for input events. Clients link this plus libgfx. */
#ifndef VIBEOS_GUI_CLIENT_H
#define VIBEOS_GUI_CLIENT_H

#include "libgfx.h"
#include "gui_proto.h"

typedef struct gui_conn {
    int      fd;
    uint32_t wid;
    int      w, h;
} gui_conn_t;

/* Connect to the WM and create a window of w*h with the given title. Returns a
   connection (heap) on success, or NULL on failure. The caller draws into a
   gfx_alloc(w,h) surface and calls gc_commit to show it. */
gui_conn_t *gc_open(int w, int h, const char *title);

/* Push the whole window surface to the server (a full-window frame). 0 on ok. */
int  gc_commit(gui_conn_t *c, gfx_surface_t *s);

/* Push only the sub-rectangle (x,y,w,h) of the surface as a damage frame — the
   server keeps the rest of the window untouched. Far cheaper than gc_commit when
   little changed (e.g. one text row). The rect must lie within the surface.
   0 on ok, -1 on error. */
int  gc_commit_rect(gui_conn_t *c, gfx_surface_t *s, int x, int y, int w, int h);

/* Non-blocking: 1 + fills ev if an event is pending, 0 if none, -1 if the
   connection closed (window closed by the WM). */
int  gc_poll(gui_conn_t *c, gevt_input_t *ev);

void gc_close(gui_conn_t *c);

#endif
