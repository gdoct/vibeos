#ifndef VIBEOS_INPUT_H
#define VIBEOS_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified input event ring (GUI phase 2). The USB HID driver pushes mouse and
 * keyboard events here when a userspace GUI server has grabbed input (by opening
 * /dev/input); the server drains them with read(). Single-producer (the usbd
 * worker) / single-consumer (the GUI server on the BSP), so the ring is lock-free
 * on x86's strong store/load ordering.
 *
 * The event layout is ABI between the kernel and the userspace GUI lib — keep it
 * fixed (8 bytes). Mirrored in gui/client's headers.
 */

#define INPUT_EV_MOUSE  1   /* absolute x/y + button bitmask */
#define INPUT_EV_KEY    2   /* a key press (code = ASCII, pressed = 1) */

#define INPUT_MOD_CTRL  0x01    /* key event: Ctrl held (carried in `buttons`) */

typedef struct input_event {
    uint8_t  type;          /* INPUT_EV_* */
    uint8_t  buttons;       /* mouse: bit0=L,1=R,2=M; key: INPUT_MOD_* mask */
    uint8_t  code;          /* key: ASCII byte */
    uint8_t  pressed;       /* key: 1 = down */
    int16_t  x, y;          /* mouse: absolute pixel position */
} input_event_t;            /* 8 bytes */

void input_push_mouse(int x, int y, int buttons);
void input_push_key(char c, int mods);

/* Drain up to n bytes of whole events into buf (non-blocking). Returns bytes. */
int  input_read(void *buf, uint32_t n);

/* Grab: once a userspace GUI server opens /dev/input, the HID driver routes
   events here instead of to the console TTY / in-kernel WM. */
int  input_grabbed(void);
void input_set_grab(int on);

#ifdef __cplusplus
}
#endif

#endif
