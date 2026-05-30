# VibeOS — what we have, what's next

A from-scratch x86-64 kernel: boots over UEFI into the higher half with its own
page tables, schedules preemptively across multiple CPUs, isolates user
processes in their own address spaces, has a writable on-disk filesystem, and
runs **real static `x86_64-linux-musl` binaries** over a serial shell. Every
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
  per-CPU scheduler with one `sched_lock` baton held across switches
  ([task.c](kernel/src/task.c)). Kernel tasks migrate across all cores; **user
  tasks are BSP-pinned** (no `swapgs` yet).
- **Memory** — own 4-level paging + direct map + guarded kstacks
  ([paging.c](kernel/src/paging.c)); bump+freelist PMM; slab/large kmalloc.
- **Processes** — per-process address spaces; `fork`/`execve`/`wait4`; wait
  queues, blocking sleep, timer preemption.
- **Devices/FS** — IRQ-driven virtio-blk with scatter-gather DMA; PCI;
  framebuffer; serial TTY backing `read(0)`. **VibeFS** ([fs.c](kernel/src/fs.c)) —
  writable, crash-safe ordered writes + `fsck`, 4 KiB blocks, triple-indirect +
  64-bit size.
- **Userspace / Linux ABI** — ring 3 via SYSCALL/SYSRET; ELF loader with real
  auxv + argv/envp ([elf64.c](kernel/src/elf64.c)). **Runs unmodified static
  musl binaries** (`/bin/mhello`, `/bin/ftest`, `/bin/pipetest`). Syscalls:
  TLS (`arch_prctl`), anon `mmap`/`munmap`/`mprotect`, `read`/`write`/`writev`,
  `brk`, signals/`madvise` (stubs); **file I/O over a per-process fd table** —
  `open`/`openat`/`close`/`lseek`/`stat`/`fstat`/`getdents64`/`getcwd`/`fcntl`/
  `dup`/`dup2` ([file.c](kernel/src/file.c)); **`pipe`/`pipe2`**
  ([pipe.c](kernel/src/pipe.c)); `fork`/`execve`/`wait4`/`exit`. Non-crypto
  kernel RNG ([random.c](kernel/src/random.c), RDRAND + xorshift fallback)
  seeds `AT_RANDOM`.
- **Shell + tooling** — `/bin/init` → `/bin/sh` over serial; host `disktool-cli`
  ([interop/tools/diskutil](interop/tools/diskutil/)) builds/populates VibeFS
  images; `./build.sh` + `make run` (`-smp 4`).

**ABI simplifications still open:** cwd fixed at `/` (no `chdir`); no symlinks;
dirs open read-only; `FD_CLOEXEC` not enforced; eager (non-COW) fork;
copy_to/from_user unvalidated.

---

## What's next, ordered

§4 rungs 1–2 (static musl: TLS, mmap, fd table, file I/O, pipes) are shipped.
Lock down fundamentals before piling on subsystems — memory corruption is far
cheaper to prevent than to debug once networking/dynamic-linking are in play.

**1. Memory correctness + safety.** COW fork (today eager-copies the whole user
half); `copy_to/from_user` validation (bad user pointers currently fault in the
kernel); kernel-stack reclamation for reaped tasks (kstacks leak); guarded user
stacks (mirror the kernel guard-page trick).

**2. User tasks on all cores.** Per-CPU run queues + work-stealing; TLB-shootdown
IPIs; `swapgs` on syscall/IRQ entry with per-CPU `KERNEL_GS_BASE`, which lifts
the BSP-pin on user tasks. Unlocks real multicore performance.

**3. Signals (real Linux semantics).** Promote the `rt_sig*` stubs: delivery on
return to userspace, `sigaltstack`, default actions (term/core/stop/cont),
`kill`/`tgkill`/`sigprocmask`, pending queues. Most real binaries expect these.

**4. Dynamic linking (`ET_DYN` + `ld-musl`).** PIE support, `PT_INTERP` loader
path, file-backed `mmap`, `mprotect` correctness, ship `ld-musl.so`. Unlocks a
huge amount of software (and most distro binaries).

**5. Networking — virtio-net + IPv4/UDP/TCP.** Port, don't invent — use BSD/lwIP
as the source. virtio-net driver (same shape as virtio-blk); ARP, IPv4, ICMP
(`ping`), UDP, TCP; socket syscalls (`socket`/`bind`/`listen`/`accept`/`connect`/
`sendto`/`recvfrom`/`poll`) riding the §4 fd table. Payoff: ported `wget`/`curl`.

**6. Proper CSPRNG.** ChaCha20 DRBG (or Fortuna) over real entropy — RDRAND,
timing jitter, virtio-rng — replacing `krandom` for networking/TLS. (`krandom`
stays fine for boot entropy / `AT_RANDOM`.)

**7. Userspace quality-of-life.** Tiny `/proc` (pid dirs + `stat`); `/dev`
(`null`/`zero`/`tty`/`random`); a real (small) init; a package format
(tar + VibeFS metadata).

**8. Toolchain integration.** A cross target `x86_64-vibeos-musl` + a sysroot, so
`gcc`/`clang` build for VibeOS directly instead of repurposing host musl.

**Also widening §4 toward busybox/binutils** as opportunity allows (more
`stat`/`fcntl` variants, `clock_gettime`, `getrandom`, `uname`); it advances
with items 3–4.

> **TL;DR — best bang-for-effort next five:** (1) COW fork + usercopy validation,
> (2) user tasks on all cores + TLB shootdowns, (3) signals, (4) dynamic linking,
> (5) networking.

*Known issue:* unclean-`fsck` drops `/bin/init` on diskutil volumes (workaround =
clean image per build); root-cause before relying on crash persistence. Other
backlog: ACPI poweroff; FS journaling/`rename`/permissions.

---

## How it boots

```
UEFI/OVMF → BOOTX64.EFI → kernel.elf → start.S (bootstrap tables, jump high) → kmain()
  → gdt/idt/tss/syscall → pmm → paging (drop identity) → apic+timer → virtio-blk
  → fs_mount → irq_enable → sched_init → create init+workers → smp_init (APs join)
  → scheduler() [every CPU] → /bin/init → /bin/sh
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
  per-CPU (`push_off`/`pop_off`), never in the lock.
- **Legacy virtio + shared-vector INTx + APIC** — simplicity over MSI-X / ACPI
  `_PRT` / AML; revisit only if a device demands it.
- **EOI before the handler** — a handler that context-switches must not strand
  the line.
- **Fixed-size task/file/stack pools** — cheap, obvious failure (panic). Grow
  when there's a real reason.
