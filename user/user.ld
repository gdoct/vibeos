/*
 * Userspace link script (ROADMAP §3, Phase 5A).
 *
 * Static ET_EXEC linked at 4 MiB — comfortably inside the low half the kernel
 * now leaves free (Phase 0). Page-aligned segments so the loader maps clean
 * 4 KiB pages. (§4's Linux binaries will use their own base, typically the
 * same 0x400000; nothing here is VibeOS-specific except the value.)
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text : ALIGN(0x1000) { *(.text._start) *(.text .text.*) }
    .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) }
    .data : ALIGN(0x1000) { *(.data .data.*) }
    .bss : ALIGN(0x1000) { *(.bss .bss.*) *(COMMON) }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}
