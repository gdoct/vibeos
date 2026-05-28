# MyOS — what we have, what's next

This document is the running picture of where the project is and the
order of next steps. It's the place to come back to when "what should I
build next?" gets fuzzy.

## What works today

Everything below boots end-to-end on QEMU q35 + OVMF and is verified by
serial output (and, for block writes, by reading the backing file from
the host).

### Boot chain
- **UEFI bootloader** ([boot/](boot/)) — PE32+ application. Reads
  `\myos\kernel.elf` off the ESP, validates the ELF, allocates each
  `PT_LOAD` segment at its `p_paddr`, queries GOP for a linear
  framebuffer, fetches the memory map, calls `ExitBootServices`, and
  jumps to the kernel entry with `rdi = BootInfo*`. Built as an ELF DSO
  then `objcopy`'d to PE32+ with a hand-rolled `.reloc` stub — see
  [boot/efi.lds](boot/efi.lds) and [boot/src/reloc.S](boot/src/reloc.S).
- **BootInfo handoff** ([boot/include/bootinfo.h](boot/include/bootinfo.h))
  — framebuffer geometry, UEFI memory map descriptor array, ACPI RSDP,
  runtime services pointer, kernel image range.

### Kernel core
- **Linked at 1 MiB physical** ([kernel/linker.ld](kernel/linker.ld)),
  small code model. We build our own 4-level page tables at boot (see
  Paging below) but keep the kernel identity-mapped, so no relink is
  needed.
- **GDT** ([kernel/src/gdt.S](kernel/src/gdt.S)) — three descriptors
  (null, kernel CS=0x08, kernel DS=0x10). Loaded by `gdt_init`, which
  also reloads CS via a far return.
- **IDT** ([kernel/src/idt.c](kernel/src/idt.c)) — 32 exception vectors
  + 16 IRQ vectors. ISR stubs in
  [kernel/src/isr.S](kernel/src/isr.S) save a uniform
  [regs_t](kernel/include/regs.h) frame and dispatch to one of two C
  entrypoints:
  - `exception_handler` → register dump + panic
    ([kernel/src/exception.c](kernel/src/exception.c))
  - `irq_dispatch` → look up handler, **EOI first**, then call
    ([kernel/src/irq.c](kernel/src/irq.c))
- **APIC (LAPIC + I/O APIC)** ([kernel/src/apic.c](kernel/src/apic.c))
  — the active interrupt controller. `apic_init` parses the ACPI MADT
  (via the BootInfo RSDP → RSDT/XSDT) for the LAPIC and I/O APIC, enables
  the LAPIC (base MSR + spurious vector 0xFF), masks every I/O APIC
  redirection entry, and routes the PCI INTx GSIs (16..23) to one shared
  vector (`0x2B`). The system tick is the **LAPIC timer**, calibrated
  against PIT channel 2 and run periodic at 100 Hz on vector `0x20`.
  `irq_dispatch` EOIs through the LAPIC in this mode. The 8259 is left
  fully masked; if the MADT is unusable we fall back to it + the PIT.
- **8259 PIC** ([kernel/src/pic.c](kernel/src/pic.c)) — legacy fallback
  only. Remapped to 0x20..0x2F, all lines masked at init. `pic_unmask` /
  `pic_mask` / `pic_eoi`.
- **PIT** ([kernel/src/drivers/timer.c](kernel/src/drivers/timer.c)) —
  channel 2 is the LAPIC-timer calibration reference; channel 0 only
  drives the tick in the no-APIC fallback (`timer_start_pit`). `g_ticks`
  counter; `ksleep_ms` blocks the task on a sleeper list (busy-`hlt` only
  before the scheduler exists).
- **Round-robin preemptive scheduler**
  ([kernel/src/task.c](kernel/src/task.c),
  [kernel/src/context_switch.S](kernel/src/context_switch.S)) — fixed
  pool of 8 tasks, 16 KiB per-task kernel stack from PMM, cooperative
  `task_yield` *and* timer-driven preemption (via `sched_tick` from the
  system-tick IRQ — the LAPIC timer). A `TASK_BLOCKED` state plus an
  always-runnable **idle task**
  (`sti; hlt`) mean a CPU with no ready work draws no power instead of
  spinning. Verified by workers that sleep and an I/O worker that blocks.
