#ifndef MYOS_TIMER_H
#define MYOS_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register the tick handler on IRQ 0 and record the target rate. The
   actual source is started separately: the LAPIC timer (apic_init) or
   the PIT (timer_start_pit). Caller must have already done irq_init(). */
void     timer_init(uint32_t hz);

/* Fallback: drive the tick from the 8254 PIT on IRQ 0 (no-APIC path). */
void     timer_start_pit(void);

uint64_t timer_ticks(void);
uint32_t timer_hz(void);

/* Sleep at least `ms` milliseconds. Uses hlt; requires interrupts on. */
void     ksleep_ms(uint64_t ms);

#ifdef __cplusplus
}
#endif

#endif
