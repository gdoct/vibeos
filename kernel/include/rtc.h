#ifndef VIBEOS_RTC_H
#define VIBEOS_RTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wall-clock time from the CMOS RTC (ROADMAP: real timekeeping).
 *
 * The RTC is read exactly once at boot and converted to a Unix epoch second,
 * which is then anchored to the monotonic timer tick count. Current wall time is
 * that anchor plus elapsed ticks — so the clock advances at the timer's rate
 * (100 Hz) rather than re-reading the slow CMOS on every gettimeofday.
 */
void     rtc_init(void);                              /* read CMOS, anchor to ticks */
uint64_t rtc_boot_epoch(void);                        /* Unix seconds sampled at boot */

/* Current CLOCK_REALTIME, split into whole seconds + nanoseconds. */
void     rtc_realtime(uint64_t *sec, uint64_t *nsec);

#ifdef __cplusplus
}
#endif

#endif
