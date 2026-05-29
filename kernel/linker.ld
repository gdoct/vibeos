/*
 * VibeOS kernel link script — higher-half kernel (ROADMAP §3, Phase 0).
 *
 * The kernel runs at -2 GiB (0xFFFFFFFF80000000 + physical), so the entire
 * low canonical half is free for userspace. The bootloader, however, still
 * loads PT_LOAD segments at their p_paddr and jumps to e_entry under UEFI
 * identity paging — so the entry point must stay at a LOW address.
 *
 * We solve that by keeping a small `.boot` section linked LOW (VMA == LMA ==
 * physical) that holds _start; everything else is linked HIGH with AT() so
 * its load address (p_paddr) lands contiguously in low physical RAM. _start
 * builds bootstrap page tables, switches to the high half, and calls kmain;
 * paging_init then builds the real tables and drops the low identity map.
 *
 * Because _start is in .boot, e_entry == its low physical address, which
 * sits inside the bootloader's [p_paddr_lo, p_paddr_hi) range — so the
 * bootloader needs no change.
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

KERNEL_PHYS  = 0x100000;              /* lowest physical page of the image */
KERNEL_VBASE = 0xFFFFFFFF80000000;    /* -2 GiB; matches -mcmodel=kernel    */

SECTIONS {
    . = KERNEL_PHYS;
    _kernel_phys_base = .;

    /* Low trampoline: identity VMA == LMA == physical. Holds _start and the
       bootstrap page-table scratch (emitted zeroed; _start fills them). */
    .boot ALIGN(0x1000) : {
        *(.boot.text)
        *(.boot.data)
    }
    . = ALIGN(0x1000);
    _boot_phys_end = .;

    /* Shift the location counter into the high half. From here every output
       section has VMA = KERNEL_VBASE + phys and LMA = phys (via AT). */
    . += KERNEL_VBASE;

    .text ALIGN(0x1000) : AT(ADDR(.text) - KERNEL_VBASE) {
        *(.text._start_high)
        *(.text .text.*)
    }

    .rodata ALIGN(0x1000) : AT(ADDR(.rodata) - KERNEL_VBASE) {
        *(.rodata .rodata.*)
    }

    .data ALIGN(0x1000) : AT(ADDR(.data) - KERNEL_VBASE) {
        *(.data .data.*)
    }

    .bss ALIGN(0x1000) : AT(ADDR(.bss) - KERNEL_VBASE) {
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
