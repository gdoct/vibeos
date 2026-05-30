# VibeOS — what we have, what's next

A from-scratch x86-64 kernel: boots over UEFI into the higher half with its own
page tables, schedules user processes preemptively across **all** CPUs, isolates
them in copy-on-write address spaces, delivers POSIX signals, has a writable
on-disk filesystem and an IPv4/TCP network stack, and runs **real static *and*
dynamically-linked `x86_64-linux-musl` binaries** over a serial shell. Every
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
  ([task.c](kernel/src/task.c)). **User tasks run on every core**: per-CPU TSS +
  GS base with `swapgs` on syscall/IRQ entry ([percpu.c](kernel/src/percpu.c),
  [usermode.S](kernel/src/usermode.S), [isr.S](kernel/src/isr.S)); APs enable
  SYSCALL + SSE. Cross-CPU IPIs + TLB shootdown ([apic.c](kernel/src/apic.c)).
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
- **Networking** — virtio-net driver ([virtio_net.c](kernel/src/drivers/virtio_net.c))
  sharing PCI INTx with virtio-blk; a compact IPv4 stack ([net.c](kernel/src/net.c)):
  ARP, IPv4, ICMP (`ping`), UDP, TCP (handshake/data/close), plus a loopback
  netif. **BSD sockets** on the fd table —
  `socket`/`bind`/`listen`/`accept`/`connect`/`sendto`/`recvfrom`/`poll`. Static
  SLIRP addressing (10.0.2.15/24). Ported `/bin/wget` fetches over the stack.
- **Userspace / Linux ABI** — ring 3 via SYSCALL/SYSRET; ELF loader with real
  auxv + argv/envp ([elf64.c](kernel/src/elf64.c)). **Runs unmodified static and
  dynamically-linked musl binaries** — `ET_DYN`/PIE + `PT_INTERP` load
  `ld-musl.so`, file-backed `mmap`, `mprotect` (`/bin/dynhello`, `/bin/mhello`,
  `/bin/ftest`, `/bin/pipetest`, `/bin/sigtest`, `/bin/nettest`). Syscalls: TLS
  (`arch_prctl`), anon + file `mmap`/`munmap`/`mprotect`, `read`/`write`/`writev`,
  `brk`, `nanosleep`, signals, sockets; **file I/O over a per-process fd table** —
  `open`/`openat`/`close`/`lseek`/`stat`/`fstat`/`getdents64`/`getcwd`/`fcntl`/
  `dup`/`dup2` ([file.c](kernel/src/file.c)); **`pipe`/`pipe2`**
  ([pipe.c](kernel/src/pipe.c)); `fork`/`execve`/`wait4`/`exit`. Non-crypto
  kernel RNG ([random.c](kernel/src/random.c), RDRAND + xorshift fallback)
  seeds `AT_RANDOM`.
- **Shell + tooling** — `/bin/init` → `/bin/sh` over serial; host `disktool-cli`
  ([interop/tools/diskutil](interop/tools/diskutil/)) builds/populates VibeFS
  images; `./build.sh` + `make run` (`-smp 4`, virtio-blk + virtio-net).

**ABI simplifications still open:** cwd fixed at `/` (no `chdir`); no symlinks;
dirs open read-only; `FD_CLOEXEC` not enforced. **TCP has no retransmission /
reassembly / RTT** — fine for loopback and low-loss SLIRP, not yet a WAN stack.

---

## What's next, ordered

The original "next five" — (1) memory correctness + safety (COW fork, usercopy
validation, kstack reclamation, guarded stacks), (2) user tasks on all cores
(per-CPU TSS/GS/`swapgs`, IPIs/TLB shootdown), (3) signals, (4) dynamic linking
(`ET_DYN` + `ld-musl`, file-backed `mmap`), and (5) networking (virtio-net +
ARP/IP/ICMP/UDP/TCP + BSD sockets + ported `wget`) — are **all shipped** and
serial-verified. See "What works today". What remains, ordered:

**1. WAN-grade TCP.** The stack is correct over loopback / low-loss SLIRP but has
no retransmission queue, RTT estimation, out-of-order reassembly, delayed/
duplicate ACK handling, or TIME-WAIT. Add those (and a real send buffer) before
trusting it on a lossy link. Then verify against a real host once outbound
connectivity is available.

**2. Proper CSPRNG.** ChaCha20 DRBG (or Fortuna) over real entropy — RDRAND,
timing jitter, virtio-rng — replacing `krandom` for TCP ISNs / TLS. (`krandom`
stays fine for boot entropy / `AT_RANDOM`.) TCP ISNs are currently a fixed
counter; predictable.

**3. Per-CPU run queues + work-stealing.** Today one global run queue under the
`sched_lock` baton serves every core (an idle CPU pulls any ready task — implicit
work-stealing, but the single lock is the scalability ceiling). Split into
per-CPU queues with explicit stealing once contention matters.

**4. Userspace quality-of-life.** `chdir`/cwd, symlinks, `FD_CLOEXEC`; a tiny
`/proc` (pid dirs + `stat`) and `/dev` (`null`/`zero`/`tty`/`random`); a real
(small) init that respawns the shell; a package format (tar + VibeFS metadata).

**5. Toolchain integration.** A cross target `x86_64-vibeos-musl` + a sysroot, so
`gcc`/`clang` build for VibeOS directly instead of repurposing host musl.

**Also widening the ABI toward busybox/binutils** as opportunity allows (more
`stat`/`fcntl` variants, `clock_gettime`, `getrandom`, `uname`, `getsockname`/
`setsockopt` beyond stubs); PIE load base is fixed (no ASLR yet).

*Known issue:* unclean-`fsck` drops `/bin/init` on diskutil volumes (workaround =
clean image per build); root-cause before relying on crash persistence. Other
backlog: ACPI poweroff; FS journaling/`rename`/permissions.

## Additional features in scope, high level
** a /config directory with a simple format (yaml) for build and runtime configuration.
** kernel services for reading, parsing, and reloading these config files
** A more fully-featured init system with a simple service definition format, dependency management, and logging.
** Simple windowing system for the framebuffer (in /gui )
** USB (EHCI/XHCI) + input drivers (keyboard/mouse) for a graphical UI.
** Audio subsystem + virtio-sound.
---

## How it boots

```
UEFI/OVMF → BOOTX64.EFI → kernel.elf → start.S (bootstrap tables, jump high) → kmain()
  → gdt/idt/percpu(TSS+GS)/syscall → pmm → paging (drop identity) → page refcounts
  → apic+timer → virtio-blk → fs_mount → irq_enable → sched_init → net_init
  (virtio-net + worker) → create init+workers → smp_init (APs join: percpu+SYSCALL+SSE)
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
