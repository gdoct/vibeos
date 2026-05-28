#ifndef MYOS_IRQ_H
#define MYOS_IRQ_H

#include <stdint.h>
#include "regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware IRQs land at CPU vectors 0x20..0x2F after PIC remap.
 * From the driver's perspective they're irq 0..15 — the IRQ_BASE
 * translation lives in irq.c.
 */
#define IRQ_BASE   0x20
#define NUM_IRQS   16

typedef void (*irq_handler_t)(uint8_t irq, regs_t *regs);

void irq_init(void);
void irq_register(uint8_t irq, irq_handler_t h);
void irq_mask(uint8_t irq);
void irq_unmask(uint8_t irq);

/* Switch irq_dispatch's end-of-interrupt from the 8259 to the local APIC.
   Called by apic_init once the APIC is the active controller. */
void irq_set_apic_mode(int on);

/* Set IF=1. Don't call until irq_init() has remapped the PIC. */
static inline void irq_enable(void)  { __asm__ volatile("sti"); }
static inline void irq_disable(void) { __asm__ volatile("cli"); }

/* Is the interrupt flag currently set? */
static inline int irqs_enabled(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f) :: "memory");
    return (f >> 9) & 1;          /* RFLAGS.IF */
}

/* Disable interrupts, returning the previous RFLAGS so a matching
   irq_restore can put IF back exactly how it was. Lets code that may run
   in either task or IRQ context guard a critical section without
   unconditionally re-enabling. */
static inline uint64_t irq_save(void) {
    uint64_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f) {
    __asm__ volatile("pushq %0; popfq" :: "r"(f) : "memory", "cc");
}

#ifdef __cplusplus
}
#endif

#endif
