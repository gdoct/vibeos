/* VibeOS GUI phase-2 protocol — the wire format between the userspace window
 * manager (/bin/guiwm) and its clients, carried over a loopback TCP stream
 * (127.0.0.1:GUI_PORT). All integers are host-endian (both ends are the same
 * machine). One window per connection keeps the model simple; a client that
 * wants several windows opens several connections.
 *
 * Also defines the kernel ABI structs the server reads from /dev/fb0 + /dev/input
 * (kept here so the whole GUI userspace shares one header). */
#ifndef VIBEOS_GUI_PROTO_H
#define VIBEOS_GUI_PROTO_H

#include <stdint.h>

#define GUI_PORT 7000

/* ---- kernel ABI: /dev/fb0 geometry header (read()) ---- */
typedef struct gfb_info {
    uint32_t width, height, pitch, bpp;
    uint64_t size;
} gfb_info_t;

/* ---- kernel ABI: /dev/input events (read()) — mirrors kernel input.h ---- */
#define GIN_MOUSE 1
#define GIN_KEY   2
typedef struct gin_event {
    uint8_t  type;          /* GIN_MOUSE | GIN_KEY */
    uint8_t  buttons;       /* mouse: bit0=L,1=R,2=M */
    uint8_t  code;          /* key: ASCII */
    uint8_t  pressed;       /* key: 1=down */
    int16_t  x, y;          /* mouse: absolute */
} gin_event_t;

/* ---- client -> server messages ---- */
enum {
    GMSG_CREATE = 1,        /* hdr + gmsg_create: request a window            */
    GMSG_FRAME  = 2,        /* hdr + gmsg_frame + w*h*4 pixels: window content*/
    GMSG_CLOSE  = 3,        /* hdr only: destroy this window                  */
};
/* ---- server -> client messages ---- */
enum {
    GEVT_CREATED = 100,     /* hdr + gevt_created: window accepted            */
    GEVT_INPUT   = 101,     /* hdr + gevt_input: an input event for this win  */
    GEVT_CLOSE   = 102,     /* hdr only: the WM closed this window            */
};

/* Every message starts with this fixed header; `len` counts the bytes that
   follow the header (the body, including any pixel payload). */
typedef struct gmsg_hdr {
    uint32_t type;
    uint32_t len;
} gmsg_hdr_t;

typedef struct gmsg_create {
    uint32_t w, h;
    char     title[48];
} gmsg_create_t;

typedef struct gmsg_frame {
    uint32_t x, y, w, h;    /* damage rect within the window; pixels follow */
} gmsg_frame_t;

typedef struct gevt_created {
    uint32_t wid;           /* server-assigned window id */
    uint32_t w, h;          /* granted content size */
} gevt_created_t;

/* Input event types delivered to a client (window-local coordinates). */
enum {
    GE_MOUSE_MOVE = 1,
    GE_MOUSE_DOWN = 2,
    GE_MOUSE_UP   = 3,
    GE_KEY        = 4,
    GE_FOCUS      = 5,
    GE_UNFOCUS    = 6,
};
typedef struct gevt_input {
    uint32_t ev;            /* GE_* */
    int32_t  x, y;          /* window-local pixel position (mouse events) */
    uint32_t buttons;       /* mouse button bitmask */
    uint32_t key;           /* ASCII (GE_KEY) */
} gevt_input_t;

#endif
