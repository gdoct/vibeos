#ifndef VIBEOS_SMP_H
#define VIBEOS_SMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed low physical page the AP trampoline is copied to and where APs start
   in real mode (SIPI vector = this >> 12). Must be page-aligned and < 1 MiB. */
#define AP_TRAMPOLINE_PHYS  0x8000

#define SMP_MAX_CPUS  8

struct cpu {
    uint32_t index;       /* 0 = BSP */
    uint32_t apic_id;
    volatile int online;
};

/* Bring up the application processors. Call after apic_init (LAPIC up, MADT
   parsed) and paging_init, with interrupts enabled (uses the timer for the
   SIPI delays). APs that don't have work yet park in a hlt loop. */
void smp_init(void);

int  smp_cpu_count(void);   /* CPUs online, including the BSP */

/* Index (0 = BSP) of the calling CPU. Safe before the APIC/SMP are up: returns
   0 (only the BSP runs then). Used by the scheduler's per-CPU state and by the
   interrupt-nesting (push/pop) accounting. */
int  smp_cpu_index(void);

/* Per-CPU LAPIC timer start for an AP (reuses the BSP's calibrated rate). */
void ap_entry(void);

/* Flush the TLB on every CPU (this one plus an IPI to the rest, waiting for
   acks). Call from ordinary kernel context with interrupts enabled — never from
   a syscall (IF=0) or under sched_lock, or the ack-wait can deadlock. */
void tlb_shootdown_all(void);

/* Boot-time IPI sanity check (BSP, interrupts enabled). */
void smp_ipi_selftest(void);

#ifdef __cplusplus
}
#endif

#endif
