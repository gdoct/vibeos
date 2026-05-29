/*
 * Application-processor bringup trampoline (ROADMAP §1).
 *
 * Copied verbatim to a fixed low physical page (AP_TRAMPOLINE_PHYS = 0x8000)
 * before the BSP sends SIPI. An AP starts here in 16-bit real mode with
 * CS.base = 0x8000, IP = 0. The code walks real -> protected -> long mode,
 * loads the bootstrap page tables (which identity-map this low page, map the
 * kernel high half, and provide the direct map), switches to a 64-bit stack,
 * and jumps to the high-half C entry. All addresses are computed relative to
 * the fixed base, so the link address is irrelevant.
 *
 * The BSP fills the parameter block (cr3 / stack / entry) in the copied page
 * before each SIPI (APs are brought up one at a time).
 */

#define TRAMP   0x8000
#define OFF(l)  ((l) - ap_trampoline_start)            /* real-mode offset (ds.base=0x8000) */
#define LIN(l)  ((l) - ap_trampoline_start + TRAMP)    /* flat linear address */

    .section .text.ap_trampoline, "ax", @progbits
    .code16
    .globl ap_trampoline_start
ap_trampoline_start:
    cli
    cld
    movw    %cs, %ax              /* CS.base = 0x8000; mirror to data segs */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    lgdtl   OFF(tramp_gdt_ptr)    /* ds.base + offset -> the GDT pointer */
    movl    %cr0, %eax
    orl     $1, %eax              /* CR0.PE */
    movl    %eax, %cr0
    ljmpl   $0x08, $LIN(ap_pm32)

    .code32
ap_pm32:
    movw    $0x10, %ax            /* 32-bit data */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    %cr4, %eax
    orl     $0x20, %eax           /* CR4.PAE */
    movl    %eax, %cr4
    movl    LIN(ap_tramp_cr3), %eax  /* bootstrap CR3 (phys < 4 GiB) */
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx     /* IA32_EFER */
    rdmsr
    orl     $0x100, %eax          /* EFER.LME */
    wrmsr
    movl    %cr0, %eax
    orl     $0x80000000, %eax     /* CR0.PG */
    movl    %eax, %cr0
    ljmpl   $0x18, $LIN(ap_lm64)

    .code64
ap_lm64:
    movw    $0x20, %ax            /* 64-bit data */
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movq    LIN(ap_tramp_stack), %rsp
    xorq    %rbp, %rbp
    movq    LIN(ap_tramp_entry), %rax
    jmp     *%rax                 /* -> ap_entry (high half); does not return */

    /* GDT covering 32- and 64-bit code/data; base is a flat linear address. */
    .balign 16
tramp_gdt:
    .quad 0x0000000000000000      /* 0x00 null            */
    .quad 0x00CF9A000000FFFF      /* 0x08 32-bit code     */
    .quad 0x00CF92000000FFFF      /* 0x10 32-bit data     */
    .quad 0x00AF9A000000FFFF      /* 0x18 64-bit code (L) */
    .quad 0x00AF92000000FFFF      /* 0x20 64-bit data     */
tramp_gdt_end:
tramp_gdt_ptr:
    .word   tramp_gdt_end - tramp_gdt - 1
    .long   LIN(tramp_gdt)

    /* Parameter block — filled by the BSP in the copied page before SIPI. */
    .balign 8
    .globl ap_tramp_cr3
ap_tramp_cr3:   .quad 0
    .globl ap_tramp_stack
ap_tramp_stack: .quad 0
    .globl ap_tramp_entry
ap_tramp_entry: .quad 0
    .globl ap_trampoline_end
ap_trampoline_end:

    .section .note.GNU-stack, "", @progbits
