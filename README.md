# VibeOS
<img width="1211" height="598" alt="image" src="https://github.com/user-attachments/assets/b56c282c-477a-4776-b14e-d45ba2580cc5" />


VibeOS is a small x86_64 operating system built around a UEFI boot
chain, a higher-half kernel, an SMP scheduler with per-CPU run queues and
work-stealing, a writable on-disk filesystem, a WAN-grade IPv4/TCP network stack,
and a serial-backed userspace. The current tree boots end-to-end in QEMU + OVMF,
mounts a VibeFS volume, loads `/bin/init` from disk, and runs a shell in userspace.

VibeOS is implemented from scratch in C++ (C-style, freestanding) and assembly,
with a .NET 8 host tool for creating and populating VibeFS disk images. The
kernel follows the Linux x86_64 syscall ABI, so it runs unmodified
`x86_64-linux-musl` binaries — both static and dynamically linked (it ships
`ld-musl.so` and loads PIEs via `PT_INTERP`) — as long as they don't depend on
syscalls it hasn't implemented yet. The kernel, userspace, and bootloader are
built with `g++` and GNU `ld` (the bootloader is a UEFI PE32+ application).

All features were vibe coded, and the project is licensed under MIT.

<img width="1277" height="1023" alt="image" src="https://github.com/user-attachments/assets/f3e462ce-f0a7-4d82-bfed-bc4252b58ea9" />

## What is implemented

The roadmap in [ROADMAP.md](docs/ROADMAP.md) is the source of truth for the
project state. The major pieces that are available today are:

- UEFI bootloader that loads `kernel.elf` from the ESP, gathers GOP,
	ACPI, and the UEFI memory map, then hands off a `BootInfo` struct.
- Higher-half kernel with its own paging setup, direct map, and the low
	half left free for userspace.
- Core x86_64 plumbing: per-CPU GDT/TSS, IDT, exception handling, SYSCALL /
	SYSRET, and APIC-based interrupt handling.
- SMP with per-CPU run queues and work-stealing — per-CPU TSS + GS base with
	`swapgs` on kernel entry, cross-CPU IPIs, and TLB shootdown. Both kernel and
	user tasks run on every core; the ring-3 syscall / fd-table / pipe / address-
	space paths carry their own locks (atomic refcounts, per-fdtable and
	per-vmspace spinlocks, cross-core TLB shootdown) so user threads are safe to
	steal across cores.
- Preemptive scheduler with blocking sleep and wait queues.
- Physical memory management and a slab-style `kmalloc` / `kfree`;
	copy-on-write `fork` with per-page refcounts and validated user/kernel copies.
- POSIX signals: handlers, blocked/pending masks, default actions,
	`kill`/`sigaltstack`/`sigreturn`; CPU faults are turned into signals.
- Threads: `clone`/`futex` over a refcounted address space + fd table, running
	unmodified musl pthreads.
- ASLR per-`execve` (PIE image, dynamic linker, stack, mmap arena) backed by a
	ChaCha20 CSPRNG seeded from RDRAND, RDTSC jitter, and virtio-rng.
- USB input: a UHCI host-controller driver enumerating a USB keyboard and mouse
	for the console and GUI.
- A graphical stack: a userspace windowing system (`guiwm` over mmap'd
	`/dev/fb0` + `/dev/input`, with one-process-per-window clients over loopback
	TCP), plus a legacy in-kernel compositor. Framebuffer graphics, serial
	logging, and a basic text console.
- RAM disk and virtio-blk drivers, virtio-net, plus PCI enumeration.
- A WAN-grade IPv4 network stack (ARP, IP, ICMP, UDP, and TCP with congestion
	control, retransmission, and reassembly) with BSD sockets
	(`socket`/`bind`/`listen`/`accept`/`connect`/`sendto`/`recvfrom`,
	`O_NONBLOCK`/`MSG_DONTWAIT`) and a ported `wget`.
- I/O multiplexing over a unified readiness layer: `poll`/`select`/`pselect6`/
	`ppoll` across files, ttys, pipes, sockets, plus `eventfd` and `timerfd`.
