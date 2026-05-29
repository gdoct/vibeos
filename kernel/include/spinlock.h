#ifndef VIBEOS_SPINLOCK_H
#define VIBEOS_SPINLOCK_H

#include <stdint.h>
#include "irq.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * IRQ-safe test-and-set spinlock (ROADMAP §1).
 *
 * Acquire disables interrupts on this CPU *then* spins on an atomic word, so
 * (a) an IRQ handler on the holding CPU can't deadlock trying to re-take the
 * lock, and (b) other CPUs are excluded by the atomic. Release stores the word
 * (store-release) and restores the caller's interrupt state.
 *
 * On a uniprocessor there's no contention — IF is already off, so the exchange
 * always succeeds on the first try and this degenerates to irq_save/restore.
 *
 * Convention: every shared mutable structure owns a spinlock_t and takes it
 * here rather than calling irq_save directly.
 */
typedef struct spinlock {
    volatile uint32_t locked;   /* 0 = free, 1 = held */
    uint64_t          flags;    /* saved RFLAGS of the current holder */
} spinlock_t;

#define SPINLOCK_INIT  { 0, 0 }

static inline void spin_lock_init(spinlock_t *l) {
    l->locked = 0;
    l->flags  = 0;
}

static inline void spin_lock(spinlock_t *l) {
    uint64_t f = irq_save();    /* IF off first: IRQ-safe on this CPU */
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
    l->flags = f;               /* only the holder writes flags */
}

static inline void spin_unlock(spinlock_t *l) {
    uint64_t f = l->flags;
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    irq_restore(f);
}

#ifdef __cplusplus
}
#endif

#endif
