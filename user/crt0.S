/*
 * Userspace C runtime startup (ROADMAP §3, Phase 5A).
 *
 * The kernel's ELF loader sets rsp to the System V initial stack, so on entry
 * rsp -> argc, then argv[], NULL, envp[], NULL, auxv. We hand argc/argv to
 * main and turn its return value into exit(). There is no libc to register
 * atexit handlers or run constructors yet — this is a freestanding program.
 */

    .section .text
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp              /* end of stack frame chain */
    movq    (%rsp), %rdi            /* argc        */
    leaq    8(%rsp), %rsi           /* argv        */
    andq    $-16, %rsp              /* keep ABI 16-byte alignment for the call */
    call    main

    movl    %eax, %edi              /* exit(main(...)) */
    movl    $60, %eax               /* __NR_exit */
    syscall
1:  jmp     1b                      /* exit never returns */
    .size _start, . - _start

    .section .note.GNU-stack, "", @progbits