- Real wall-clock time from the CMOS RTC, anchored to the timer tick:
	`clock_gettime` (REALTIME + MONOTONIC) / `gettimeofday` and wall-clock `stat`
	timestamps. Hardcoded-root credentials (`getuid`/`geteuid`/`getgid`/`getegid`).
- VibeFS, a small writable filesystem with directories, files, symlinks, and
	crash-safe ordered updates, with in-place mutation (`unlink`/`unlinkat`/
	`rmdir`/`rename`/`truncate`/`ftruncate`).
- Userspace loading from disk (static and dynamically-linked musl), per-process
	address spaces and cwd, `fork`, `execve`, `wait4`, and an interactive
	`/bin/sh` (with `mkdir`/`touch`/`rm`/`rmdir`/`mv`/`id` builtins).
- A `/config`-driven, service-managed init (PID 1) that starts and supervises
	services from `/config/services/`, plus an `x86_64-vibeos-musl` cross
	toolchain for building target binaries.

<img width="1440" height="1304" alt="image" src="https://github.com/user-attachments/assets/97be2029-1b5b-4897-a5d0-7a61534f7aa7" />

## Repository Layout

- [boot/](boot/) - UEFI bootloader and ESP image builder.
- [kernel/](kernel/) - kernel, drivers, memory management, scheduler,
	filesystem, and userspace support.
- [gui/](gui/) - the graphical stack: [gui/client/](gui/client/) is the
	userspace windowing system (`guiwm` + per-window client apps);
	[gui/core/](gui/core/) is the legacy in-kernel compositor.
- [user/](user/) - userspace programs: the freestanding `init`/`sh`/`hello`,
	plus static and dynamic musl test binaries under [user/musl/](user/musl/)
	(`sigtest`, `cputest`, `nettest`, `wget`, `dynhello`, …).
- [interop/tools/diskutil/](interop/tools/diskutil/) - host-side .NET
	tooling for creating and populating VibeFS volumes.
- [ROADMAP.md](docs/ROADMAP.md) - feature status and planned follow-ups.

## Boot Flow

1. UEFI firmware loads `BOOTX64.EFI` from the EFI System Partition.
2. The bootloader reads `\\vibeos\\kernel.elf` from the FAT image.
3. The kernel is loaded at its physical target, the framebuffer and
	 memory map are collected, and `BootInfo` is passed to the kernel.
4. The kernel initializes paging, interrupts, memory management,
	 devices, and the filesystem.
5. The kernel mounts the VibeFS volume and launches `/bin/init` from
	 disk.
6. `init` (a service-managed PID 1) reads `/config/services/`, brings up the
	 userspace window manager (`/bin/guiwm`) and `/bin/sh`, and supervises them —
	 giving an interactive shell over the serial console.

## Building

The top-level build uses GNU `make`, `g++`, `ld`, `mtools`, and
`qemu-system-x86_64` with OVMF. The host disk utility is a .NET 8 tool.

```bash
make
```

That builds the bootloader, kernel, and userspace binaries.

To create a fresh bootable image and populate a VibeFS volume with the
userspace programs:

```bash
./build.sh
```

You can pass a custom volume size if you want a different VibeFS disk:

```bash
./build.sh 4G
```

## Running

Boot the image in QEMU with OVMF, the attached VibeFS data disk, and a
virtio-net NIC (QEMU user/SLIRP networking):

```bash
make run
```

The kernel logs to serial, so QEMU's stdio is the primary console. At the shell,
try the bundled test binaries — e.g. `mhello`, `sigtest`, `nettest`, or
`wget http://localhost/` (fetched over the in-guest TCP/IP stack).

## Notes

- `boot/build/vibeos.img` is the FAT ESP image.
- `boot/build/vdisk.img` is the virtio-blk VibeFS volume used by the
	kernel.
- If you want to inspect or modify the filesystem image from the host,
	use the disk utility under [interop/tools/diskutil/](interop/tools/diskutil/).
- For the detailed implementation sequence and deferred work, read
	[ROADMAP.md](docs/ROADMAP.md).
