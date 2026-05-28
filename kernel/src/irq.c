#include "kernel.h"
#include "irq.h"

extern "C" void pic_remap(uint8_t off1, uint8_t off2);
extern "C" void pic_eoi(uint8_t irq);
extern "C" void pic_mask(uint8_t irq);
extern "C" void pic_unmask(uint8_t irq);

static irq_handler_t g_handlers[NUM_IRQS];

void irq_init(void) {
    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    for (int i = 0; i < NUM_IRQS; i++) g_handlers[i] = nullptr;
    kprintf("[irq] PIC remapped to %x..%x, all lines masked\n",
            IRQ_BASE, IRQ_BASE + NUM_IRQS - 1);
}

void irq_register(uint8_t irq, irq_handler_t h) {
    if (irq < NUM_IRQS) g_handlers[irq] = h;
}

void irq_mask(uint8_t irq)   { if (irq < NUM_IRQS) pic_mask(irq); }
void irq_unmask(uint8_t irq) { if (irq < NUM_IRQS) pic_unmask(irq); }

/*
 * Called from the IRQ asm stub (irq_common in isr.S). The vector is
 * already in regs->vector; convert to an IRQ number and dispatch.
 *
 * EOI is sent BEFORE the handler runs. The scheduler's timer handler
 * may context-switch away and never return here — in that case the
 * deferred-EOI variant would leave the PIC line latched and block all
 * further timer interrupts. With IF=0 in the CPU there's no harm in
 * EOIing early.
 */
extern "C" void irq_dispatch(regs_t *r) {
    uint64_t v = r->vector;
    if (v < IRQ_BASE || v >= IRQ_BASE + NUM_IRQS) {
        kprintf("[irq] spurious vector %lu\n", (unsigned long)v);
        return;
    }
    uint8_t irq = (uint8_t)(v - IRQ_BASE);
    pic_eoi(irq);
    if (g_handlers[irq]) g_handlers[irq](irq, r);
}