- **Wait queues + blocking sleep** ([kernel/src/task.c](kernel/src/task.c))
  — `wait_queue_t` FIFO of blocked tasks; `wait_queue_sleep` /
  `wake_one` / `wake_all`, built on `sched_block_and_switch` /
  `sched_make_ready`. `ksleep_ms` now blocks the task on a timer sleeper
  list keyed on an absolute tick deadline; `on_tick` wakes anything due.
  IRQ-state save/restore (`irq_save`/`irq_restore` in
  [kernel/include/irq.h](kernel/include/irq.h)) makes the wake paths
  safe from both task and IRQ context.
- **Paging / virtual memory** ([kernel/src/paging.c](kernel/src/paging.c))
  — our own 4-level page tables replace the firmware's at boot. `PML4[0]`
  identity-maps low physical memory with 2 MiB pages (kernel keeps
  running unrelinked); `PML4[256]` is a Linux-style **direct map** at
  `PHYS_OFFSET` (0xFFFF800000000000) that shares the same PD tables, so
  any phys address is reachable via `phys_to_virt`. The map auto-extends
  past 4 GiB to cover the framebuffer/MMIO. A 4 KiB `vmap` / `vunmap` /
  `paging_query` walker (intermediate tables allocated on demand) backs
  **guarded kernel stacks**: each task stack lives in a high-half
  vmalloc window (`KSTACK_REGION`) with an unmapped guard page below, so
  a stack overflow takes a clean `#PF` instead of corrupting a neighbour.
  The exception handler decodes `#PF` error bits + CR2. Verified by a
  direct-map round-trip, a vmap/query/unmap round-trip, and a guard-page
  fault landing exactly on the guard address.

### Device subsystem
- **Device registry** ([kernel/include/device.h](kernel/include/device.h))
  — class-tagged linked list of `device_t`. Drivers embed `device_t` as
  the first field of their own struct and downcast container-of style.
- **Framebuffer driver**
  ([kernel/src/drivers/fb.c](kernel/src/drivers/fb.c)) — pixel /
  rect / 8x8 ASCII text. Hand-coded 5x7 font in
  [kernel/src/drivers/font.c](kernel/src/drivers/font.c). Lowercase
  folded to upper at draw time.
- **Block device interface** ([kernel/include/block.h](kernel/include/block.h))
  — `read(lba, count, buf)` / `write(lba, count, buf)`, errno-style
  return codes.
- **RAM disk** ([kernel/src/drivers/ramdisk.c](kernel/src/drivers/ramdisk.c))
  — 256 KiB backed by PMM pages. Exercises the interface end-to-end.
- **Virtio-blk** ([kernel/src/drivers/virtio_blk.c](kernel/src/drivers/virtio_blk.c))
  — legacy PCI IO BAR, single virtqueue, three descriptors per request
  (header / data / status). **IRQ-driven completion**: the completion
  interrupt arrives via the I/O APIC (the shared PCI INTx vector; on the
  PIC fallback, the line from PCI config `0x3C`). The handler reads the
  clear-on-read ISR register and wakes the submitter via a per-device
  wait queue, so a reading task blocks (letting others run) instead of
  polling `used_idx`. Falls back to polling during early boot before the
  scheduler exists. Verified: bytes the kernel writes show up in the
  host `vdisk.img` byte-for-byte, and an I/O worker completes 8 reads
  via IRQ while other tasks run.
- **PCI config space**
  ([kernel/src/drivers/pci.c](kernel/src/drivers/pci.c)) — type-1 via
  ports 0xCF8/0xCFC, `pci_find(vendor, dev_lo, dev_hi)`.

