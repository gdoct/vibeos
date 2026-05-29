# VibeOS

VibeOS is a small x86_64 operating system built around a UEFI boot
chain, a higher-half kernel, a writable on-disk filesystem, and a
serial-backed userspace. The current tree boots end-to-end in QEMU + OVMF, 
mounts a VibeFS volume, loads `/bin/init` from disk, and runs a shell in userspace.

VibeOS is implemented from scratch in C and assembly, with a .NET 8 host tool for creating and populating VibeFS disk images. The kernel is Linux-compatible at the syscall level, so it can run simple Linux applications that don't depend on specific hardware features or complex syscalls. The kernel and userspace are built with Clang targeting x86_64, and the bootloader is a UEFI application linked with LLD. 

All features were vibe coded, and the project is licensed under MIT.

## What is implemented

The roadmap in [ROADMAP.md](ROADMAP.md) is the source of truth for the
project state. The major pieces that are available today are:

- UEFI bootloader that loads `kernel.elf` from the ESP, gathers GOP,
	ACPI, and the UEFI memory map, then hands off a `BootInfo` struct.
- Higher-half kernel with its own paging setup, direct map, and the low
	half left free for userspace.
- Core x86_64 plumbing: GDT, IDT, TSS, exception handling, SYSCALL /
	SYSRET, and APIC-based interrupt handling.
- SMP support is implemented.
- Preemptive round-robin scheduler with blocking sleep, wait queues, and
	an idle task.
- Physical memory management and a slab-style `kmalloc` / `kfree`.
- Framebuffer graphics, serial logging, and a basic text console.
- RAM disk and virtio-blk drivers, plus PCI enumeration.
- VibeFS, a small writable filesystem with directories, files, and
	crash-safe ordered updates.
- Userspace loading from disk, per-process address spaces, `fork`,
	`execve`, `wait4`, and an interactive `/bin/sh`.

## Repository Layout

- [boot/](boot/) - UEFI bootloader and ESP image builder.
- [kernel/](kernel/) - kernel, drivers, memory management, scheduler,
	filesystem, and userspace support.
- [user/](user/) - static userspace programs such as `init`, `hello`,
	and `sh`.
- [interop/tools/diskutil/](interop/tools/diskutil/) - host-side .NET
	tooling for creating and populating VibeFS volumes.
- [ROADMAP.md](ROADMAP.md) - feature status and planned follow-ups.

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

Boot the image in QEMU with OVMF and the attached VibeFS data disk:

```bash
make run
```

The kernel logs to serial, so QEMU's stdio is the primary console.

## Notes

- `boot/build/vibeos.img` is the FAT ESP image.
- `boot/build/vdisk.img` is the virtio-blk VibeFS volume used by the
	kernel.
- If you want to inspect or modify the filesystem image from the host,
	use the disk utility under [interop/tools/diskutil/](interop/tools/diskutil/).
- For the detailed implementation sequence and deferred work, read
	[ROADMAP.md](ROADMAP.md).
