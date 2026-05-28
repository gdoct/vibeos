/*
 * void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
 *
 * Save the current callee-saved registers on the current task's stack,
 * stash RSP into *old_rsp, then load RSP from new_rsp and restore that
 * task's callee-saved registers. The `ret` at the bottom pops whatever
 * address the new task last left there — either:
 *   - the instruction after the previous context_switch call site, for
 *     a task that was switched out cooperatively or via the timer IRQ
 *   - task_trampoline, for a brand-new task whose stack we hand-crafted
 *     in task_create
 *
 * The SysV ABI says the caller already saved caller-clobbered regs; we
 * only need to preserve rbx, rbp, r12-r15.
 */

    .section .text
    .globl  context_switch
    .type   context_switch, @function
context_switch:
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, (%rdi)
    movq    %rsi, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret
    .size   context_switch, . - context_switch

    .section .note.GNU-stack, "", @progbits
