# Terminal window — a framebuffer vtty as a user process

A plan for a graphical terminal **window** drawn on the framebuffer: a centered,
fixed box (not fullscreen, no window management) that renders text like the
serial console, runs an interactive `/bin/sh` inside it, keeps a virtualized
scrollback buffer, and shows a scrollbar you drive with the arrow keys and
PageUp/PageDown. It is a **user process** (`/bin/term`), started after boot.

This is the project's first pixel-drawing userspace program and its first use of
input that isn't the serial line — so most of the work is the **plumbing that
lets a ring-3 process draw and read keys**, not the terminal logic itself.

> **Status: deferred behind [ROADMAP](ROADMAP.md) §4.** Bootstrap the toolchains
> over the **serial** console first (copy/paste, host logging, scriptable — a
> framebuffer vtty is a worse build console, not a better one). This window is a
> graphics/GUI milestone, not a toolchain enabler, so it's sequenced *after* a
> static `busybox`/`binutils` runs over serial. The one slice worth pulling
> forward is the fd plumbing in **Phase 1** (`pipe`, char-device-backed stdio
> redirection) — §4's shell pipelines want it anyway, so do it on the serial
> path and the terminal inherits it. The fd table + `dup`/`dup2`/`fcntl` it
> builds on already shipped (§4 rung 2).

---

## What it looks like (from the mockup)

A centered window over the boot framebuffer (the teal kernel banner stays at the
very top, behind it):

- **Title bar** — a dark charcoal bar with a single rounded **tab**
  (`guido@PC-GUIDO: ~`) carrying a small `×`, and a window-level `×` at the far
  right. Decorative only — there is no window management, no mouse; the `×`
  glyphs are drawn but not clickable.
- **Body** — a light/cream background with dark text, an 8×8 monospaced grid.
  Content is live shell output: in the mockup, multi-column `ls --color` output
  where some names are **red** (archives: `*.tar.gz`, `*.zip`, `id.tgz`), some
  **gray/dim**, some **bold black** — i.e. the vtty must interpret **ANSI SGR
  color escapes**. A **solid block cursor** sits at the `guido@PC-GUIDO:~$`
  prompt.
- **Scrollbar** — a track down the right inside edge with a darker **thumb**
  whose size is `visible_rows / total_lines` and whose position tracks the
  top visible line. At the live tail the thumb sits at the bottom; scrolling
  back moves it up.