### Filesystem
- **MyFS v1** ([kernel/src/fs.c](kernel/src/fs.c),
  [kernel/include/fs.h](kernel/include/fs.h)) — a tiny, non-journaled,
  writable filesystem over any `block_device_t`. 4 KiB FS blocks (8 sectors);
  superblock / inode bitmap / data bitmap / fixed inode table / data area.
  128-byte inodes with 13 direct + one single-indirect pointer (files up to
  ~4 MiB). ext2-style variable-length directory entries with `.`/`..`.
  **mkfs runs on first boot** when it sees an unformatted volume. VFS-ish
  kernel API: `fs_mount`/`fs_unmount`, `fs_open`/`fs_read`/`fs_write`/
  `fs_close`, `fs_create`/`fs_mkdir`/`fs_unlink`/`fs_readdir`. The inode table
  is the source of truth; the bitmaps are a derived cache, so every mutating
  op writes in crash-safe order (data → inode → dirent → bitmaps+superblock)
  and a **dirty-mount `fsck`** rebuilds the bitmaps, fixes link counts, drops
  dangling directory entries, and frees orphans. Verified end-to-end on
  virtio-blk: format, a 64 KiB indirect-block file round-trip, `readdir`,
  `unlink`, persistence across a clean unmount/remount **and across a reboot**
  (the `MYFS` magic + file bytes are visible in the host `vdisk.img`), plus a
  forced-dirty `fsck` that rebuilds a deliberately-wiped data bitmap with the
  data left intact.

### Memory & I/O plumbing
- **Page tables** — see *Paging / virtual memory* under Kernel core.
- **Physical page allocator** ([kernel/src/pmm.c](kernel/src/pmm.c)) —
  bump allocator over the largest ConventionalMemory region above the
  kernel image, plus a **single-page freelist**: `pmm_free_page` returns
  a page (storing the next-free pointer in its own first 8 bytes) and
  `pmm_alloc_page` reuses it before the bump cursor advances. Multi-page
  requests still come from the bump arena (no contiguity guarantee from
  the freelist).
- **kmalloc / kfree** ([kernel/src/kmalloc.c](kernel/src/kmalloc.c)) —
  power-of-two slab classes (32..2048 B) carved from PMM pages with a
  per-class freelist; allocations larger than the top class are rounded
  up to whole pages. Each block carries a 16-byte header (magic +
  class + size) so `kfree` finds its class and a double-free trips a
  panic. Verified by a boot-time heap self-test.
- **Serial** (COM1, 115200 8N1) — only output device for kernel logs
  during boot, mirrored to QEMU stdio.
- **libk** — `kmemset` / `kmemcpy` / `kmemcmp` / `kstrlen`, `kprintf`
  with `%d %u %x %X %s %c %p`, width + zero-pad + left-justify, `l` /
  `z` length modifiers.

## How the pieces fit

```
┌──────────────────────────────────────────────────────────────┐
│ UEFI firmware (OVMF)                                         │
│   ↳ loads \EFI\BOOT\BOOTX64.EFI via startup.nsh              │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ Bootloader (boot/)                                           │
│   reads kernel.elf  →  loads PT_LOAD at p_paddr              │
│   queries GOP        →  fills FramebufferInfo                │
│   ExitBootServices   →  freezes memory map into BootInfo     │
│   jmp kernel_entry, rdi = BootInfo*                          │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│ kmain (kernel/src/main.c)                                    │
│   gdt_init → idt_init  →  exceptions live                    │
│   pmm_init(BootInfo) → paging_init (CR3) → self-tests        │
│   fb_init                                                    │
│   irq_init → timer_init → apic_init (LAPIC+IOAPIC) [IF 0]    │
│   ramdisk_init, virtio_blk_init (hooks INTx) → self-tests    │
│   sti  →  device_dump                                        │
│   scheduler_demo: sched_init + idle + workers + io task      │
└──────────────────────────────────────────────────────────────┘

  IRQ path (PIT example):
    CPU push (SS,RSP,RFLAGS,CS,RIP)
      → irq0 stub pushes vec=0x20, errcode=0
        → irq_common saves 15 GPRs, calls irq_dispatch(regs*)
          → pic_eoi(0)
          → handler = on_tick: g_ticks++; sched_tick()
            → schedule_to_next() → context_switch(&prev->rsp, next->rsp)
              ... new task resumes via its own irq_common's iretq ...
```

## Recently shipped

The first four follow-ups are done — they're folded into "What works
today" above:

- **Wait queues + blocking I/O** — `TASK_BLOCKED`, an idle task,
  `wait_queue_*`, and a `ksleep_ms` that blocks on a timer sleeper list.
- **Virtio-blk IRQ-driven completion** — INTx handler reads the ISR and
  wakes the submitter via a per-device wait queue; polling only as an
  early-boot fallback. (Note: this needed a PIC fix — unmasking a slave
  line, 8..15, now also opens the master IRQ2 cascade.)
