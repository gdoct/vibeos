# VibeOS

*A from-scratch x86-64 operating system — UEFI boot, a higher-half SMP kernel, a
writable filesystem, a WAN-grade TCP/IP stack, and a userspace windowing system —
that runs **unmodified `x86_64-linux-musl` binaries**.*

![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)
![Language](https://img.shields.io/badge/language-C%2B%2B17%20%2F%20asm-orange.svg)
![Platform](https://img.shields.io/badge/platform-x86__64%20UEFI-lightgrey.svg)
![Status](https://img.shields.io/badge/status-archived-inactive.svg)

> ### 🧪 About this project
> VibeOS was **vibe coded in about a week of evening sessions** (~100 commits,
> May–June 2026) and active development has since stopped. It was an experiment:
> *how far can current frontier models be pushed with expert guidance?* Every
> feature below — the paging code, the scheduler, the TCP stack, the compositor —
> was written by a model and verified end-to-end on real boots. The answer, it
> turns out, is "surprisingly far."

<img width="1211" height="598" alt="VibeOS booting in QEMU" src="https://github.com/user-attachments/assets/b56c282c-477a-4776-b14e-d45ba2580cc5" />

## Overview

VibeOS boots over a UEFI chain into a higher-half kernel with its own paging, an
SMP scheduler with per-CPU run queues and work-stealing, a writable on-disk
filesystem (VibeFS), a WAN-grade IPv4/TCP network stack, and a userspace built on
musl. The current tree boots end-to-end in QEMU + OVMF, mounts a VibeFS volume,
loads `/bin/init` from disk, and brings up an interactive shell and a graphical
desktop.

It is implemented from scratch in C++ (C-style, freestanding) and assembly, with
a .NET 8 host tool for creating and populating VibeFS disk images. Crucially, the
kernel follows the **Linux x86_64 syscall ABI**, so cross-compiled
`x86_64-linux-musl` programs — static *and* dynamically linked (it ships
`ld-musl.so` and loads PIEs via `PT_INTERP`) — run unmodified, with no translation
layer, as long as they only touch syscalls that are implemented. The kernel,
userspace, and bootloader build with `g++` and GNU `ld` (the bootloader is a UEFI
PE32+ application).

[ROADMAP.md](docs/ROADMAP.md) is the source of truth for project state and design
rationale. The project is licensed under the [MIT License](LICENSE).

<img width="1281" height="1024" alt="VibeOS desktop" src="https://github.com/user-attachments/assets/a4bf7a0c-d8d8-46fd-b108-c77e1d883f0c" />

## Highlights

- 🐧 **Runs real Linux/musl binaries** — Linux syscall numbering from day one, so
  cross-compiled static and dynamically-linked musl programs run unmodified.
- ⚙️ **True SMP** — per-CPU run queues with work-stealing; both kernel *and* user
  tasks run on every core, with the ring-3 paths made safe by atomic refcounts,
  per-fdtable / per-vmspace spinlocks, and cross-core TLB shootdown.
- 🌐 **A network stack, not a toy** — ARP/IP/ICMP/UDP and TCP with congestion
  control, retransmission, and reassembly, behind BSD sockets — enough to `wget`
  over the wire.
- 🪟 **Userspace windowing** — a `guiwm` compositor over mmap'd `/dev/fb0` +
  `/dev/input`, with one-process-per-window client apps talking over loopback TCP.
- 🔒 **Modern hardening** — copy-on-write `fork`, validated user/kernel copies,
  POSIX signals, threads (`clone`/`futex` → musl pthreads), and per-`execve` ASLR
  seeded from a ChaCha20 CSPRNG.

## What is implemented

The major pieces available today (see [ROADMAP.md](docs/ROADMAP.md) for the full
status):

- **Boot** — a UEFI bootloader that loads `kernel.elf` from the ESP, gathers GOP,
  ACPI, and the UEFI memory map, then hands off a `BootInfo` struct.
- **Kernel core** — higher-half kernel with its own paging and direct map (the low
  half is left to userspace); per-CPU GDT/TSS, IDT, exception handling, SYSCALL /
  SYSRET, and APIC-based interrupts.
- **SMP & scheduling** — per-CPU run queues with work-stealing, per-CPU TSS + GS
  base with `swapgs` on kernel entry, cross-CPU IPIs, and TLB shootdown; a
  preemptive scheduler with blocking sleep and wait queues. The ring-3 syscall /
  fd-table / pipe / address-space paths carry their own locks so user threads are
  safe to steal across cores.
- **Memory** — physical memory management and a slab-style `kmalloc` / `kfree`;
  copy-on-write `fork` with per-page refcounts and validated user/kernel copies.
- **Processes, signals & threads** — per-process address spaces and cwd, `fork`,
  `execve`, `wait4`; POSIX signals (handlers, blocked/pending masks, default
  actions, `kill` / `sigaltstack` / `sigreturn`, CPU faults turned into signals);
  threads via `clone` / `futex` over a refcounted address space + fd table,
  running unmodified musl pthreads.
- **ASLR & randomness** — per-`execve` ASLR (PIE image, dynamic linker, stack,
  mmap arena) backed by a ChaCha20 CSPRNG seeded from RDRAND, RDTSC jitter, and
  virtio-rng.
- **Devices** — RAM disk, virtio-blk, virtio-net, virtio-rng, and PCI enumeration;
  a UHCI USB host-controller driver enumerating a keyboard and mouse for the
  console and GUI.
- **Graphics** — a userspace windowing system (`guiwm` over mmap'd `/dev/fb0` +
  `/dev/input`, with one-process-per-window clients over loopback TCP), plus a
  legacy in-kernel compositor; framebuffer graphics, serial logging, and a basic
  text console.
- **Networking** — a WAN-grade IPv4 stack (ARP, IP, ICMP, UDP, and TCP with
  congestion control, retransmission, and reassembly) behind BSD sockets
  (`socket` / `bind` / `listen` / `accept` / `connect` / `sendto` / `recvfrom`,
  `O_NONBLOCK` / `MSG_DONTWAIT`), with a ported `wget`.
- **I/O multiplexing** — a unified readiness layer for `poll` / `select` /
  `pselect6` / `ppoll` across files, ttys, pipes, and sockets, plus `eventfd` and
  `timerfd`.
- **Time & credentials** — real wall-clock time from the CMOS RTC anchored to the
  timer tick: `clock_gettime` (REALTIME + MONOTONIC) / `gettimeofday` and
  wall-clock `stat` timestamps; hardcoded-root credentials (`getuid` / `geteuid` /
  `getgid` / `getegid`).
- **Filesystem** — VibeFS, a small writable filesystem with directories, files,
  symlinks, and crash-safe ordered updates, with in-place mutation (`unlink` /
  `unlinkat` / `rmdir` / `rename` / `truncate` / `ftruncate`).
- **Userspace & init** — userspace loading from disk (static and dynamically-linked
  musl), an interactive `/bin/sh` (with `mkdir` / `touch` / `rm` / `rmdir` / `mv` /
  `id` builtins), a `/config`-driven service-managed init (PID 1) that starts and
  supervises services from `/config/services/`, and an `x86_64-vibeos-musl` cross
  toolchain for building target binaries.

<img width="1440" height="1304" alt="VibeOS apps" src="https://github.com/user-attachments/assets/97be2029-1b5b-4897-a5d0-7a61534f7aa7" />

## Repository layout

| Path | Contents |
| --- | --- |
| [boot/](boot/) | UEFI bootloader and ESP image builder. |
| [kernel/](kernel/) | Kernel, drivers, memory management, scheduler, filesystem, and userspace support. |
| [gui/](gui/) | The graphical stack: [gui/client/](gui/client/) is the userspace windowing system (`guiwm` + per-window client apps); [gui/core/](gui/core/) is the legacy in-kernel compositor. |
| [user/](user/) | Userspace programs: the freestanding `init` / `sh` / `hello`, plus static and dynamic musl test binaries under [user/musl/](user/musl/) (`sigtest`, `cputest`, `nettest`, `wget`, `dynhello`, …). |
| [interop/tools/diskutil/](interop/tools/diskutil/) | Host-side .NET tooling for creating and populating VibeFS volumes. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Feature status, design choices, and planned follow-ups. |

## Boot flow

1. UEFI firmware loads `BOOTX64.EFI` from the EFI System Partition.
2. The bootloader reads `\vibeos\kernel.elf` from the FAT image.
3. The kernel is loaded at its physical target, the framebuffer and memory map
   are collected, and `BootInfo` is passed to the kernel.
4. The kernel initializes paging, interrupts, memory management, devices, and the
   filesystem.
5. The kernel mounts the VibeFS volume and launches `/bin/init` from disk.
6. `init` (a service-managed PID 1) reads `/config/services/`, brings up the
   userspace window manager (`/bin/guiwm`) and `/bin/sh`, and supervises them —
   giving an interactive shell over the serial console and a desktop on the
   framebuffer.

## Building

The top-level build uses GNU `make`, `g++`, `ld`, `mtools`, and
`qemu-system-x86_64` with OVMF. The host disk utility is a .NET 8 tool.

```bash
make
```

That builds the bootloader, kernel, and userspace binaries.

To create a fresh bootable image and populate a VibeFS volume with the userspace
programs:

```bash
./build.sh          # default 2G data disk
./build.sh 4G       # custom VibeFS volume size
```

To update the existing images in place instead of rebuilding them from scratch:

```bash
./update-kernel.sh          # rebuild kernel.elf and replace it in boot/build/vibeos.img
./update-system.sh          # rebuild kernel + userspace and refresh boot/build/{vibeos,vdisk}.img
./merge-package.sh doom     # build one package archive and merge it into boot/build/vdisk.img
```

## Running

Boot the image in QEMU with OVMF, the attached VibeFS data disk, and a virtio-net
NIC (QEMU user/SLIRP networking):

```bash
make run
```

The kernel logs to serial, so QEMU's stdio is the primary console. At the shell,
try the bundled test binaries — e.g. `mhello`, `sigtest`, `nettest`, or
`wget http://localhost/` (fetched over the in-guest TCP/IP stack).

## Notes

- `boot/build/vibeos.img` is the FAT ESP image; `boot/build/vdisk.img` is the
  virtio-blk VibeFS volume used by the kernel.
- To inspect or modify the filesystem image from the host, use the disk utility
  under [interop/tools/diskutil/](interop/tools/diskutil/).
- For the detailed implementation sequence, design choices, and deferred work,
  read [ROADMAP.md](docs/ROADMAP.md).
