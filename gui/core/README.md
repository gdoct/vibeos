# gui/core — kernel-side windowing library

Three strictly-layered libraries, each only depending on the one below
([../../graphics/graphics.md](../../graphics/graphics.md) has the full design):

| layer      | source                       | header                              | does |
|------------|------------------------------|-------------------------------------|------|
| **libdraw**| [src/gui_draw.c](src/gui_draw.c) | [include/gui_draw.h](include/gui_draw.h) | surfaces + clipped primitives (rects, lines, blit, color-keyed blit, alpha blit, bitmap text) |
| **libwin** | [src/gui_win.c](src/gui_win.c)   | [include/gui_win.h](include/gui_win.h)   | windows with chrome (title bar/border) + button/label/textbox widgets + hit-testing |
| **libwm**  | [src/gui_wm.c](src/gui_wm.c)     | [include/gui.h](include/gui.h)           | desktop compositor: back buffer, Z-order, raise/focus, USB-mouse pointer, title-bar drag, click routing, keyboard focus; runs as the `guiwm` worker |

`src/gui_logo.c` is the generated VibeOS logo pixel array
(`tools/genlogo.py <png> src/gui_logo.c <width>`), alpha-blitted onto the desktop.

## Build / run

Compiled into the kernel by the top-level `Makefile` (`GUI_C_SRCS`,
`-Igui/core/include`, objects under `gui/build/`). It runs today because the
framebuffer (`fb.*`) and USB HID input live in the kernel; `gui/client` will host
the userspace counterpart once the GUI IPC + `/dev/fb` mmap path exists.

Entry point: `gui_init()` ([include/gui.h](include/gui.h)), called from
`kernel/src/main.c` after `usb_init()`. No-op when there is no framebuffer.