- **Real PMM + kmalloc** — single-page freelist (`pmm_free_page`) and a
  power-of-two slab `kmalloc`/`kfree`.
- **Paging + virtual memory** — own 4-level tables (identity + a
  `PHYS_OFFSET` direct map), 4 KiB `vmap`/`vunmap`, and guarded kernel
  stacks. *Deferred:* the kernel is still identity-mapped rather than
  relinked into the higher half — fine for now, revisit when userspace
  needs the low half (see Userspace below).
- **APIC (LAPIC + I/O APIC)** — replaced the 8259 as the active
  controller: MADT parse, LAPIC enable, I/O APIC routing of PCI INTx to a
  shared vector, LAPIC-timer system tick calibrated against the PIT.
  *Deferred:* MSI/MSI-X + modern virtio (the I/O APIC INTx path works
  today, so this is no longer blocking); the ACPI `_PRT` (we route all
  PCI GSIs to one shared vector instead of resolving each device's pin —
  correct for shared INTx, revisit if we need per-device routing).
- **Filesystem (MyFS v1)** — the §2 follow-up below, finished end-to-end:
  on-disk format, direct + single-indirect blocks, the full kernel file API,
  crash-safe ordered writes, and a dirty-mount `fsck`. Folded into "What works
  today" above. *Deferred:* no journaling, no rename, single global mounted
  volume, no permissions/uid — all fine until userspace needs them.

## Follow-ups, ordered

This is roughly the order to do them in — each one removes the most
limiting constraint at the time it's chosen.

### 1. SMP

**Why now.** One CPU is fine, but most of the interesting design
questions (per-CPU run queues, locking, IPIs, RCU) only show up at
N>1.

**What to build.**
- Bring up APs via INIT-SIPI-SIPI from BSP
- Per-CPU `task_t *current` (`gs:0` trick)
- Per-CPU runqueues + work stealing or a single locked global queue
- Spinlocks; spinlock irqsave variants for IRQ-shared data
- IPI scaffolding (LAPIC ICR)

**Unlocks.** Real concurrency, real performance discussions.

### 2. Filesystem: simple read/write from day one

**✅ Shipped (MyFS v1).** Built as described below — see the *Filesystem*
section under "What works today". The design notes here are kept as the
record of what was built and what was intentionally deferred. **Userspace
(below) is now the next step.**

**Why now.** Right now the kernel can read and write raw blocks on
virtio-blk, but there's no way to load anything *named* or persist
structured state safely. A minimal writable filesystem gives us both
program loading and real stateful behavior immediately.

**What to build.**
- Start with a tiny non-journaled writable FS over `block_device_t`
  (single volume, fixed block size, straightforward on-disk metadata)
- VFS-ish file API for kernel callers from day one:
  `open` / `read` / `write` / `create` / `mkdir` / `unlink` / `close`
- Keep consistency rules simple (ordered metadata updates + fsck tool on
  boot after unclean shutdown) instead of adding journaling now
- Pick one clear target format and finish it end-to-end before adding
  compatibility layers or a second FS

**Layout details (v1 target).**
- **Block size:** 4096 bytes everywhere (metadata + data)
- **Block 0: superblock**
  - Magic/version, total blocks, inode count, feature flags
  - Block numbers for inode bitmap, data bitmap, inode table, data area
  - Root inode number, clean/dirty unmount flag, last mount/write tick
- **Inode bitmap:** 1 bit per inode (allocated/free)
- **Data bitmap:** 1 bit per data block (allocated/free)
- **Inode table:** fixed-size inode records (for example 128 bytes each)
  - Type (`file`/`dir`), size, link count, direct block pointers
  - One single-indirect pointer (optional in v1 if direct blocks are enough)
  - Basic timestamps (`ctime`/`mtime`)
- **Data area:** file contents and directory blocks
- **Directory format:** variable-length entries in data blocks
  - `inode`, `type`, `name_len`, `name[]`
  - Always maintain `.` and `..` for directories

**Write/update rules (no journal).**
- Allocate in bitmap first in memory, but write blocks in crash-safe order:
  data block -> inode (size/pointers) -> directory entry -> bitmap + superblock
