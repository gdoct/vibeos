# VibeOS — what we have, what's next

A from-scratch x86-64 kernel: boots over UEFI into the higher half with its own
page tables; preemptive SMP scheduler with per-CPU run queues + work-stealing;
copy-on-write, ASLR'd address spaces; POSIX signals **and threads** (`clone` /
`futex`, so unmodified musl **pthreads** run); a writable, crash-safe filesystem
(VibeFS, with symlinks); a **WAN-grade TCP/IP** stack over virtio-net; a
ChaCha20 **CSPRNG**; and a **`/config`-driven, service-managed init**. It runs
**real static *and* dynamically-linked `x86_64-linux-musl` binaries** over a
serial shell, and ships its own **`x86_64-vibeos-musl` cross toolchain**. Every
feature boots end-to-end on QEMU q35 + OVMF and is verified by serial output.

Detail lives in the code and git history; this file is the map, not the manual.

---

## What works today

- **Boot** — UEFI bootloader ([boot/](boot/)) loads `kernel.elf`; higher-half
  kernel at `-2 GiB` ([linker.ld](kernel/linker.ld), [start.S](kernel/src/start.S)).
  No identity map after boot — the low half is entirely userspace; the kernel
  reaches RAM via a `PHYS_OFFSET` direct map.
- **CPU/interrupts** — GDT/TSS, IDT (32 exc + 16 IRQ), APIC (LAPIC + I/O APIC,
  per-CPU 100 Hz timers) ([apic.c](kernel/src/apic.c), [idt.c](kernel/src/idt.c)).
- **SMP** — AP bringup via INIT-SIPI-SIPI ([smp.c](kernel/src/smp.c)); xv6-style
  scheduler with one `sched_lock` baton held across switches
  ([task.c](kernel/src/task.c)). Per-CPU TSS + GS base with `swapgs` on
  syscall/IRQ entry ([percpu.c](kernel/src/percpu.c),
  [usermode.S](kernel/src/usermode.S), [isr.S](kernel/src/isr.S)); APs enable
  SYSCALL + SSE. Cross-CPU IPIs + TLB shootdown ([apic.c](kernel/src/apic.c)).
  **Per-CPU run queues with work-stealing** (ROADMAP §3): each CPU owns a FIFO of
  ready tasks (home-CPU affinity), and an idle core steals a *kernel* task from a
  busy core's queue — O(1) pick + a much shorter critical section than the old
  global ready-scan. **Kernel tasks run on every core; user tasks are pinned to
  the BSP** — the ring-3 syscall / tty-line / pipe / fd-table paths are
  deliberately lock-free on that assumption, so user tasks are never stolen.
- **Memory** — own 4-level paging + direct map + guarded kstacks
  ([paging.c](kernel/src/paging.c)); bump+freelist PMM; slab/large kmalloc.
  **Copy-on-write fork** with per-page refcounts; **validated `copy_to/from_user`**
  (a bad user pointer is `-EFAULT`, never a kernel fault); guarded user stacks.
- **Processes / signals** — per-process address spaces; `fork`/`execve`/`wait4`;
  wait queues, blocking sleep, timer preemption. **POSIX signals**
  ([signal.c](kernel/src/signal.c)): handlers on a Linux-compatible `rt_sigframe`,
  blocked/pending masks, default actions (term/core/ignore/stop/cont),
  `kill`/`tgkill`/`sigaltstack`/`rt_sigreturn`; CPU faults become
  `SIGSEGV`/`SIGILL`/… (a buggy process dies, the kernel lives).
- **Devices/FS** — IRQ-driven virtio-blk with scatter-gather DMA; PCI;
  framebuffer; serial TTY backing `read(0)`. **VibeFS** ([fs.c](kernel/src/fs.c)) —
  writable, crash-safe ordered writes + `fsck`, 4 KiB blocks, triple-indirect +
  64-bit size.
