/*
 * Kernel entry from the UEFI bootloader — higher-half relink (ROADMAP §3,
 * Phase 0).
 *
 * The bootloader loads our PT_LOAD segments at their p_paddr and jumps to
 * e_entry under UEFI identity paging, with:
 *   - rdi = BootInfo*   (a low physical pointer)
 *   - rsp = a low boot stack
 *
 * _start lives in the .boot section (linked LOW: VMA == LMA == physical),
 * so e_entry stays a low address the firmware can jump to. It builds a
 * throwaway bootstrap page table that maps three things:
 *   - PML4[0]   -> low identity, so our own RIP stays valid across the CR3
 *                  load (we are still executing at a low address here);
 *   - PML4[256] -> the PHYS_OFFSET direct map, so phys_to_virt() works the
 *                  instant we are high (paging.c's table walker needs it);
 *   - PML4[511] -> the kernel image at KERNEL_VBASE (-2 GiB).
 * Each of the three PDPTs points at one shared 1 GiB page directory (2 MiB
 * pages mapping physical 0..1 GiB) — enough to cover the kernel image, the
 * BootInfo, and all early PMM memory in our QEMU config.
 *
 * After loading CR3 we jump to _start_high (linked high), switch to a high
 * boot stack, and call kmain. paging_init then builds the real tables (no
 * low identity) and the low half belongs to userspace.
 */

    .set KV_PML4_IDX, 511     /* (0xFFFFFFFF80000000 >> 39) & 0x1FF */
    .set KV_PDPT_IDX, 510     /* (0xFFFFFFFF80000000 >> 30) & 0x1FF */
    .set DM_PML4_IDX, 256     /* (0xFFFF800000000000 >> 39) & 0x1FF */

    .set PF_P,  0x1
    .set PF_W,  0x2
    .set PF_PS, 0x80

/* ===== low trampoline ===================================================== */

    .section .boot.text, "ax", @progbits
    .globl _start
    .type  _start, @function
_start:
    cli
    cld
    movq    %rdi, %r15                 /* preserve BootInfo* across the stub */

    /* boot_pd[i] = i*2MiB | P|W|PS  (one 1 GiB window, 512 * 2 MiB pages) */
    leaq    boot_pd(%rip), %rdi
    xorl    %ecx, %ecx
    movq    $(PF_P | PF_W | PF_PS), %rax   /* phys 0, then += 2 MiB each step */
1:
    movq    %rax, (%rdi, %rcx, 8)
    addq    $0x200000, %rax
    incl    %ecx
    cmpl    $512, %ecx
    jb      1b

    /* Point each PDPT at the shared PD. */
    leaq    boot_pd(%rip), %rax
    orq     $(PF_P | PF_W), %rax
    leaq    boot_pdpt_id(%rip), %rdi
    movq    %rax, 0(%rdi)                       /* identity: PDPT[0]  */
    leaq    boot_pdpt_dm(%rip), %rdi
    movq    %rax, 0(%rdi)                       /* direct map: PDPT[0] */
    leaq    boot_pdpt_kv(%rip), %rdi
    movq    %rax, KV_PDPT_IDX*8(%rdi)           /* kernel high: PDPT[510] */

    /* Point the PML4 at the three PDPTs. */
    leaq    boot_pml4(%rip), %rsi
    leaq    boot_pdpt_id(%rip), %rax
    orq     $(PF_P | PF_W), %rax
    movq    %rax, 0(%rsi)                        /* PML4[0]   identity   */
    leaq    boot_pdpt_dm(%rip), %rax
    orq     $(PF_P | PF_W), %rax
    movq    %rax, DM_PML4_IDX*8(%rsi)            /* PML4[256] direct map */
    leaq    boot_pdpt_kv(%rip), %rax
    orq     $(PF_P | PF_W), %rax
    movq    %rax, KV_PML4_IDX*8(%rsi)            /* PML4[511] kernel     */

    /* CR3 = boot_pml4. Its low VMA equals its physical address (.boot is
       linked VMA == LMA), so leaq gives exactly what CR3 wants. */
    leaq    boot_pml4(%rip), %rax
    movq    %rax, %cr3

    /* We are still executing low (identity still maps us); jump high. */
    movabsq $_start_high, %rax
    jmp     *%rax
    .size _start, . - _start

    /* Bootstrap page-table scratch. PROGBITS + .skip => the file image
       carries them pre-zeroed, so only the entries _start writes are set;
       all other slots stay 0. ~20 KiB of zeros in the image — negligible. */
    .section .boot.data, "aw", @progbits
    .balign 4096
    .globl boot_pml4       /* reused as the AP-bringup CR3 (identity+kernel+dm) */
boot_pml4:     .skip 4096
boot_pdpt_id:  .skip 4096
boot_pdpt_dm:  .skip 4096
boot_pdpt_kv:  .skip 4096
boot_pd:       .skip 4096

/* ===== high-half entry ==================================================== */

    .section .text._start_high, "ax", @progbits
    .globl _start_high
    .type  _start_high, @function
_start_high:
    movabsq $boot_stack_top, %rsp      /* switch to the high boot stack */
    xorq    %rbp, %rbp
    movq    %r15, %rdi                 /* BootInfo* (still a low phys ptr) */
    call    kmain

.Lhang:
    cli
    hlt
    jmp     .Lhang
    .size _start_high, . - _start_high

    /* High boot stack (16 KiB) lives in .bss, i.e. the kernel-high window. */
    .section .bss
    .balign 16
boot_stack:
    .skip 16384
boot_stack_top:

    .section .note.GNU-stack, "", @progbits
