#ifndef MYOS_APIC_H
#define MYOS_APIC_H

#include <stdint.h>
#include "../../boot/include/bootinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Local APIC + I/O APIC, replacing the 8259 PIC.
 *
 * apic_init parses the ACPI MADT (via BootInfo's RSDP) to locate the
 * LAPIC and I/O APIC, enables the LAPIC, masks every I/O APIC redirection
 * entry, routes the PCI INTx GSIs to a shared vector, calibrates the
 * LAPIC timer against the PIT, and starts it as the periodic system tick.
 *
 * Returns 1 on success (APIC is now the active interrupt controller), 0
 * if the MADT is missing/unusable (caller should stay on the PIC + PIT).
 */
int  apic_init(const BootInfo *bi, uint32_t tick_hz);

/* True once apic_init has switched the system onto the APIC. */
int  apic_enabled(void);

/* Acknowledge the in-service interrupt to the local APIC. */
void lapic_eoi(void);

/* Vector the LAPIC timer and routed PCI INTx land on (both dispatched
   through irq_common, so they reuse irq_register: irq 0 and irq 11). */
#define APIC_TIMER_VECTOR   0x20
#define APIC_PCI_VECTOR     0x2B
#define APIC_SPURIOUS_VECTOR 0xFF

#ifdef __cplusplus
}
#endif

#endif
