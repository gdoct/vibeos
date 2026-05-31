# VibeOS graphics & windowing

A small, **strictly layered** GUI stack for the framebuffer. Each layer depends
only on the one below it, so each can be built and tested on its own:

```
  ┌─────────────────────────────────────────────────────────────┐
  │ libwm     desktop + window management                        │  layer 3
  │           stacking / focus / input routing / cursor / repaint│
  ├─────────────────────────────────────────────────────────────┤
  │ libwin    windows + controls                                 │  layer 2
  │           window chrome, widgets, layout, per-window events  │
  ├─────────────────────────────────────────────────────────────┤
  │ libdraw   2D drawing primitives                              │  layer 1
  │           surfaces, pixels, shapes, text, blit, clipping     │
  ├─────────────────────────────────────────────────────────────┤
  │ kernel    framebuffer (fb_device) · USB HID input            │  platform
  └─────────────────────────────────────────────────────────────┘
```

The rule: **a layer never reaches around the one below it.** `libwin` draws only
through `libdraw` surfaces; `libwm` composes only through `libwin` windows and
`libdraw` blits. Nothing above `libdraw` touches the framebuffer directly.

---

## Layer 1 — `libdraw` (primitives)

The only layer that knows what a pixel is. It draws into a **surface** (a width ×
height pixel buffer), never directly to hardware — the screen itself is just the
surface that happens to wrap the framebuffer. This makes off-screen rendering,
double-buffering, and per-window backing stores fall out for free.

Core types:

- `draw_color` — packed RGBA (or the framebuffer's native format via a helper).
- `draw_surface` — `{ uint32_t *pixels; int w, h, stride; }`. Created over an
  allocation, or wrapping the framebuffer.
- `draw_rect` — `{ int x, y, w, h; }`, plus a clip rectangle carried by the
  drawing context so primitives never write outside it.

Operations (all clipped):

- pixels: `draw_pixel`, `draw_hline`, `draw_vline`
- shapes: `draw_fill_rect`, `draw_rect_outline`, `draw_line`, `draw_circle`,
  `draw_fill_circle`
- text: `draw_text` / `draw_char` over the existing bitmap font ([font.c]),
  with a fixed-cell glyph metric; later, proportional fonts
- images: `draw_blit(dst, src, dx, dy)` and `draw_blit_rect` (copy a sub-rect),
  with optional color-key or alpha
- `draw_fill` / `draw_clear` for whole-surface fills

Explicitly **not** here: anything about windows, events, or input. `libdraw` is
pure rendering and could be unit-tested by drawing into a memory surface and
checking pixels.

---

## Layer 2 — `libwin` (windows & controls)

Turns surfaces into **windows** and the things inside them — the *look* and the
*content*, but not the desktop policy. A window owns a `draw_surface` (its
backing store) and a tree of **widgets**; `libwin` knows how to paint window
chrome (title bar, border, close box) and standard controls, run a simple layout,
and deliver events that a window has been handed to the widgets inside it.

Types:

- `win_window` — `{ draw_surface surface; draw_rect frame; char title[]; widget*
  root; int flags; void *user; }`. Knows how to repaint itself into its surface.
- `win_widget` — a rectangle that can paint and handle events:
  `{ draw_rect bounds; void (*paint)(widget*, draw_surface*); int (*event)(widget*,
  const win_event*); widget *next, *children; }`.
- Built-in controls: `label`, `button`, `checkbox`, `textbox`, `panel`
  (container), `listbox`. Each is a `win_widget` with its own paint + event.
- `win_event` — `{ type; x; y; button; key; ... }` (mouse enter/leave/move/
  down/up/click, key down/up, focus in/out). Coordinates are **window-local**.

Responsibilities:

- paint window chrome + dispatch `widget->paint` over the tree into the window
  surface (dirty-rectangle aware so a button hover doesn't repaint everything)
- hit-testing: map a window-local point to the widget under it
- a minimal layout (absolute placement first; a row/column box layout later)
- per-widget event delivery + simple focus *within* a window (tab order)

`libwin` has **no global state** — no notion of which window is on top, where the
mouse is on screen, or the desktop. It is given a window and events and paints.

---

## Layer 3 — `libwm` (desktop & window management)

The policy layer: the one stateful "server" that owns the screen. It holds the
list of windows, decides stacking and focus, owns the cursor and the desktop
background, routes raw input to the right window, and composes everything to the
framebuffer.

State it owns:

- the **screen surface** (wraps `fb_device`) and a back buffer for flicker-free
  compositing
- the **window list in Z-order** (bottom→top), the **focused** window, and the
  window currently being dragged/resized
- the **mouse pointer** — a cursor sprite (an arrow bitmap with a transparent
  color-key, plus its hotspot) drawn at the `usb_mouse_get` position. It is
  **always composited last, on top of every window**, so it is never occluded.
  To move it without smearing, the WM saves the pixels under the cursor before
  drawing it and restores them next frame (or marks the old + new cursor rects
  dirty and re-composites them). Showing a pointer that tracks the mouse is the
  first visible sign of life and a hard requirement of the GUI.
- the desktop background / wallpaper
- a **dirty region** so each frame only re-composites what changed

What it does each frame / event:

1. **input** — pull mouse position + buttons from `usb_mouse_get` (and keyboard
   events; see *Integration*), translate motion/clicks/keys into `win_event`s.
2. **routing** — a click goes to the top-most window under the cursor (raising +
   focusing it); a drag on a title bar moves the window, on a border resizes it;
   keys go to the focused window. Everything else (clicks on the desktop) is the
   WM's own (context menu, launcher).
3. **paint** — ask each dirty window to repaint into its surface (`libwin`), then
   composite back-to-front (desktop bg → windows in Z-order → cursor) into the
   back buffer and blit the dirty region to the screen (`libdraw`).

A thin **desktop shell** lives here too: wallpaper, a taskbar listing open
windows, and a launcher to start apps. The WM is the long-running loop; apps just
create windows and react to events.

---

## Data flow

```
  USB HID (usb_uhci)                 framebuffer (fb_device)
        │  mouse: usb_mouse_get()            ▲
        │  keys:  (input event hook)         │ blit dirty region
        ▼                                    │
   libwm  ──route──►  window  ──dispatch──►  widget
     │                  │                      │
     │ composite        │ paint chrome+tree    │ paint self
     ▼                  ▼ (libwin)             ▼ (libdraw)
   back buffer  ◄───────┴──────────────────────┘
        │
        └──► screen surface ──► framebuffer
```

---

## Integration with the kernel (today)

- **Framebuffer** — `fb_device_t` ([kernel/include/fb.h]) already exposes
  `width`/`height`/`put_pixel`/`fill_rect`. `libdraw`'s screen surface wraps the
  raw framebuffer pointer + stride for direct pixel writes (much faster than
  per-pixel callbacks).
- **Mouse** — the UHCI driver exports `usb_mouse_get(&x, &y, &buttons)`
  ([kernel/src/drivers/usb_uhci.c]); `libwm` polls it (or, later, drains an event
  queue).
- **Keyboard** — today USB keystrokes are injected into the console line
  discipline (`tty_input`). The GUI needs them routed to the **focused window**
  instead, so this needs a small switch: the USB keyboard delivers to either the
  TTY *or* a GUI input queue depending on whether the WM is active. A clean way is
  a kernel `input` event ring that both the TTY and the WM can read, with the WM
  taking the keyboard while it runs.

## Where it runs (phasing)

1. **In-kernel, single window** — bring `libdraw` + a minimal `libwin` + `libwm`
   up inside the kernel (a `gui` worker task) drawing straight to `fb_device` and
   polling `usb_mouse_get`. First milestone: **a mouse pointer rendered on the
   desktop that tracks the mouse** (proves libdraw + the cursor pipeline); then a
   draggable window with a button that reacts to clicks. This proves all three
   layers end-to-end with the least plumbing.
2. **Userspace clients** — once the model is right, expose the framebuffer (mmap
   a `/dev/fb`) and an input device (`/dev/input`), and let userspace apps link
   `libdraw`/`libwin` and talk to the WM over a small protocol (a pipe or shared
   surface). The WM becomes a userspace service; apps are ordinary musl programs.

Start at phase 1 — it is the shortest path to "a window you can drag with the
mouse and type into," and it validates the layering before any IPC exists.
