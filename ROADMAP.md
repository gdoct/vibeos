# VibeOS — what we have, what's next

The running picture of where the project is and what to build next. Every
feature below boots end-to-end on QEMU q35 + OVMF and is verified by serial
output (and, for the disk, by inspecting the backing file from the host).

VibeOS is a from-scratch x86-64 kernel: it boots over UEFI, runs in the higher
half with its own page tables, schedules preemptively across multiple CPUs,
isolates user processes in their own address spaces, has a writable on-disk
filesystem, and drops to an interactive shell over the serial console — with
`fork`/`execve`/`wait4` and programs loaded from disk.

---

## What works today

### Boot
- **UEFI bootloader** ([boot/](boot/)) — PE32+ app: reads `\vibeos\kernel.elf`
  off the ESP, loads each `PT_LOAD` at its `p_paddr`, gets a GOP framebuffer
  and the UEFI memory map, `ExitBootServices`, and jumps to the kernel with
  `rdi = BootInfo*` ([boot/include/bootinfo.h](boot/include/bootinfo.h)).
- **Higher-half kernel** at `-2 GiB` (`0xFFFFFFFF80000000`), `-mcmodel=kernel`
  ([kernel/linker.ld](kernel/linker.ld)). A tiny low `.boot` trampoline
  ([kernel/src/start.S](kernel/src/start.S)) builds bootstrap page tables,
  jumps high, and calls `kmain`. **The entire low canonical half is free for
  userspace** — there is no identity map after boot; the kernel reaches
  physical memory through a `PHYS_OFFSET` direct map.

### CPU & interrupts
- **GDT** ([kernel/src/gdt.S](kernel/src/gdt.S)) — null / kernel CS+DS / user
  CS+DS (DPL 3) / TSS. **TSS** ([kernel/src/tss.c](kernel/src/tss.c)) holds
  `rsp0` for ring 3 → ring 0 transitions.
- **IDT** ([kernel/src/idt.c](kernel/src/idt.c), [isr.S](kernel/src/isr.S)) —
  32 exception + 16 IRQ vectors; uniform `regs_t` frame; `#PF` decodes CR2 +
  error bits.
- **APIC** ([kernel/src/apic.c](kernel/src/apic.c)) — LAPIC + I/O APIC are the
  active controller (8259 masked fallback). MADT parse; PCI INTx routed to a
  shared vector; **per-CPU LAPIC timers** at 100 Hz drive preemption.

### SMP (multi-core)
- **AP bringup** ([kernel/src/ap_boot.S](kernel/src/ap_boot.S),
  [smp.c](kernel/src/smp.c)) — real-mode → long-mode trampoline copied to a low
  page; INIT-SIPI-SIPI. All CPUs come online (verified `-smp 4`).
- **xv6-style scheduler** ([kernel/src/task.c](kernel/src/task.c)) — each CPU
  runs its own `scheduler()` loop; tasks switch to/from a per-CPU scheduler
  context. One `sched_lock` guards all task state and is held across each
  context switch (baton: the resumed side releases it). Interrupt nesting is
  per-CPU (`push_off`/`pop_off`). Kernel tasks run + migrate across all cores;
  user tasks are pinned to the BSP for now.
- **IRQ-safe test-and-set spinlock** ([kernel/include/spinlock.h](kernel/include/spinlock.h));
  PMM, kmalloc, and `kprintf` are lock-protected.

### Processes & scheduling
- **Per-process address spaces** ([vmspace in paging.c](kernel/src/paging.c)) —
  each process owns a PML4; the kernel upper half (direct map, kstacks, kernel
  image) is shared by copying top-level entries; CR3 switches on schedule.
- **fork / execve / wait4** — eager-copy `fork`, `execve` loads a fresh image
  from disk, `wait4` reaps `TASK_ZOMBIE` children. Guarded 16 KiB kernel stacks
  (unmapped guard page). Wait queues, blocking `ksleep_ms`, timer preemption.

### Memory
- **Paging** ([kernel/src/paging.c](kernel/src/paging.c)) — own 4-level tables;
  `PHYS_OFFSET` direct map; high-half kstack window with guard pages;
  `vmap`/`vunmap`/`paging_query`/`kva_to_phys`.
- **PMM** ([pmm.c](kernel/src/pmm.c)) — bump allocator + single-page freelist.
- **kmalloc** ([kmalloc.c](kernel/src/kmalloc.c)) — power-of-two slabs +
  whole-page large path; header magic catches double-free.

### Devices & FS
- **virtio-blk** ([virtio_blk.c](kernel/src/drivers/virtio_blk.c)) — legacy PCI,
  IRQ-driven completion, **per-page scatter-gather DMA** (handles non-contiguous
  buffers). **PCI** config space, **ramdisk**, **framebuffer** (text/rect/pixel).
- **Serial console** — COM1 TX + RX; a canonical line-discipline **TTY**
  ([tty.c](kernel/src/tty.c)) polled from the timer tick backs `read(0)`.
