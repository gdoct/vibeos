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
- **[client/](client/)** — the **userspace GUI** (phase 2, shipped). A userspace
  window-manager **server** (`guiwm`) mmaps `/dev/fb0`, grabs `/dev/input`, and
  composites the desktop; **client** apps (`gmandel`, `gclock`) are ordinary musl
  programs, one process per window, talking to the server over a loopback-TCP
  message bus. Rendering and input now live in userspace; the kernel just exposes
  the framebuffer + input as devices. See [client/README.md](client/README.md).

By default the kernel runs the **userspace** WM (`gui/client`) and `gui/core` is
the legacy in-kernel compositor, selectable with `gui.mode: kernel` in
`/config/system.conf`. `core` and `client` share the same layered design (and a
near-identical libdraw/libgfx); the split is the client/server boundary the GUI
protocol now crosses.

The logo asset pipeline lives in [core/tools/genlogo.py](core/tools/genlogo.py).
