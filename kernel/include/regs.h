#ifndef MYOS_REGS_H
#define MYOS_REGS_H

#include <stdint.h>

/*
 * Snapshot of CPU state at an exception/interrupt entry.
 *
 * The layout exactly mirrors the order the ISR stubs push values onto the
 * stack (see kernel/src/isr.S), so a pointer to this struct is just the
 * stack pointer at the point we call into C. Lowest address first.
 */
typedef struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;       /* pushed by ISR stub */
    uint64_t error_code;   /* pushed by CPU on some vectors; 0 otherwise */
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by CPU */
} regs_t;

#endif