Layout is derived at runtime from the framebuffer geometry (see
[§ Window geometry](#window-geometry)); nothing is hard-coded to a resolution.

---

## Decisions locked in

- **Keyboard: a real PS/2 driver.** Keystrokes typed into the QEMU graphical
  window arrive at the i8042/PS/2 controller (IRQ1), *not* the serial UART that
  backs today's [tty.c](kernel/src/tty.c). The framebuffer window lives in that
  graphical display, so it needs PS/2 input. Exposed to userspace as a key-event
  char device.
- **Drawing: `mmap` of `/dev/fb0`, Linux-fbdev style.** The process maps the
  linear framebuffer, queries geometry with an `ioctl`, and draws every pixel,
  glyph, and the window chrome itself (carrying its **own** 8×8 font — the
  kernel [font8x8](kernel/src/drivers/font.c) is uppercase-only; the mockup is
  full lowercase + symbols). Rendering stays in userspace.
- **Behaviour: a live vtty with scrollback.** A child `/bin/sh` runs *inside*
  the window through a pty; its output accumulates into a virtualized scrollback
  ring; typing goes to the shell; arrows/PgUp/PgDn scroll the history without
  disturbing the shell.

---

## Architecture

```
  /bin/term (ring 3, user process)
    ├─ open("/dev/fb0")  + ioctl(geometry) + mmap  → draws chrome, text, scrollbar
    ├─ open("/dev/kbd")  → reads key events (incl. arrows / PgUp / PgDn)
    └─ openpty() → fork() → child: dup2 slave→0/1/2, execve("/bin/sh")
                          parent (term): read master → vte parse → scrollback
                                         key events → (scroll) or → write master
```

Three kernel device files (`/dev/fb0`, `/dev/kbd`, the pty pair) all ride one new
abstraction: a **char-device file** behind the existing fd table. That shared
prerequisite is Phase 1; everything else builds on it.

Concurrency note: the kernel only touches the framebuffer once, for the boot
banner ([main.c:75-83,267](kernel/src/main.c#L75)); `kprintf` goes to serial. So
once `/bin/term` owns the framebuffer there is no kernel/user draw contention to
arbitrate — the process simply overdraws the banner area it uses.

---

## Implementation phases

Phases 1–5 are independently shippable and give an on-screen result early (a
window with scrollable seeded text) *before* the heavier pty work in Phase 6.

### Phase 1 — Char-device files + fd routing (kernel prerequisite)

**Already shipped (§4 rung 2):** the per-process fd table, refcounted `file_t`
objects, `dup`/`dup2`/`fcntl`, `fork` dup'ing the table, and `execve` keeping it.
**What's still missing** is char devices and redirectable stdio — the fd table
holds only VibeFS files/dirs (`FD_FILE`/`FD_DIR`), and `read`/`write` on 0/1/2 are
hardwired to serial. Generalize so a descriptor can name a character device, and
so a child can point its stdio at one.

- Extend [file.h](kernel/include/file.h): add `FD_CHAR`, and a `cdev_t *cdev`
  (or a `void *priv` + ops) to `file_t`. Define `cdev_t` with
  `read/write/ioctl/mmap/close` function pointers.
- Route in [syscall.c](kernel/src/syscall.c): `sys_read`/`sys_write` first
  consult `fdt[fd]`; an `FD_CHAR` entry dispatches to its `cdev` ops. **Only**
  fall back to the implicit serial console for 0/1/2 when the slot is empty — so
  an installed `file_t` on 0/1/2 (a dup'd pty slave) overrides serial.
- Make 0/1/2 redirectable: lift `sys_dup2`'s `newfd ≤ 2` guard
  ([syscall.c:614](kernel/src/syscall.c#L614)) so a slave fd can be `dup2`'d onto
  0/1/2. (`dup`/`dup2`/`fcntl` themselves already exist — this is the one-line
  restriction that blocks redirecting a child's stdio.)
- Add `ioctl` routing: today nr 16 returns `-ENOTTY` unconditionally
  ([syscall.c:669](kernel/src/syscall.c#L669)); route to the `cdev->ioctl` when
  the fd is a char device, keep `-ENOTTY` otherwise.
- Add `pipe`/`pipe2` while here — needed for shell pipelines on the §4 path, and
  the simplest backing for the pty's two ring buffers in Phase 6.

*Verify:* a throwaway char device (e.g. `/dev/null`/`/dev/zero`) opened, read,
written, and `dup2`'d over fd 0 *and* fd 1 behaves correctly; existing serial
console path unchanged when 0/1/2 are empty; `mhello`/`ftest`/serial `sh` still
run.

### Phase 2 — `/dev/fb0`: framebuffer device + geometry ioctl + mmap

Give userspace the framebuffer.

- A char device wrapping [fb.c](kernel/src/drivers/fb.c)/`g_fb`. `open("/dev/fb0")`
  yields an `FD_CHAR`.
- `ioctl(FBIOGET_GEOMETRY)` → `{ width, height, pitch, bpp, format }` (a small
  VibeOS struct; no need to mimic Linux `fb_var_screeninfo` exactly).
- `mmap` support for a **device-backed** mapping: today `sys_mmap` only honors
  `MAP_ANONYMOUS` ([syscall.c:375](kernel/src/syscall.c#L375)). Add the file path:
  for an `FD_CHAR` with an `mmap` op, map the framebuffer's physical pages
  (`g_fb.base` is already a direct-map kernel VA; translate via `kva_to_phys`)
  into the process at the chosen VA with `PTE_P|PTE_U|PTE_W` (write-combining is
  a later optimization). Map the whole `pitch*height` span.

*Verify:* a tiny test program maps `/dev/fb0` and fills it with a color / draws a
rectangle visible in the QEMU window.

### Phase 3 — PS/2 keyboard driver + `/dev/kbd`

First non-serial input source.

- [ps2kbd.c](kernel/src/drivers/ps2kbd.c) + header: init the i8042 controller,
  register an IRQ1 handler (the `IRQ_STUB 1` vector already exists in
  [isr.S:123](kernel/src/isr.S#L123); wire it through `irq_register` like the
  timer/virtio paths, EOI before handler per the project rule). Read scancodes
  from port `0x60`.
- Decode **scancode set 1** → a key-event stream: printable ASCII (with Shift),
  plus distinct codes for **Up/Down/Left/Right, PageUp/PageDown, Home/End,
  Enter, Backspace** and modifier state. Define a compact `kbd_event` (e.g.
  `{ u16 keysym; u8 mods; u8 pressed }`); arrows/page keys get keysyms above
  0x100 so they don't collide with ASCII.
- `/dev/kbd` char device: a ring buffer of events filled in the IRQ handler,
  drained by blocking `read` (wake a wait queue, same pattern as
  [tty.c](kernel/src/tty.c)).

*Verify:* test program opens `/dev/kbd`, prints event codes for letters, arrows,
and PageUp/Down to serial.

### Phase 4 — `/bin/term` skeleton: chrome + font

The user program and its renderer; no shell yet.

- [user/term.c](user/term.c) + an embedded **full 8×8 ASCII font** (lowercase,
  digits, punctuation — the mockup needs them) in a `user/font8x8.h` blob.
- Map `/dev/fb0`, compute window rect (see [geometry](#window-geometry)), draw
  the chrome once: charcoal **title bar** with the tab (`HOSTNAME:~` text + a
  drawn `×`) and the window-level `×`, a 1px border, the cream **body**, and an
  empty **scrollbar track** on the right.
- Glyph + filled-rect primitives in userspace writing into the mmap'd buffer.

*Verify:* booting and launching `/bin/term` by hand shows the empty centered
window over the framebuffer, matching the mockup's frame.

### Phase 5 — Virtualized scrollback + text rendering + scroll keys

Make it a scrollable text view, driven (for now) by seeded text or piped serial
output — no pty yet.

- **Scrollback model:** a ring of text lines (e.g. a few thousand lines × N
  cols), each cell holding a char + an attribute byte (fg/bg color). A
  `top_line` view cursor and a `follow_tail` flag.
- **Render:** draw `visible_rows` lines from `top_line` into the body grid; clip
  to the body rect; repaint only on change (dirty flag) to keep it cheap.
- **Scrollbar thumb:** `thumb_h = track_h * visible_rows / total_lines`,
  `thumb_y = track_h * top_line / total_lines`; redraw on scroll.
- **Input → scroll:** read `/dev/kbd`; Up/Down move `top_line` by 1, PgUp/PgDn by
  `visible_rows-1`, Home/End jump to top/tail. Any scroll-back clears
  `follow_tail`; reaching the end re-arms it (so new output auto-scrolls).
- Cursor: draw a solid block at the active cell when viewing the tail.

*Verify:* seed the buffer with a few hundred lines; arrows and PgUp/PgDn scroll,
the thumb tracks position, Home/End jump.

### Phase 6 — pty + run `/bin/sh` inside the window

Wire a live shell. This is the heaviest phase.

- **pty device** [pty.c](kernel/src/pty.c): a master/slave pair of char devices
  with two ring buffers and a **canonical line discipline** mirroring
  [tty.c](kernel/src/tty.c) (echo, backspace, line commit on Enter) so the
  existing `/bin/sh` — which relies on cooked-mode `read(0)` — works unchanged.
  Writes to the master appear as slave input (and are echoed to the master for
  display); slave writes (program output) appear as master reads.
- An `openpty()` entry point (a syscall or an `ioctl`/`open("/dev/ptmx")`); pick
  the simplest — a dedicated syscall returning `{master_fd, slave_fd}` avoids a
  `/dev` node-allocation scheme.
- **Spawn:** `/bin/term` calls `openpty`, `fork`s; the child `dup2`s the slave
  onto 0/1/2 (Phase 1 made fork copy the fdt and let 0/1/2 hold real files),
  closes the master, and `execve("/bin/sh")`. The parent closes the slave and
  loops: drain `/dev/kbd` → printable keys + Enter/Backspace **write to the
  master**, arrows/page keys **scroll locally**; drain the master → feed the vte
  parser → append to scrollback → repaint.
- Make the loop non-blocking or multiplexed so keyboard and shell output are both
  serviced (no `poll` yet — a simple alternating non-blocking read on both fds is
  enough; widen to `poll`/`select` only if it's needed).

*Verify:* the window shows the `vibe$`/sh prompt, typing runs commands, output
fills the buffer, and PgUp/PgDn scrolls back through it while the shell keeps
running.

### Phase 7 — ANSI/SGR colors + final polish

Make `ls --color` look like the mockup.

- **vte parser:** handle `\n`, `\r`, `\b`, tab, and CSI **SGR** (`ESC [ … m`) for
  the 8/16 ANSI colors + bold/dim/reset, writing the color into each cell's
  attribute byte. Ignore (consume) cursor-movement CSIs safely for now.
- Map ANSI palette → framebuffer RGB via `fb_rgb`-style packing in userspace.
- **Launch after boot:** have [init.c](user/init.c) `fork` a child that execs
  `/bin/term` (which spawns its own sh), while the parent continues to the serial
  `/bin/sh` — two independent consoles. User tasks are BSP-pinned today
  (per [ROADMAP](ROADMAP.md)); that's fine.
- **Build:** add `/bin/term` to [build.sh](build.sh) and the [Makefile](Makefile)
  (compile + import onto the VibeFS disk alongside `/bin/sh`).

*Verify:* clean `./build.sh && make run` boots straight into the windowed
terminal with colored `ls` output, scrollback, and a working prompt.

---

## Window geometry

All derived from the `/dev/fb0` geometry ioctl (`W`,`H`), font 8×8:

```
  win_w   = round(W * 0.72)         (centered: win_x = (W - win_w)/2)
  win_h   = round(H * 0.66)         (         win_y = (H - win_h)/2)
  title_h = 18px                    (charcoal bar + tab)
  pad     = 6px                     (inner margin around the body text)
  sbar_w  = 10px                    (scrollbar track on the right)
  cols    = (win_w - 2*pad - sbar_w) / 8
  rows    = (win_h - title_h - 2*pad) / 8
```

Fractions/sizes are starting points to tune against the mockup, not contract.

---

## Deferred / out of scope (matches "extremely basic")

- **No window management** — no move/resize/focus/z-order, single window, the
  `×` glyphs are cosmetic. (A keypress like the shell exiting can tear it down.)
- **No mouse** — there is no PS/2 mouse driver; the scrollbar is keyboard-driven
  only.
- **No `poll`/`select`** unless the alternating non-blocking loop proves
  insufficient.
- **No reflow on (nonexistent) resize**, no Unicode (8-bit/ASCII font only), no
  alternate screen buffer, no full terminfo — just enough CSI to render
  `ls --color` and a prompt.
- **Write-combining / dirty-rect batching** for the framebuffer — start with a
  plain mapping and full-line repaints; optimize only if it's visibly slow.

## Risks / things to watch

- **PS/2 may need explicit enable.** Under OVMF/QEMU the i8042 is usually live,
  but confirm the controller and IRQ1 are unmasked through the I/O APIC path
  ([apic.c](kernel/src/apic.c)) the way other IRQs are routed.
- **fbdev mmap span.** Map `pitch*height`, not `width*4*height`, and honor the
  `BGR8`/`RGB8` `format` when packing colors in userspace.
- **0/1/2 override correctness.** The Phase 1 routing change is load-bearing and
  touches every existing program; keep the empty-slot serial fallback so the
  serial shell and `mhello`/`ftest` keep working.
- **pty line discipline parity.** The shell depends on cooked-mode echo/edit;
  the pty must reproduce [tty.c](kernel/src/tty.c)'s behaviour or `/bin/sh` will
  feel broken inside the window.
