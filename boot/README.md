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

## What Boot Does Not Do

- launch user programs
- start services or sessions
- run the shell or any UI
- set up paging beyond what UEFI already provides (identity-mapped low memory)
- set up its own IDT/GDT (the kernel will replace them)

Those responsibilities belong to later kernel and userspace stages.

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

## Implementation Plan

### Phase 1 — Skeleton UEFI application

Stand up the build and a do-nothing `efi_main` that prints "VibeOS boot" and waits for a key. This proves the toolchain (`clang -target x86_64-unknown-windows` + `lld-link /subsystem:efi_application`), the ESP layout, and the QEMU+OVMF run loop.

### Phase 2 — Read the kernel from the ESP

Use `EFI_LOADED_IMAGE_PROTOCOL` → `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` to open `\vibeos\kernel.elf`. Read it into a transient `AllocatePool` buffer. Confirm the file is reachable and the size is sane.

### Phase 3 — Parse and load the ELF

Validate `e_ident` (magic, ELFCLASS64, ELFDATA2LSB, EM_X86_64, ET_EXEC). Walk the program headers; for each `PT_LOAD` segment, `AllocatePages(AllocateAddress, …)` at `p_paddr`, copy `p_filesz` bytes from the file image, and zero the BSS tail (`p_memsz - p_filesz`). Record the lowest/highest physical pages used so we can hand the range to the kernel.

### Phase 4 — Boot info: framebuffer + ACPI + memory map

- **GOP**: `LocateProtocol(EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, …)`, prefer a 32-bit BGR/RGB linear mode at the highest available resolution within reason, record base+pitch+width+height+format.
- **RSDP**: scan the EFI configuration table for `ACPI_20_TABLE_GUID` (fall back to `ACPI_TABLE_GUID`).
- **Memory map**: `GetMemoryMap` twice — once to size the buffer, once for real — write the descriptors into a page allocation that survives `ExitBootServices`.

### Phase 5 — ExitBootServices and jump

Call `ExitBootServices(ImageHandle, MapKey)`. If it fails with `EFI_INVALID_PARAMETER`, refresh the memory map once and retry (UEFI spec mandates this dance). After success, disable interrupts, switch to the bootloader-allocated stack, and `call` the ELF entry point with `rdi = &BootInfo`.

### Phase 6 — Reliability

Once the happy path works, add:

- error reporting through `ConOut` with file:line context before any `return EFI_*`
- a serial log fallback (`0x3F8` 16550 UART) for headless debugging — useful even before `ExitBootServices`
- sanity checks on the ELF: entry inside a loaded segment, no segments overlapping bootloader memory, BootInfo magic verified

### Phase 7 — Done

At this point the bootloader is "frozen": further startup work moves into the kernel. The only future changes to the bootloader should be additions to the `BootInfo` struct (with a `version` bump) when the kernel needs new firmware-time information.

## Building and Running

```
make           # builds build/BOOTX64.EFI and build/vibeos.img
make run       # qemu-system-x86_64 + OVMF, boots the image
```

Required tools: `clang` (≥ 14), `lld` (`lld-link`), `qemu-system-x86_64`, `OVMF` firmware, `mtools` (`mformat`/`mcopy`) for building the FAT image without root.
