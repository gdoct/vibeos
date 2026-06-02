/*
 * Dummy .reloc section so objcopy → PE32+ produces a valid base
 * relocation directory. OVMF (and Tianocore) require an image to have
 * a relocation table even if it is fully RIP-relative — without one
 * the LoadImage path returns EFI_UNSUPPORTED.
 *
 * Layout per PE/COFF spec for a base relocation block:
 *   uint32_t page_rva
 *   uint32_t block_size  (in bytes, including this header)
 *   uint16_t entries[]   (type<<12 | offset; type 0 = ABSOLUTE = no-op)
 */

    .section .reloc, "a", @progbits
    .balign 4
    .long   0           /* page RVA */
    .long   12          /* block size: 8 (header) + 2 entries × 2 bytes */
    .word   0           /* type=ABSOLUTE, offset=0 (ignored at runtime) */
    .word   0           /* padding entry to keep block size 4-aligned */

   .section .note.GNU-stack,"",@progbits
