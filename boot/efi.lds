/*
 * Linker script for building a UEFI application as an ELF DSO that is
 * then converted to PE32+ via objcopy. Modelled after the gnu-efi
 * elf_x86_64_efi.lds. Keeps sections compact and provides the .reloc
 * section that objcopy needs to emit a proper PE base-relocation
 * directory.
 */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = 0;
    ImageBase = .;

    .hash         : { *(.hash) }
    .gnu.hash     : { *(.gnu.hash) }

    . = ALIGN(4096);
    .text         : {
        _text = .;
        *(.text .text.*)
        . = ALIGN(16);
    }
    _etext = .;

    . = ALIGN(4096);
    .rodata       : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data         : {
        _data = .;
        *(.data .data.*)
        *(.got .got.plt)
        *(.sdata .sdata.*)
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    }
    _edata = .;

    . = ALIGN(4096);
    .dynamic      : { *(.dynamic) }
    . = ALIGN(4096);
    .rela         : { *(.rela .rela.*) }
    . = ALIGN(4096);
    .dynsym       : { *(.dynsym) }
    . = ALIGN(4096);
    .dynstr       : { *(.dynstr) }
    . = ALIGN(4096);
    .reloc        : { *(.reloc) }

    /DISCARD/ : {
        *(.note.*)
        *(.comment)
        *(.eh_frame)
        *(.gnu.linkonce.*)
        *(.interp)
    }
}
