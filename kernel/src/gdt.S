/*
 * Minimal 64-bit GDT.
 *
 * We install our own instead of relying on UEFI's because the firmware
 * GDT lives in memory we don't track and could be reclaimed. Three
 * descriptors are enough for a kernel-only system: null, kernel code,
 * kernel data. Selectors: KERNEL_CS=0x08, KERNEL_DS=0x10.
 *
 * Descriptor encoding (8 bytes, low → high):
 *   limit[15:0] base[15:0] base[23:16] access flags|limit[19:16] base[31:24]
 *
 * Code:  access=0x9A (P=1,DPL=0,S=1,Type=Exec/Read accessed=0,
 *                    we set Exec/Read = 0xA, plus P|S = 0x9A)
 *        flags=0xAF (G=1, L=1 long mode, limit hi=0xF)
 * Data:  access=0x92 (P=1,DPL=0,S=1,Type=RW = 0x2)
 *        flags=0xCF (G=1, D=1, limit hi=0xF)
 */

    .section .rodata
    .balign 16
gdt_start:
    .quad 0x0000000000000000        /* null */
    .quad 0x00AF9A000000FFFF        /* kernel CS: L=1 64-bit code */
    .quad 0x00CF92000000FFFF        /* kernel DS */
gdt_end:

    .section .data
    .balign 8
    .globl  gdt_ptr
gdt_ptr:
    .word   gdt_end - gdt_start - 1
    .quad   gdt_start

    .section .text
    .globl  gdt_init
    .type   gdt_init, @function
gdt_init:
    lgdt    gdt_ptr(%rip)

    /* Reload data segments. Null selector is technically fine in 64-bit
       mode, but using KERNEL_DS keeps everything explicit. */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Reload CS via a far return — there's no direct mov-to-CS in long mode. */
    leaq    1f(%rip), %rax
    pushq   $0x08
    pushq   %rax
    lretq
1:  ret
    .size   gdt_init, . - gdt_init

    .section .note.GNU-stack, "", @progbits
