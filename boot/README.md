# Boot

This part contains the boot phase of VibeOS.

The boot phase is responsible for getting the system from power-on to a running kernel. Its job is not to start services or applications. Its job is to prepare the machine, load the kernel into memory, and transfer control to the kernel entry point.

## Target

The boot target is fixed for VibeOS:

- **Firmware**: UEFI (no legacy BIOS / no MBR / no GRUB)
- **CPU**: x86_64, long mode (UEFI already places us here)
- **Kernel format**: ELF64, statically linked, position-fixed at a known virtual address
- **Bootloader format**: PE32+ EFI application (`BOOTX64.EFI`)

The bootloader is itself a UEFI application loaded by the firmware from the EFI System Partition (ESP) at the standard fallback path `\EFI\BOOT\BOOTX64.EFI`. The kernel lives on the same FAT partition as `\vibeos\kernel.elf`.

## Scope

The boot phase does only the minimum required to start the kernel:

- run as a UEFI application from the firmware boot path
- locate `\vibeos\kernel.elf` on the ESP
- read and validate its ELF64 header
- allocate physical memory and load each `PT_LOAD` segment at its requested physical address
- pull together the boot info the kernel needs (framebuffer, memory map, ACPI RSDP)
- call `ExitBootServices` and jump to the kernel entry point

## Kernel Handoff Contract

Before jumping to the kernel, the bootloader guarantees:

- **CPU mode**: x86_64 long mode, CPL 0, interrupts disabled
- **Paging**: UEFI's identity mapping is still active; kernel must build its own page tables before relying on higher-half addresses
- **Stack**: a 64 KiB stack allocated by the bootloader, 16-byte aligned
- **Entry**: `e_entry` from the kernel ELF header, called as `void kmain(BootInfo *info)` using the SysV AMD64 ABI (`info` in `rdi`)
- **Boot services**: already exited; runtime services pointer is in `BootInfo`
- **BootInfo struct**: layout defined in `include/bootinfo.h`, contains framebuffer, memory map, RSDP, kernel image range

The `BootInfo` struct is versioned with a magic value and `version` field so kernel and bootloader can detect drift.

## Layout

```
01 BOOT/
├── README.md          (this file)
├── Makefile           (build BOOTX64.EFI + a bootable disk image)
├── include/
│   ├── efi.h          (minimal UEFI types, GUIDs, protocols)
│   ├── elf.h          (ELF64 structures and constants)
│   └── bootinfo.h     (kernel handoff struct — shared with kernel)
└── src/
    ├── main.c         (efi_main, top-level boot flow)
    ├── elf.c          (ELF64 validation and segment loading)
    ├── fs.c           (read a file from the ESP)
    ├── gop.c          (acquire the framebuffer)
    ├── mmap.c         (memory map + ExitBootServices)
    └── util.c         (print, memset, memcpy, panic)
```

## Building and Running

```
make           # builds build/BOOTX64.EFI and build/vibeos.img
make run       # qemu-system-x86_64 + OVMF, boots the image
```

Required tools: `clang` (≥ 14), `lld` (`lld-link`), `qemu-system-x86_64`, `OVMF` firmware, `mtools` (`mformat`/`mcopy`) for building the FAT image without root.
