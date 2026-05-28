#ifndef MYOS_TIMER_H
#define MYOS_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Program the 8254 PIT channel 0 to fire IRQ0 at `hz` Hz. Caller must
   have already done irq_init(). */
void     timer_init(uint32_t hz);

uint64_t timer_ticks(void);
uint32_t timer_hz(void);

/* Sleep at least `ms` milliseconds. Uses hlt; requires interrupts on. */
void     ksleep_ms(uint64_t ms);

#ifdef __cplusplus
}
#endif

#endif
