#ifndef MYOS_SPINLOCK_H
#define MYOS_SPINLOCK_H

#include <stdint.h>
#include "irq.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Critical-section seam (ROADMAP §3).
 *
 * On a uniprocessor there is no true concurrency: clearing IF (irq_save)
 * already gives mutual exclusion against IRQ handlers and, since nothing
 * else runs, against every other thread. So the UP body of a spinlock is
 * just irq_save / irq_restore — no actual spinning.
 *
 * The point of having the *type* now is to mark every site that will need
 * a real lock once SMP (§1) lands. At that point spin_lock grows a
 * test-and-set / ticket loop around this same IRQ-disable (an IRQ-safe
 * spinlock: take the lock with interrupts off so an IRQ on the holding CPU
 * can't deadlock against it), and these call sites do not change.
 *
 * Convention: every shared mutable structure (scheduler run queue, PMM
 * freelist, kmalloc slabs, FS tables, and the new userspace vmspace / fd
 * table / process table) owns a spinlock_t and takes it here rather than
 * calling irq_save directly. New code uses this from day one; existing
 * bare irq_save/irq_restore sites migrate to it opportunistically.
 */
typedef struct spinlock {
    uint64_t flags;   /* UP: saved RFLAGS from irq_save while held */
#ifndef NDEBUG
    int      held;    /* cheap re-entrancy / unlock-without-lock check */
#endif
} spinlock_t;

#define SPINLOCK_INIT  { 0 }

static inline void spin_lock_init(spinlock_t *l) {
    l->flags = 0;
#ifndef NDEBUG
    l->held = 0;
#endif
}

static inline void spin_lock(spinlock_t *l) {
    uint64_t f = irq_save();
    /* SMP §1: a `lock; bts` / ticket-wait loop goes here, spinning with
       interrupts already off so the holder can't be preempted into a
       deadlock. UP has nothing to spin on. */
    l->flags = f;
#ifndef NDEBUG
    l->held = 1;
#endif
}

static inline void spin_unlock(spinlock_t *l) {
    uint64_t f = l->flags;
#ifndef NDEBUG
    l->held = 0;
#endif
    /* SMP §1: release the lock word (store-release) before restoring IF. */
    irq_restore(f);
}

#ifdef __cplusplus
}
#endif

#endif
