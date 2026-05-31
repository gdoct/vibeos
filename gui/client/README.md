# gui/client — userspace GUI clients (phase 2, scaffold)

Empty by design — this is where the **userspace** side of the windowing system
will live once the GUI IPC protocol exists. Today the whole stack runs in the
kernel as the `guiwm` worker ([../core/](../core/)); phase 2 moves rendering and
input out to per-application processes.

Planned shape (see [../../graphics/graphics.md](../../graphics/graphics.md) phase 2):

- A shared client library (the `gui` namespace) that links `gui/core`'s libdraw +
  libwin so a client draws into its own window surface with the same primitives.
- **Process per window**: each app is its own process; the in-kernel compositor
  becomes a server.
- **Transport**: window surfaces shared via mmap'd `/dev/fb`; input delivered via
  `/dev/input`; control messages (create/destroy/raise/focus, damage, events)
  over a small message bus.
- First demo client: a Mandelbrot renderer drawing into its own window.

Prerequisites still missing in the kernel: mmap of `/dev/fb`, a `/dev/input`
event source, and the message-bus syscalls. Until those land this directory stays
a placeholder so the intended layout is discoverable.
