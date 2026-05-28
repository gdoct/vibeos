/*
 * Kernel entry from the UEFI bootloader.
 *
 * Bootloader contract (see boot/src/main.c):
 *   - rsp points at a fresh kernel stack (top, 64 KiB region)
 *   - rdi holds BootInfo*
 *   - SysV ABI, identity-mapped paging still active
 */

    .section .text._start, "ax", @progbits
    .globl _start
    .type  _start, @function
_start:
    cld
    xor   %rbp, %rbp
    and   $-16, %rsp           /* keep 16-byte alignment */
    call  kmain

    /* Should never return. Park the CPU. */
.Lhang:
    cli
    hlt
    jmp   .Lhang
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
