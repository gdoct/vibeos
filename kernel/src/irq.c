#include "kernel.h"
#include "irq.h"
#include "apic.h"
#include "signal.h"

extern "C" void pic_remap(uint8_t off1, uint8_t off2);
extern "C" void pic_eoi(uint8_t irq);
extern "C" void pic_mask(uint8_t irq);
extern "C" void pic_unmask(uint8_t irq);

/* Up to MAX_SHARED handlers per IRQ so several PCI devices can share one
   level-triggered INTx vector (e.g. virtio-blk + virtio-net both land on
   APIC_PCI_VECTOR). Each handler checks its own device's ISR and ignores
   interrupts that aren't its. */
#define MAX_SHARED 4
static irq_handler_t g_handlers[NUM_IRQS][MAX_SHARED];
static int           g_apic_mode = 0;

void irq_set_apic_mode(int on) { g_apic_mode = on; }

void irq_init(void) {
    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    for (int i = 0; i < NUM_IRQS; i++)
        for (int j = 0; j < MAX_SHARED; j++) g_handlers[i][j] = nullptr;
    kprintf("[irq] PIC remapped to %x..%x, all lines masked\n",
            IRQ_BASE, IRQ_BASE + NUM_IRQS - 1);
}

void irq_register(uint8_t irq, irq_handler_t h) {
    if (irq >= NUM_IRQS) return;
    for (int j = 0; j < MAX_SHARED; j++)
        if (!g_handlers[irq][j]) { g_handlers[irq][j] = h; return; }
    panic("irq_register: irq %u handler table full", irq);
}

void irq_mask(uint8_t irq)   { if (irq < NUM_IRQS) pic_mask(irq); }
void irq_unmask(uint8_t irq) { if (irq < NUM_IRQS) pic_unmask(irq); }

/*
 * Called from the IRQ asm stub (irq_common in isr.S). The vector is
 * already in regs->vector; convert to an IRQ number and dispatch.
 *
 * EOI is sent BEFORE the handler runs. The scheduler's timer handler
 * may context-switch away and never return here — in that case the
 * deferred-EOI variant would leave the interrupt latched and block all
 * further interrupts. With IF=0 in the CPU there's no harm in EOIing
 * early. For level-triggered I/O APIC lines this can cause one redundant
 * re-fire (the line is still asserted when we EOI), but the device's ISR
 * read in the handler deasserts it and the redundant interrupt is a
 * harmless no-op once the handler checks "was this mine?".
 */
extern "C" void irq_dispatch(regs_t *r) {
    uint64_t v = r->vector;
    if (v < IRQ_BASE || v >= IRQ_BASE + NUM_IRQS) {
        kprintf("[irq] spurious vector %lu\n", (unsigned long)v);
        return;
    }
    uint8_t irq = (uint8_t)(v - IRQ_BASE);
    if (g_apic_mode) lapic_eoi();
    else             pic_eoi(irq);
    for (int j = 0; j < MAX_SHARED; j++)        /* shared INTx: poll each device */
        if (g_handlers[irq][j]) g_handlers[irq][j](irq, r);

    /* On the way back to ring 3, deliver any pending signal to the interrupted
       task (ROADMAP §3). This is how a signal reaches a process spinning in
       userspace, including one running on another core (its next tick). */
    signals_deliver_regs(r);
}
