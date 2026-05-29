/*
 * 64-bit GDT with user-mode + TSS support.
 *
 * We install our own instead of relying on UEFI's because the firmware
 * GDT lives in memory we don't track and could be reclaimed.
 *
 *   0x00 null
 *   0x08 kernel code   (DPL 0, L=1)
 *   0x10 kernel data   (DPL 0)
 *   0x18 user   data   (DPL 3)        <- order fixed by SYSRET (SS = base+8)
 *   0x20 user   code   (DPL 3, L=1)   <-                       (CS = base+16)
 *   0x28 TSS (16-byte system descriptor, two slots) — base filled by tss_init
 *
 * Descriptor encoding (8 bytes, low → high):
 *   limit[15:0] base[15:0] base[23:16] access flags|limit[19:16] base[31:24]
 *
 * Code:  access=0x9A (P,DPL0,S,Exec/Read);  user code 0xFA (DPL3).
 * Data:  access=0x92 (P,DPL0,S,RW);         user data 0xF2 (DPL3).
 *        code flags=0xAF (G,L,limit hi=0xF); data flags=0xCF (G,D,limit hi).
 *
 * The table lives in .data (not .rodata) because tss_init() patches the TSS
 * descriptor in place at runtime.
 */

    .section .data
    .balign 16
    .globl  gdt_start
gdt_start:
    .quad 0x0000000000000000        /* 0x00 null */
    .quad 0x00AF9A000000FFFF        /* 0x08 kernel CS: L=1 64-bit code */
    .quad 0x00CF92000000FFFF        /* 0x10 kernel DS */
    .quad 0x00CFF2000000FFFF        /* 0x18 user DS (DPL 3) */
    .quad 0x00AFFA000000FFFF        /* 0x20 user CS (DPL 3, L=1) */
    .globl  gdt_tss
gdt_tss:
    .quad 0x0000000000000000        /* 0x28 TSS low  (filled by tss_init) */
    .quad 0x0000000000000000        /* 0x30 TSS high */
gdt_end:

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
