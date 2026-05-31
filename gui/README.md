# gui — VibeOS graphical stack

The windowing stack, split the way it will eventually run as a real client/server
system (see [../graphics/graphics.md](../graphics/graphics.md)):

- **[core/](core/)** — the layered drawing + windowing library. Three layers:
  **libdraw** (surfaces + clipped primitives), **libwin** (windows, chrome,
  widgets), **libwm** (desktop compositor, Z-order, focus, the mouse pointer,
  input routing). Today it is compiled **into the kernel** and runs as the
  `guiwm` worker — the framebuffer and input devices live in the kernel, so the
  compositor does too. The split into `core/` is what lets phase 2 lift the same
  code into userspace without a rewrite.
- **[client/](client/)** — the **userspace GUI client** side (phase 2, scaffold).
  Each app will be its own process talking to the compositor over an IPC message
  bus and rendering into a window surface shared via mmap'd `/dev/fb` +
  `/dev/input`. See [client/README.md](client/README.md) for the planned shape.

Why `core` vs `client`: `core` is the shared library + the in-kernel compositor;
`client` is per-application userspace code. The boundary between them is the
future GUI IPC protocol. Keeping them as sibling trees now makes that protocol
the only thing left to introduce, rather than a directory shuffle on top of it.

The logo asset pipeline lives in [core/tools/genlogo.py](core/tools/genlogo.py).
