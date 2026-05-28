/*
 * MyOS kernel link script.
 *
 * The UEFI bootloader loads PT_LOAD segments at their p_paddr. We anchor
 * the kernel at 1 MiB — the lowest address that's reliably outside the
 * legacy real-mode and EBDA regions while still being below 16 MiB
 * (where AllocatePages will always find conventional memory).
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_PHYS = 0x100000;

SECTIONS {
    . = KERNEL_PHYS;
    _kernel_start = .;

    .text : ALIGN(0x1000) {
        *(.text._start)
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        _bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(0x1000);
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
        *(.dynamic)
        *(.dynsym)
        *(.dynstr)
        *(.hash)
        *(.gnu.hash)
        *(.gnu.version*)
        *(.interp)
    }
}
