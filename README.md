# VibeOS
<img width="1211" height="598" alt="image" src="https://github.com/user-attachments/assets/b56c282c-477a-4776-b14e-d45ba2580cc5" />


VibeOS is a small x86_64 operating system built around a UEFI boot
chain, a higher-half kernel, an SMP scheduler that runs user processes on every
core, a writable on-disk filesystem, an IPv4/TCP network stack, and a
serial-backed userspace. The current tree boots end-to-end in QEMU + OVMF,
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

The roadmap in [ROADMAP.md](ROADMAP.md) is the source of truth for the
project state. The major pieces that are available today are:

- UEFI bootloader that loads `kernel.elf` from the ESP, gathers GOP,
	ACPI, and the UEFI memory map, then hands off a `BootInfo` struct.
- Higher-half kernel with its own paging setup, direct map, and the low
	half left free for userspace.
- Core x86_64 plumbing: per-CPU GDT/TSS, IDT, exception handling, SYSCALL /
	SYSRET, and APIC-based interrupt handling.
- SMP with user tasks scheduled on every core — per-CPU TSS + GS base with
	`swapgs` on kernel entry, cross-CPU IPIs, and TLB shootdown.
- Preemptive scheduler with blocking sleep and wait queues.
- Physical memory management and a slab-style `kmalloc` / `kfree`;
	copy-on-write `fork` with per-page refcounts and validated user/kernel copies.
- POSIX signals: handlers, blocked/pending masks, default actions,
	`kill`/`sigaltstack`/`sigreturn`; CPU faults are turned into signals.
- Framebuffer graphics, serial logging, and a basic text console.
- RAM disk and virtio-blk drivers, virtio-net, plus PCI enumeration.
- A compact IPv4 network stack (ARP, IP, ICMP, UDP, TCP) with BSD sockets
	(`socket`/`bind`/`listen`/`accept`/`connect`/`sendto`/`recvfrom`/`poll`) and a
	ported `wget`.
- VibeFS, a small writable filesystem with directories, files, and
	crash-safe ordered updates.
- Userspace loading from disk (static and dynamically-linked musl), per-process
	address spaces, `fork`, `execve`, `wait4`, and an interactive `/bin/sh`.

<img width="1440" height="1304" alt="image" src="https://github.com/user-attachments/assets/97be2029-1b5b-4897-a5d0-7a61534f7aa7" />

## Repository Layout

- [boot/](boot/) - UEFI bootloader and ESP image builder.
- [kernel/](kernel/) - kernel, drivers, memory management, scheduler,
	filesystem, and userspace support.
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
6. `init` execs `/bin/sh`, giving an interactive shell over the serial
	 console.

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
