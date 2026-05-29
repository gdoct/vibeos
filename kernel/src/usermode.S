/*
 * SYSCALL/SYSRET entry + the ring-3 launch path (ROADMAP §3, Phase 1).
 */

    .set USER_CS_RPL3, 0x23
    .set USER_DS_RPL3, 0x1B

    .section .text
    .globl  syscall_entry
    .type   syscall_entry, @function
syscall_entry:
    /* Entered from `syscall`: still on the USER stack, IF cleared by SFMASK.
       rcx = user return RIP, r11 = user RFLAGS. Syscall number in rax, args
       in rdi,rsi,rdx,r10,r8,r9 (Linux x86_64 ABI). SYSCALL does NOT switch
       stacks, so we do it: stash the user rsp and load the kernel stack.
       UP uses a global; SMP §1 replaces this with swapgs + KERNEL_GS_BASE. */
    movq    %rsp, g_user_rsp(%rip)
    movq    g_kernel_rsp(%rip), %rsp

    /* Build a syscall_frame_t on the kernel stack (see syscall.c). Pushes go
       high->low, so the last push (rax) is the struct's first field. The
       callee-saved registers are highest (pushed first) — captured so fork can
       clone the parent's rbx/rbp/r12-r15 into the child. */
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %rbp
    pushq   %rbx
    pushq   g_user_rsp(%rip)        /* .user_rsp */
    pushq   %r11                    /* .r11  (user rflags) */
    pushq   %rcx                    /* .rcx  (user rip)    */
    pushq   %r9                     /* .r9   (arg6) */
    pushq   %r8                     /* .r8   (arg5) */
    pushq   %r10                    /* .r10  (arg4) */
    pushq   %rdx                    /* .rdx  (arg3) */
    pushq   %rsi                    /* .rsi  (arg2) */
    pushq   %rdi                    /* .rdi  (arg1) */
    pushq   %rax                    /* .rax  (nr)  -> lowest address */

    movq    %rsp, %rdi              /* arg0 = syscall_frame_t* */
    call    syscall_dispatch        /* result in rax */

    /* Restore. The ABI says a syscall preserves every register except rax
       (return), rcx and r11 (clobbered by the instruction itself). */
    addq    $8, %rsp                /* drop .rax slot; keep result in rax */
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %r10
    popq    %r8
    popq    %r9
    popq    %rcx                    /* user rip   -> sysret target */
    popq    %r11                    /* user rflags */
    popq    %rsp                    /* back to the user stack */
    sysretq                         /* CS/SS from STAR (RPL 3), rax = result */
    .size   syscall_entry, . - syscall_entry

    /* fork_child_return: a forked child's kernel stack is crafted (task_fork)
       so context_switch `ret`s here with rsp pointing at a syscall_frame_t.
       Same tail as syscall_entry, but rax comes from the frame (= 0) so the
       child sees fork() return 0. */
    .globl  fork_child_return
    .type   fork_child_return, @function
fork_child_return:
    call    sched_unlock            /* release the scheduler baton (lock held on entry) */
    movq    0(%rsp), %rax           /* frame.rax (= 0 for the child) */
    addq    $8, %rsp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %r10
    popq    %r8
    popq    %r9
    popq    %rcx                    /* user rip */
    popq    %r11                    /* user rflags */
    /* rsp now points at .user_rsp; the callee-saved regs sit just above it.
       Restore them, then load the user stack last (it overwrites rsp). */
    movq    8(%rsp),  %rbx
    movq    16(%rsp), %rbp
    movq    24(%rsp), %r12
    movq    32(%rsp), %r13
    movq    40(%rsp), %r14
    movq    48(%rsp), %r15
    movq    0(%rsp),  %rsp          /* user rsp */
    sysretq
    .size   fork_child_return, . - fork_child_return

    /* void enter_user(uint64_t entry /rdi/, uint64_t user_rsp /rsi/); noreturn.
       Drops to ring 3 via a hand-built iretq frame. */
    .globl  enter_user
    .type   enter_user, @function
enter_user:
    cli                             /* assemble the frame without an IRQ in it */
    movw    $USER_DS_RPL3, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs

    pushq   $USER_DS_RPL3           /* SS */
    pushq   %rsi                    /* RSP */
    pushq   $0x202                  /* RFLAGS: IF=1 (bit9) + reserved bit1 */
    pushq   $USER_CS_RPL3           /* CS */
    pushq   %rdi                    /* RIP */
    iretq
    .size   enter_user, . - enter_user

    .section .note.GNU-stack, "", @progbits