- **GUI — userspace windowing** (graphics/graphics.md phase 2, [gui/client/](gui/client/)) —
  the window manager runs **in userspace** as an ordinary musl service. The kernel
  exposes the framebuffer and input as devices — **`/dev/fb0`** (a device `mmap`
  maps the scanout's physical pages straight into a process) and **`/dev/input`**
  (a kernel event ring fed by the USB HID driver, grabbed on open) — and the
  **`/bin/guiwm`** server ([guiwm.c](user/musl/guiwm.c)) mmaps the framebuffer,
  reads input, and composites the desktop: wallpaper + the alpha-blended VibeOS
  logo + client windows (title-bar/border/close-box chrome, Z-order, title-bar
  drag, focus) + a **taskbar with a launcher** (click an app button to start it) +
  a mouse pointer tracking the USB mouse. **Clients are
  separate processes — one per window** — that link the shared **libgfx**
  ([gui/client/src/libgfx.c](gui/client/src/libgfx.c)) and talk to the server over
  a small **loopback-TCP message bus** ([gui_proto.h](gui/client/include/gui_proto.h)):
  `/bin/gmandel` renders the Mandelbrot set in its own window, `/bin/gclock` is a
  live animated window. (The launcher spawns apps via a helper process forked
  before the framebuffer is mapped — the compositor never `fork`s with a device
  mapping live.) Started at boot by the service-managed init
  ([guiwm.yaml](config/services/guiwm.yaml)); `gui.mode: kernel` selects the legacy
  in-kernel compositor instead. Verified on QEMU: `screendump`s show the desktop,
  the logo, the cursor, and client-rendered window content (a Mandelbrot fractal,
  a ticking clock); serial confirms the server↔client protocol.
- **GUI — in-kernel compositor** (graphics/graphics.md, phase 1) — a strictly-layered framebuffer
  windowing stack under [gui/core/](gui/core/) (now the `gui.mode: kernel`
  alternative to the userspace WM): **libdraw**
  ([gui_draw.c](gui/core/src/gui_draw.c)) — surfaces +
  clipped primitives (rects, lines, blit, color-keyed blit, bitmap text);
  **libwin** ([gui_win.c](gui/core/src/gui_win.c)) — windows with chrome (title
  bar/border) + button/label/**text-field** widgets + hit-testing; **libwm**
  ([gui_wm.c](gui/core/src/gui_wm.c)) — a desktop compositor (back buffer, window
  Z-order, raise/focus, **a mouse pointer that tracks the USB mouse**, title-bar
  drag, click routing, **keyboard focus**) in a `guiwm` worker. The desktop shows
  the **alpha-blended VibeOS logo** (`draw_blit_alpha` over a 0xAARRGGBB asset,
  [gui_logo.c](gui/core/src/gui_logo.c) generated by
  [genlogo.py](gui/core/tools/genlogo.py)),
  and a demo window (draggable, with a click-counting button and an editable text
  field). **Keystrokes route to the focused text field** — the USB keyboard feeds
  the GUI when a field is focused (`gui_wants_keyboard`/`gui_input_key`) and the
  console TTY otherwise. Verified on QEMU: a `screendump` confirms the logo +
  cursor + window pixels, and self-tests confirm drag, click, typing-into-field,
  and logo compositing.
- **USB input** (ROADMAP) — a **UHCI** host-controller driver
  ([usb_uhci.c](kernel/src/drivers/usb_uhci.c)): frame-list schedule of queue
  heads + transfer descriptors in DMA memory, device enumeration over endpoint 0
  (control transfers: `GET_DESCRIPTOR` / `SET_ADDRESS` / `SET_CONFIGURATION` /
  HID `SET_PROTOCOL`(boot)/`SET_IDLE`), and per-frame polling of the HID
  interrupt-IN endpoints (no IRQ). A **USB keyboard** feeds the console
  (`tty_input`, US keymap with shift — or the focused GUI text field when one is
  up) and a **USB mouse** drives the GUI pointer (`usb_mouse_get`). Both are
  enumerated and driven over `piix3-usb-uhci` in QEMU.
- **Networking** — virtio-net driver ([virtio_net.c](kernel/src/drivers/virtio_net.c))
  sharing PCI INTx with virtio-blk; a compact IPv4 stack ([net.c](kernel/src/net.c)):
  ARP, IPv4, ICMP (`ping`), UDP, and a **WAN-grade TCP** — 3-way handshake, a
  send buffer with go-back-N retransmission off an RTO derived from Jacobson/
  Karels RTT estimation (Karn on retransmits), congestion control (slow start +
  congestion avoidance + fast retransmit on triple dup ACKs), out-of-order
  reassembly, delayed ACKs, MSS-option negotiation, a zero-window persist probe,
  and a 2*MSL TIME-WAIT — all driven by a worker-context timer. Plus a loopback
  netif. **BSD sockets** on the fd table —
  `socket`/`bind`/`listen`/`accept`/`connect`/`sendto`/`recvfrom`/`poll`, with
  **`O_NONBLOCK`/`MSG_DONTWAIT`** recv (so a single-threaded server can multiplex).
  Static SLIRP addressing (10.0.2.15/24). Ported `/bin/wget` fetches over the
  stack; a loopback self-test pushes 6 KB multi-segment, and the userspace GUI
  pushes **hundreds of KB** of window pixels per frame over loopback TCP — which
  flushed out a real bug: a receiver draining a full `RCVBUF` now emits a
  **window-update ACK** so large transfers don't stall.
- **Userspace / Linux ABI** — ring 3 via SYSCALL/SYSRET; ELF loader with real
  auxv + argv/envp ([elf64.c](kernel/src/elf64.c)). **Runs unmodified static and
  dynamically-linked musl binaries** — `ET_DYN`/PIE + `PT_INTERP` load
  `ld-musl.so`, file-backed `mmap`, `mprotect` (`/bin/dynhello`, `/bin/mhello`,
  `/bin/ftest`, `/bin/pipetest`, `/bin/sigtest`, `/bin/nettest`). Syscalls: TLS
  (`arch_prctl`), anon + file `mmap`/`munmap`/`mprotect`, `read`/`write`/`writev`,
  `brk`, `nanosleep`, signals, sockets; **file I/O over a per-process fd table** —
  `open`/`openat`/`close`/`lseek`/`stat`/`lstat`/`statx`/`fstat`/`getdents64`/
  `getcwd`/`chdir`/`access`/`faccessat`/`fcntl`/`dup`/`dup2`/`mkdir`/`symlink`/
  `readlink` ([file.c](kernel/src/file.c)); **`pipe`/`pipe2`**
  ([pipe.c](kernel/src/pipe.c)); `fork`/`execve`/`wait4`/`exit`;
  `uname`/`clock_gettime`/`getrandom`/`getsockname`. **ASLR** (ROADMAP) — a
  per-execve CSPRNG slide on the PIE image, dynamic linker, stack, and mmap
  arena.
- **Threads** (ROADMAP) — `clone(CLONE_VM|CLONE_FILES|CLONE_THREAD|CLONE_SETTLS
  |CLONE_*_TID)` over a refcounted address space + descriptor table, `futex`
  (WAIT/WAKE), `gettid`/`tgid`, and `CHILD_CLEARTID` for `pthread_join`. Runs
  unmodified **musl pthreads** ([user/musl/threadtest.c](user/musl/threadtest.c):
  4 threads × 20 000 mutex-guarded increments join to an exact total).
- **Userspace quality-of-life** (ROADMAP §4) — a per-process **cwd** (`chdir`/
  `getcwd`, `./`–`../`–normalizing path resolution); **`FD_CLOEXEC`** tracked
  per-descriptor and enforced by `execve`; **symlinks** in VibeFS (new
  `FT_SYMLINK` inode, followed during path resolution, fsck-safe);
  synthetic **`/dev`** (`null`/`zero`/`full`/`random`/`urandom`/`tty`, plus
  `fb0`/`input` for the userspace GUI) and a
  tiny **`/proc`** (a dir per live task + `self`, each with a `stat` file)
  ([synth.c](kernel/src/synth.c)); a real **init** (PID 1) that forks + respawns
  the shell; and a **package tool** ([user/musl/pkg.c](user/musl/pkg.c)) that
  extracts/lists POSIX ustar tarballs (files, dirs, symlinks). Shell builtins
  `cd`/`pwd`/`echo`/`cat`/`ls`/`ln -s`/`readlink` exercise it over serial.
- **Randomness** — a **ChaCha20 DRBG** ([csprng.c](kernel/src/csprng.c)) seeded
  from RDRAND, RDTSC timing jitter, and **virtio-rng** hardware entropy
  ([virtio_rng.c](kernel/src/drivers/virtio_rng.c)), with per-request rekey for
  forward secrecy. It backs RFC 6528 TCP ISNs (per-4-tuple keyed hash + a
  monotonic term) and `AT_RANDOM` (musl's stack canary). A non-crypto
  xorshift/RDRAND RNG ([random.c](kernel/src/random.c)) remains for plumbing
  variety.
- **Configuration + service-managed init** (ROADMAP) — a kernel **config
  service** ([config.c](kernel/src/config.c)) reads **`/config/system.conf`** at
  boot into a key/value store (a small YAML-ish `key: value` format with `#`
  comments and dotted keys) and lets subsystems query it — `uname`'s hostname and
  the boot motd come from `/config`. `config_reload()` re-reads it live; the
  `sysconfig(1000)` syscall + **`/bin/sysconf`** tool
  ([user/musl/sysconf.c](user/musl/sysconf.c)) `list`/`get`/`set`/`reload` it (a
  `set` rewrites the file and reloads, so e.g. the hostname changes without a
  reboot). PID 1 is a **service-managed init** ([user/musl/sinit.c](user/musl/sinit.c)):
  it reads one discoverable YAML file per service from **`/config/services/`**
  (`exec`/`respawn`/`oneshot`/`enabled`/`after:` deps/`log:` path), starts them in
  dependency order, runs oneshots to completion, and supervises the rest —
  **restart back-off** with a rapid-failure limit (gives up after repeated fast
  crashes), **per-service file logging** (redirects a service's stdout/stderr to
  its `log:` file), and a live snapshot at `/config/services.state` (viewable via
  `sysconf services`). Not SysV (no runlevels / rc scripts) and not systemd (no
  unit sections / dbus) — just files you can `ls` and `cat`. (Fds 0/1/2 are now
  redirectable — console by default, but the fd table is consulted first — which
  is what lets init point a service at a log file.)
- **Shell + tooling** — `/bin/init` (the service manager) brings up `/bin/sh`
  over serial; host `disktool-cli`
  ([interop/tools/diskutil](interop/tools/diskutil/)) builds/populates VibeFS
  images; `./build.sh` + `make run` (`-smp 4`, virtio-blk + virtio-net). A
  **cross toolchain** ([toolchain/](toolchain/)): `x86_64-vibeos-musl-gcc` + a
  generated VibeOS sysroot compiles binaries for the target directly
  (`make sysroot`; `/bin/vibehello` is built with it).

**ABI simplifications still open:** dirs open read-only; no hard links / rename;
no file permissions (mode bits are cosmetic); `fchdir` unsupported (no
inode→path map). TCP's retransmit / reassembly
paths are implemented but only structurally exercised — loopback/SLIRP is
lossless and in-order, so loss-recovery awaits verification against a real host
once outbound connectivity is available.

---

## What's next

The original ordered backlog — (1) memory correctness + safety, (2) user tasks
on all cores, (3) signals, (4) dynamic linking, (5) networking, then (6)
WAN-grade TCP, (7) a ChaCha20 CSPRNG, (8) per-CPU run queues with work-stealing,
(9) userspace quality-of-life (`chdir`/cwd, `FD_CLOEXEC`, symlinks, `/dev` +
`/proc`, a respawning init, a tar package tool), and (10) a cross toolchain
(`x86_64-vibeos-musl-gcc` + sysroot) — is **fully shipped and serial-verified**.
It all lives in "What works today" now. The ABI was widened alongside (`uname`,
`clock_gettime`, `getrandom`, real `getsockname`/`getpeername`/`getsockopt`,
`mkdir`, `lstat`, `symlink`/`readlink`), exercised by the cross-built
`/bin/abitest`.

The "small ABI gaps" are now closed too: **ASLR**, **threads** (`clone`/`futex`/
pthreads), and the `statx` / `access` / fcntl-lock variants all shipped above.
What remains:

- **Remaining ABI polish**, as opportunity allows: tear down sibling threads on
  `exec`/`exit_group` (today join-before-exit is assumed); robust-futex
  ownership; `clock_nanosleep`/`ppoll`; signals targeted at a specific thread.
- **Init polish.** The config store + service-managed init (with restart back-off,
  per-service file logging, and a `sysconf services` view) are shipped. What's
  left of this theme is socket-/timer-activated services and shell I/O redirection
  (`>`/`<`/`2>` — the kernel now supports it; the shell doesn't parse it yet).
- **Graphical stack — phase 2 shipped.** The client/server split is done: rendering
  and input moved to userspace over mmap'd `/dev/fb0` + `/dev/input`, a `guiwm`
  **server** composites, **clients are one process per window** over a loopback-TCP
  message bus, a Mandelbrot demo client renders in its own window, and a **taskbar
  launcher** starts apps with a click (see "What works today"). What's left to
  *mature* it: more widgets (menus/scrollbars/list boxes — clients currently push
  raw pixel surfaces), window **resize**, and shared-memory window buffers (frames
  stream over TCP for now — fine for a demo, a copy per frame at scale). A future
  cleanup: collapse `gui/core`'s libdraw and `gui/client`'s libgfx into one source
  built for both kernel and userspace.
- **USB follow-on for GUI.** Optional xHCI/EHCI + USB hub support (UHCI already
  drives the HID devices).
- **Audio.** An audio subsystem + virtio-sound.
- **User tasks on all cores.** Today user tasks are pinned to the BSP for simplicity
  (the ring-3 syscall / tty-line / pipe / fd-table paths are deliberately lock-free
  on that assumption, so user tasks are never stolen). The scheduler is fully
  SMP-aware and kernel tasks run on every core; the remaining work is to make
  user tasks stealable too, which will require some locking in the syscall/tty/pipe
  paths. This will unlock real multi-threaded workloads and let the net stack run
  on a different core from the shell.

*Known issue:* unclean-`fsck` drops `/bin/init` on diskutil volumes (workaround =
clean image per build); root-cause before relying on crash persistence. Other
backlog: ACPI poweroff; FS journaling / `rename` / permissions.

---

## How it boots

```
UEFI/OVMF → BOOTX64.EFI → kernel.elf → start.S (bootstrap tables, jump high) → kmain()
  → gdt/idt/percpu(TSS+GS)/syscall → pmm → paging (drop identity) → page refcounts
  → csprng → apic+timer → virtio-blk → virtio-rng (seed CSPRNG) → fs_mount
  → config (/config/system.conf) → irq_enable → sched_init → net_init
  (virtio-net + worker + tcp timer) → usb_init (UHCI + HID worker)
  → create init+workers → smp_init (APs join:
  percpu+SYSCALL+SSE) → scheduler() [every CPU]
  → /bin/init (service manager) → /config/services/* → /bin/guiwm (userspace WM)
  + /bin/sh
```

---

## Design choices worth remembering

- **C++17 frontend, C-style code** — `g++`, `extern "C"` across asm/TU
  boundaries; no RTTI/exceptions/STL.
- **Higher-half, no identity map** — physical access via `PHYS_OFFSET`
  (`phys_to_virt`); device addresses via `kva_to_phys`. Low half is userspace.
- **Linux syscall numbering from day one** — cross-compiled musl runs without a
  translation layer.
- **`sched_lock` held across context switches** (xv6 baton); interrupt state is
  per-CPU (`push_off`/`pop_off`), never in the lock. A device whose completion
  IRQ can land on another core must check-and-sleep *and* wake under this lock
  (the `*_locked` wait-queue ops) — local `irq_save` no longer excludes a remote
  waker once user tasks run everywhere.
- **Per-CPU GS + `swapgs`** — each CPU's kernel stack / TSS live in its `struct
  percpu`; user GS base is unused (musl TLS is FS). The invariant: kernel code
  always runs with GS base = percpu, restored by `swapgs` on every entry.
- **Net stack runs in a worker, not IRQ context** — the rx IRQ only queues
  frames; ARP/IP/TCP run in a kernel task so they can block and transmit.
  Loopback sends from inside the worker are deferred to a private queue to avoid
  re-entering `sched_lock`.
- **Legacy virtio + shared-vector INTx + APIC** — simplicity over MSI-X / ACPI
  `_PRT` / AML; the shared PCI vector chains handlers, virtio devices are told
  apart by subsystem id. Revisit only if a device demands it.
- **EOI before the handler** — a handler that context-switches must not strand
  the line.
- **Fixed-size task/file/stack/pcb/socket pools** — cheap, obvious failure
  (panic). Grow when there's a real reason (e.g. `MAX_TASKS` for the net daemons).