- On delete/unlink, reverse the visibility first:
  directory entry removal -> inode link update/free -> data free in bitmap
- Superblock `dirty` set on mount, cleared only on clean unmount
- `fsck` on dirty mount repairs:
  - bitmap vs inode-table mismatches
  - directory entries pointing to invalid/free inodes
  - inode link-count mismatches
  - orphaned allocated blocks/inodes

**Unlocks.** Loading executables, writable config/state, logs, and
real application workflows that survive reboot.

### 3. Userspace

**Why now.** Everything until here has been kernel-only. Userspace
makes the kernel an actual *operating system* rather than a
single-binary monolith.

**What to build.**
- Ring 3 entry: SYSCALL/SYSRET MSRs (`STAR`, `LSTAR`, `SFMASK`)
- Per-process address space (CR3 switch on schedule)
- ELF loader for userspace binaries (loaded from the kernel FS)
- Syscall table — start with the bare minimum: `write`, `read`,
  `exit`, `getpid`, `mmap`, `brk`
- A `init` userspace program that does something visible

**Unlocks.** Multiple isolated processes, signals, fork/exec —
basically everything that makes UNIX UNIX.

### 4. Linux ABI compatibility

The note in [kernel/README.md](kernel/README.md) says the kernel
"should aim to be Linux compatible — run Linux applications and support
Linux syscalls." That's the long-term aim. Concretely it means:
- Match the Linux `x86_64` syscall numbering (`__NR_*` constants)
- Implement `vDSO` so `gettimeofday`, `clock_gettime`, `getcpu` don't
  need to trap
- Match enough of `/proc` and `/sys` that musl-static binaries work
- ELF interpreter handling (`PT_INTERP`, `ld-linux.so`) — though
  static binaries first

A reasonable milestone: a statically-linked `busybox` binary runs.

## Design choices worth remembering

These will come up again. Capturing them so future work doesn't relitigate.

- **C++17 frontend, C-style code.** Built with `g++` mostly because of
  the original boot Makefile. We use `extern "C"` liberally on anything
  asm or other-TU code touches. Don't grow this into Real C++ —
  no RTTI, no exceptions, no STL.
- **Own page tables, but kernel still identity-mapped.** We build our
  own PML4 (identity + `PHYS_OFFSET` direct map) but deliberately did
  *not* relink the kernel into the higher half — that's a bigger change
  (load vs. virtual addresses, a high-VA entry trampoline) with no payoff
  until userspace wants the low half. Keeping identity means existing
  physical-pointer code (PMM, virtio descriptors, framebuffer) keeps
  working untouched. New code that wants a phys address should reach it
  via `phys_to_virt` rather than assuming identity, so the eventual
  higher-half move is cheap.
- **Legacy virtio over modern.** ~300 lines vs. ~800. Its INTx
  completion now arrives through the I/O APIC, so the original reason to
  swap (needing MSI) is no longer pressing; revisit only if we want
  MSI-X / multiqueue.
- **APIC, not the 8259.** The LAPIC + I/O APIC are now the active
  controller (the 8259 is a masked fallback). We route *all* PCI INTx
  GSIs to one shared vector rather than parse the ACPI `_PRT` — correct
  for shared, level-triggered INTx and far cheaper than an AML
  interpreter. The LAPIC timer (not the PIT) is the system tick.
- **Fixed-size task pool, fixed-size kernel stacks**. Cheap and obvious
  failure mode (panic). Grow when there's a real reason.
- **C++ `const` globals need `extern "C"`** to be visible across TUs.
  Bit us once in the framebuffer font; will bite again.
- **EOI before handler, always.** A handler that context-switches away
  must not strand the interrupt line (PIC or LAPIC). For level-triggered
  I/O APIC lines this can cause one redundant re-fire, which the device's
  ISR-claim check absorbs harmlessly. The comment in
  [kernel/src/irq.c](kernel/src/irq.c) explains it.

## How to run

```bash
make          # builds kernel + bootloader
make image    # also builds the disk image and creates vdisk.img
make run      # boots in QEMU with virtio-blk attached
```

Serial goes to QEMU stdio. Framebuffer is in the QEMU window. The
`vdisk.img` file in `boot/build/` is the virtio-blk backing store — you
can inspect it from the host after a run to confirm writes landed.