- **VibeFS** ([fs.c](kernel/src/fs.c), [fs.h](kernel/include/fs.h)) — writable,
  non-journaled FS over `block_device_t`. 4 KiB blocks; superblock / bitmaps /
  inode table / data. 128-byte inodes with 13 direct + single/double/**triple**
  indirect and 64-bit size (files past 4 GiB). Crash-safe ordered writes +
  dirty-mount `fsck`. mkfs-on-first-boot.

### Userspace
- **Ring 3** via SYSCALL/SYSRET; ELF loader reads static `ET_EXEC` from VibeFS
  ([elf64.c](kernel/src/elf64.c)) and builds the System V initial stack.
- **Syscalls (Linux x86-64 numbers):** `read`(0→TTY), `write`(1/2→serial),
  `brk`, `getpid`, `fork`, `execve`, `exit`/`exit_group`, `wait4`.
- **`/bin/init` → `/bin/sh`** ([user/](user/)) — an interactive shell:
  `vibe$` prompt, line editing, run programs by name or path (`/bin/hello`),
  `help`/`exit`, exit-status reporting.

### Host tooling
- **`disktool-cli`** ([interop/tools/diskutil](interop/tools/diskutil/)) — C#
  VibeFS tool: `--create-volume`/`--mkdir`/`--import`/`--export`/`--ls`.
- **`build.sh`** — clean-build the kernel + bootloader + userspace, then create
  and populate a fresh VibeFS data disk with `/bin`. `make run` boots `-smp 4`.

---

## Milestones shipped (newest first)

Detailed per-phase build records live in git history; this is the index.

- **SMP** — IRQ-safe spinlocks + locked allocators; AP bringup
  (INIT-SIPI-SIPI trampoline); xv6-style per-CPU scheduler with a `sched_lock`
  baton and per-CPU timers. Kernel tasks run/migrate across 4 CPUs.
  *Deferred:* user tasks on APs (needs `swapgs`), per-CPU run queues,
  TLB-shootdown IPIs.
- **Userspace** — higher-half relink (freed the low half); ring 3 +
  SYSCALL/SYSRET; ELF loader; per-process address spaces; `fork`/`execve`/
  `wait4`; serial TTY + `read(0)`; programs loaded from disk; `/bin/sh`.
  *Deferred:* `argv`/`envp` through `execve`, COW fork, kstack reclamation,
  copy_to/from_user validation.
- **VibeFS** — writable on-disk FS, triple-indirect + 64-bit size, ordered
  writes + `fsck`; host `disktool-cli` to populate images.
- **Foundations** — wait queues + blocking I/O; IRQ-driven virtio-blk; real
  PMM + kmalloc; own page tables + direct map + guarded stacks; APIC + LAPIC
  timer tick.

---

## How it boots

```
UEFI/OVMF → BOOTX64.EFI → loads kernel.elf at p_paddr, jmp e_entry (low)
  → start.S: bootstrap page tables, jump to the high half, kmain()
  → gdt/idt/tss/syscall_init → pmm → paging (drop identity) → self-tests
  → fb → apic (LAPIC+IOAPIC, per-CPU timer) → virtio-blk → fs_mount(root)
  → irq_enable → sched_init → create init + worker tasks → smp_init (APs join)
  → scheduler()  [BSP + every AP loop here forever]
       └ runs /bin/init → execs /bin/sh → interactive prompt
```

---

## What's next, ordered

The previous top-three (SMP, filesystem, userspace) are all shipped, so the
list is re-anchored around the actual goal: **running real software.** Order
and rationale:

1. **§4 Linux ABI** is now #1 — it's the point of everything built so far
   (run a static `busybox`, `binutils`, eventually a compiler), it's the
   biggest single unlock, and nothing else blocks it.
2. **SMP completeness** (user-on-APs, per-CPU run queues) is demoted — the
   kernel is already multi-core for kernel work; these are performance/
   completeness, not capability gates, and §4 runs fine single-threaded.
3. **Robustness backlog** is last — real bugs and polish, but each has a
   workaround and none gates the headline goal.

### §4 — Linux ABI: run real static binaries (musl → busybox → binutils)

**The approach.** Don't write a native toolchain first. Cross-compile real
software on the Linux host as static `x86_64-linux-musl`, drop the ELF on the
VibeFS disk, and run it. VibeOS already uses Linux syscall numbers and loads
static ELFs from disk — so the only gap is **syscall breadth + ABI fidelity.**
Today userspace can make ~9 syscalls; a static musl binary needs ~30–50, plus
TLS and a fleshed-out initial stack.

Practical method: the dispatcher already logs unknown syscalls — run the
binary, see what it traps on, implement that one, repeat.

**Rung 1 — a static musl `hello world` runs.** The real gate; *every* real
binary needs these before `main`:
- **TLS:** `arch_prctl(ARCH_SET_FS)` → `wrmsr(FS_BASE)`, saved/restored per task
  (musl won't start without thread-local storage).
- **Anonymous `mmap`/`munmap`/`mprotect`** — musl's malloc is mmap-based; the
  kernel has none today.
- **auxv:** `AT_PAGESZ`, `AT_RANDOM` (stack canary), `AT_PHDR/PHENT/PHNUM/ENTRY`.
- **Startup stubs:** `set_tid_address`, `rt_sigprocmask`/`rt_sigaction`
  (return 0), `ioctl(TCGETS)` for `isatty`.
- **Loader fixes:** raise the 256 KiB image cap (a static `ld` is several MB),
  grow the 32 KiB user stack, and **pass `argv`/`envp` through `execve`** (it
  currently fabricates `argv[0]=path`).

**Rung 2 — programs that do file I/O** (busybox `cat`/`ls`/`echo`):
- A real **per-process fd table** (today fds 0/1/2 are hardwired).
- `openat`/`close`/`lseek`/`read`/`write`/`fstat`/`newfstatat`/`getdents64`/
  `getcwd` wired to VibeFS.
- **Linux `struct stat` / `dirent64` layouts** (mechanical but must be exact).

**Rung 3 — `busybox`, then `binutils` (`as`/`ld`/`ar`).** Mostly widening from
rung 2: more `stat` variants, file-backed `mmap` (BFD; or a read fallback),
`readlink`, `clock_gettime`, `getrandom`, `uname`, `fcntl`, `dup`. A static
`busybox` shell is the headline milestone; `ld`/`as` follow as a few more
syscalls each.

*Later/optional:* a **vDSO** so `clock_gettime`/`getcpu` don't trap; `PT_INTERP`
+ `ld-linux.so` for dynamic binaries (static first); thin `/proc` & `/sys`.

**Rough size:** rung 1 ≈ one focused session (TLS + mmap + auxv are the
load-bearing parts), rung 2 another (fd table + FS syscalls + struct layouts),
binutils a few more on top — but past rung 1 it's *widening*, not architecture.

### SMP completeness

- **`swapgs` syscall/ISR path → user tasks on any CPU.** Today user tasks are
  BSP-pinned because the ring-3 kernel-stack latch is one global. Add `swapgs`
  to `syscall_entry` and a conditional `swapgs` to the ISR stubs, with per-CPU
  state in `KERNEL_GS_BASE`. *Why later:* it's throughput/completeness; §4
  binaries run fine on the BSP.
- **Per-CPU run queues + work-stealing.** Today it's one global queue with a
  first-READY picker, so load is uneven. *Why later:* fairness/scaling, not
  correctness.
- **TLB-shootdown IPIs.** Needed once a shared mapping is *unmapped* while other
  CPUs may have it cached (e.g. munmap on a future threaded process). *Why
  later:* nothing unmaps shared pages concurrently yet.

### Robustness backlog

- **Unclean-shutdown `fsck` drops `/bin/init`** on a diskutil-made volume
  (kernel `fsck` ↔ host-tool layout nuance). Workaround: the clean-image-
  per-build flow. Worth root-causing (likely an inode-link-count or bitmap
  detail) before relying on persistence across crashes.
- **COW fork** (currently eager-copies the whole user half).
- **Kernel-stack + slot reclamation** for reaped tasks (kstacks currently leak).
- **ACPI poweroff** so a clean `exit` actually powers off (today it halts).
- **FS:** journaling, `rename`, multiple mounts, permissions/uid.

---

## Design choices worth remembering

- **C++17 frontend, C-style code.** Built with `g++`; `extern "C"` on anything
  asm/cross-TU touches. No RTTI, exceptions, or STL.
- **Higher-half kernel, no identity map.** All physical access goes through the
  `PHYS_OFFSET` direct map (`phys_to_virt`); device-facing addresses use
  `kva_to_phys`. The low half belongs entirely to userspace.
- **Linux syscall numbering from day one** — so cross-compiled musl binaries
  (§4) work without a translation layer.
- **`sched_lock` is held across context switches** (xv6 baton); interrupt
  state is per-CPU (`push_off`/`pop_off`), never stored in the lock.
- **Legacy virtio + shared-vector INTx + APIC** — chosen for simplicity over
  MSI-X / ACPI `_PRT` / an AML interpreter; revisit only if a device needs it.
- **EOI before the handler, always** — a handler that context-switches away
  must not strand the interrupt line.
- **Fixed-size task pool & kernel stacks** — cheap, obvious failure mode
  (panic). Grow when there's a real reason.

---

## How to run

```bash
./build.sh            # clean build + fresh VibeFS disk populated with /bin
make run              # boot QEMU, -smp 4, serial on stdio
```

Serial is the console (stdio); the framebuffer is in the QEMU window. The
`boot/build/vdisk.img` virtio-blk backing file can be inspected from the host
(or with `disktool-cli --ls`) after a run. Each `./build.sh` produces a clean
image, which also sidesteps the unclean-`fsck` caveat above.
