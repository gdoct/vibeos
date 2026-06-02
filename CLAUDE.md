# CLAUDE.md

Guidance for working in this repo. **[ROADMAP.md](docs/ROADMAP.md) is the source of
truth** for what works, what's next (ordered), and the design choices worth
remembering — read it first. This file covers only how to build/run and the
conventions to respect when changing code.

## What this is

VibeOS — a from-scratch x86-64 OS: UEFI boot → higher-half kernel with its own
paging → preemptive SMP scheduler → isolated user processes → writable on-disk
filesystem (VibeFS) → runs **real static and dynamically-linked
`x86_64-linux-musl` binaries** over a serial shell. Linux syscall numbering from
day one, so cross-compiled musl runs with no translation layer.

## Build & run

```bash
make              # build bootloader + kernel + userspace binaries
./build.sh [size] # clean build + fresh VibeFS data disk (default 2G), /bin populated
make run          # boot in QEMU q35 + OVMF, -smp 4; serial = QEMU stdio
make clean        # NB: does not cover user/build — build.sh removes it explicitly
```

- Toolchain: `g++` (C++17 frontend, C-style code), `ld`, `mtools`, .NET 8 for
  the host disk tool, `qemu-system-x86_64` + OVMF.
- Verification is by **serial output** — every feature is expected to boot
  end-to-end on QEMU and be confirmed via the serial log, not just compile.
- Artifacts: `boot/build/vibeos.img` (FAT ESP), `boot/build/vdisk.img`
  (virtio-blk VibeFS volume).
- *Known issue:* unclean-`fsck` can drop `/bin/init` on diskutil volumes —
  workaround is a clean image per build (`./build.sh`). See [ROADMAP.md](docs/ROADMAP.md).

## Layout

- [boot/](boot/) — UEFI bootloader + ESP image builder.
- [kernel/](kernel/) — kernel, drivers, paging, scheduler, VibeFS, userspace
  support. Sources in [kernel/src/](kernel/src/), headers in
  [kernel/include/](kernel/include/).
- [gui/](gui/) — the graphical stack. [gui/client/](gui/client/) is the **userspace
  windowing system** (default): a `guiwm` server over mmap'd `/dev/fb0` + `/dev/input`,
  client apps one-process-per-window over loopback TCP (`gmandel`, `gclock`).
  [gui/core/](gui/core/) is the legacy in-kernel compositor (`gui.mode: kernel`).
- [user/](user/) — static userspace programs (`init`, `sh`, `hello`) + musl
  test binaries; [user/musl/](user/musl/).
- [interop/tools/diskutil/](interop/tools/diskutil/) — .NET `disktool-cli` host
  tool that creates/populates VibeFS images.

## Conventions to respect

These are load-bearing — the ROADMAP's "Design choices" section has the full
reasoning; the short version for editing code:

- **C++17 frontend, C-style code** — `extern "C"` across asm/TU boundaries; no
  RTTI, exceptions, or STL.
- **Higher-half, no identity map after boot** — reach physical memory via
  `PHYS_OFFSET` / `phys_to_virt`; device addresses via `kva_to_phys`. The low
  half is userspace.
- **`sched_lock` is held across context switches** (xv6 baton). Interrupt state
  is per-CPU (`push_off`/`pop_off`) and never stored in the lock.
- **User tasks run on every core** (work-stolen like kernel tasks; `swapgs` +
  per-CPU GS/TSS make ring-3 entry per-CPU). The ring-3 paths are therefore *not*
  lock-free — keep them safe when editing: file/page/vmspace/fdtable refcounts are
  atomic, the fd table has a per-`fdtable_t` spinlock (install/close/dup), the
  address space has a per-`vmspace_t` spinlock guarding page-table mutation + COW
  repair, `munmap`/`mprotect`/`fork` on a *shared* (multithreaded) AS issue a
  cross-core TLB shootdown (`tlb_shootdown_user`), and pipes/tty/net block under
  `sched_lock`. Lock map: [docs/smp-userspace-audit.md](docs/smp-userspace-audit.md).
- **EOI before the handler** — a handler that context-switches must not strand
  the line.
- **Fixed-size task/file/stack pools** — exhaustion is a panic by design; grow
  pools only with a real reason.
- **Linux syscall numbering** — match Linux ABI exactly so musl binaries run
  unmodified.

## When picking up work

The ordered backlog and the milestones ("can VibeOS run bash / Rust / a C
compiler / OpenSSH?") live in [ROADMAP.md](docs/ROADMAP.md) — start there rather
than inferring priorities from the code. Also noted there: the ABI
simplifications still open (dirs open read-only; no hard links / `rename`; mode
bits cosmetic; `fchdir` unsupported) and the shared prerequisites that block the
milestones (file-mutation syscalls, credentials, a real timebase, PTYs/termios,
more multiplexing).
