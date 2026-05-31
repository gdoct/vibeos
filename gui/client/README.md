# gui/client — userspace GUI

The windowing system, moved out of the kernel into userspace. The kernel now only
exposes the framebuffer and input as devices; everything else — compositing,
window management, widgets, apps — is ordinary musl programs.

```
   /dev/fb0 (mmap)   /dev/input (read)        loopback TCP 127.0.0.1:7000
        ▲                  ▲                          ▲
        │                  │                          │
   ┌────┴──────────────────┴────┐   GUI protocol   ┌──┴───────────┐
   │  guiwm  (the WM server)    │◄────────────────►│  client app  │  ← one process
   │  composites desktop+windows│   create/frame/  │ (gmandel,    │    per window
   │  + cursor, routes input    │   input events   │  gclock, …)  │
   └────────────────────────────┘                  └──────────────┘
```

- **libgfx** ([src/libgfx.c](src/libgfx.c), [include/libgfx.h](include/libgfx.h)) —
  the shared drawing library (surfaces + clipped primitives + bitmap font + the
  VibeOS logo), linked by both the server and every client. A userspace port of
  gui/core's libdraw.
- **The protocol** ([include/gui_proto.h](include/gui_proto.h)) — a tiny binary
  message bus over a loopback-TCP stream: a client sends `CREATE` (request a
  window) and `FRAME` (window pixels); the server replies `CREATED` and streams
  back `INPUT` events (mouse/keys, window-local). One window per connection.
- **gui_client** ([src/gui_client.c](src/gui_client.c), [include/gui_client.h](include/gui_client.h)) —
  the client side of the protocol: `gc_open` / `gc_commit` / `gc_poll` / `gc_close`.
  A client is `gc_open(w,h,title)`, draw into a libgfx surface, `gc_commit`, react
  to events. That's it.

## Programs (in user/musl, built with the musl toolchain)

- **guiwm** — the window-manager **server**: mmaps `/dev/fb0`, grabs `/dev/input`,
  listens on `127.0.0.1:7000`, and composites the desktop (wallpaper + logo +
  client windows with title-bar/border/close-box chrome + a taskbar whose
  launcher buttons start apps on click) with a mouse
  pointer that tracks the USB mouse. Title bars drag; the **bottom-right grip
  resizes** (the WM sends the client a `GE_RESIZE` with the new size); the focused
  window gets the keyboard. Started at boot by the service-managed init
  ([config/services/guiwm.yaml](../../config/services/guiwm.yaml)).
- **gmandel** — a demo **client**: renders the Mandelbrot set into its own window
  (WASD pan, +/- zoom; re-renders on resize). Proves process-per-window.
- **gclock** — a live window (uptime counter + a bouncing box), showing the
  compositor multiplexing several independent client processes.
- **gterm** — a terminal: runs `/bin/sh` in a window (line-edited input, output
  scrollback, draggable scrollbar).
- **guiprobe** — a minimal smoke test of the kernel primitives (mmap `/dev/fb0`,
  read `/dev/input`).

A client handles resize by reallocating its surface on `GE_RESIZE` — see
[docs/examples/guihello.c](../../docs/examples/guihello.c).

## Running

`guiwm` autostarts at boot. **Click a launcher button in the taskbar** (bottom of
the screen) to start an app — `MANDELBROT` or `CLOCK`. Or launch one from the
serial shell:

```
vibe$ gmandel        # a Mandelbrot window appears on the desktop
vibe$ gclock         # a live clock window
```

The kernel chooses the userspace WM by default; `gui.mode: kernel` in
`/config/system.conf` falls back to the legacy in-kernel compositor ([gui/core/](../core/)).
