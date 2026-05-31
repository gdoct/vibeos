# GUI example apps

Minimal, self-contained starting points for writing VibeOS GUI applications
(graphics phase 2). A GUI app is an ordinary `x86_64-linux-musl` program that
links the shared GUI client library and talks to the window manager
([gui/client/](../../gui/client/)) over loopback TCP.

## [guihello.c](guihello.c) — the skeleton

A single window with a clickable button (counts clicks), keyboard handling, and
a clean exit. Read it top to bottom — the whole API surface fits on one screen:

| call | does |
|------|------|
| `gc_open(w, h, title)` | connect to the WM, create a window, return a handle (NULL if the WM isn't up) |
| `gfx_alloc(w, h)` / `gfx_*` | allocate a pixel surface and draw into it (rects, text, blit, …) |
| `gc_commit(c, &surface)` | send the surface to the WM to display |
| `gc_poll(c, &ev)` | non-blocking: `1` = got an event, `0` = none, `-1` = window closed |
| `gc_close(c)` | tear down the window |

Input events (`gevt_input_t`) carry **window-local** coordinates; the WM owns the
cursor and the window chrome (title bar / border / close box), so you only draw
content and react to `GE_MOUSE_DOWN/UP/MOVE`, `GE_KEY`, `GE_FOCUS/UNFOCUS`.

## Build

With the repo's cross toolchain (or host `musl-gcc`):

```sh
./toolchain/x86_64-vibeos-musl-gcc -static -no-pie -O2 -Igui/client/include \
    -o guihello docs/examples/guihello.c \
    gui/client/src/libgfx.c gui/client/src/font8x8_u.c \
    gui/client/src/gui_logo_u.c gui/client/src/gui_client.c
```

The top-level `Makefile` also builds it to `user/build/guihello.elf` as part of
`make user`, and `./build.sh` installs it to `/bin/guihello`.

## Run

`guiwm` autostarts at boot. Then either click the **HELLO** button in the taskbar
launcher, or from the serial shell:

```
vibe$ guihello
```
