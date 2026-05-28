/*
 * Exception entry stubs for vectors 0-31.
 *
 * The CPU pushes an error code for some vectors (8, 10-14, 17, 21, 29, 30);
 * the rest get a dummy zero pushed by us so the C handler sees a uniform
 * stack frame. The struct regs layout in <regs.h> mirrors the push order
 * here.
 */

.macro ISR_NOERR n
    .globl  isr\n
    .type   isr\n, @function
isr\n:
    pushq   $0          /* fake error code */
    pushq   $\n         /* vector */
    jmp     isr_common
    .size   isr\n, . - isr\n
.endm

.macro ISR_ERR n
    .globl  isr\n
    .type   isr\n, @function
isr\n:
    pushq   $\n         /* vector (error code already on stack) */
    jmp     isr_common
    .size   isr\n, . - isr\n
.endm

    .section .text

ISR_NOERR 0    /* #DE Divide-by-zero */
ISR_NOERR 1    /* #DB Debug */
ISR_NOERR 2    /* NMI */
ISR_NOERR 3    /* #BP Breakpoint */
ISR_NOERR 4    /* #OF Overflow */
ISR_NOERR 5    /* #BR Bound Range */
ISR_NOERR 6    /* #UD Invalid Opcode */
ISR_NOERR 7    /* #NM Device Not Available */
ISR_ERR   8    /* #DF Double Fault */
ISR_NOERR 9    /* Coprocessor Segment Overrun (reserved) */
ISR_ERR   10   /* #TS Invalid TSS */
ISR_ERR   11   /* #NP Segment Not Present */
ISR_ERR   12   /* #SS Stack-Segment Fault */
ISR_ERR   13   /* #GP General Protection */
ISR_ERR   14   /* #PF Page Fault */
ISR_NOERR 15   /* reserved */
ISR_NOERR 16   /* #MF x87 FP */
ISR_ERR   17   /* #AC Alignment Check */
ISR_NOERR 18   /* #MC Machine Check */
ISR_NOERR 19   /* #XM SIMD FP */
ISR_NOERR 20   /* #VE Virtualization */
ISR_ERR   21   /* #CP Control Protection */
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29   /* #VC VMM Communication */
ISR_ERR   30   /* #SX Security */
ISR_NOERR 31

    .type   isr_common, @function
isr_common:
    /* Save callee-clobbered and callee-saved GPRs. Order must match
       struct regs in <regs.h>: low memory address = pushed last. */
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    cld
    movq    %rsp, %rdi          /* arg0: regs * */
    call    exception_handler

    /* Unreachable in practice — exception_handler halts. Symmetric
       restore for paranoia / future use. */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp           /* pop vector + error code */
    iretq
    .size   isr_common, . - isr_common

    /* ===== Hardware IRQ stubs (vectors 0x20..0x2F) ===== */

.macro IRQ_STUB n
    .globl  irq\n
    .type   irq\n, @function
irq\n:
    pushq   $0              /* no error code from CPU */
    pushq   $(0x20 + \n)    /* vector */
    jmp     irq_common
    .size   irq\n, . - irq\n
.endm

    .section .text
IRQ_STUB 0
IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15

    .type   irq_common, @function
irq_common:
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    cld
    movq    %rsp, %rdi
    call    irq_dispatch

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp       /* pop vector + fake errcode */
    iretq
    .size   irq_common, . - irq_common

    /* Table of stub entry points, used by idt_init() in C. */
    .section .rodata
    .balign 8
    .globl  isr_table
isr_table:
    .quad   isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad   isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad   isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad   isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

    .globl  irq_table
irq_table:
    .quad   irq0,  irq1,  irq2,  irq3,  irq4,  irq5,  irq6,  irq7
    .quad   irq8,  irq9,  irq10, irq11, irq12, irq13, irq14, irq15

    .section .note.GNU-stack, "", @progbits
