#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "timer.h"
#include "task.h"
#include "tty.h"
#include "smp.h"

/*
 * 8254 PIT, channel 0, mode 3 (square wave). Fires IRQ0 at the requested
 * rate. The base oscillator is ~1.193182 MHz, divided by a 16-bit value
 * to produce the actual frequency.
 */

#define PIT_FREQ      1193182u
#define PIT_DATA0     0x40
#define PIT_CMD       0x43

static volatile uint64_t g_ticks = 0;
static uint32_t          g_hz    = 0;

/* The tick fires on every CPU (each has its own LAPIC timer). Only the BSP
   advances the global time base and drains the serial console (single owner of
   g_ticks / the TTY ring); every CPU runs task_tick to wake due sleepers and
   preempt whatever it's running. */
static void on_tick(uint8_t irq, regs_t *regs) {
    (void)irq; (void)regs;
    if (smp_cpu_index() == 0) {
        g_ticks++;
        tty_poll();
    }
    task_tick();    /* no-op until sched_init has run */
}

void timer_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    g_hz = hz;
    g_ticks = 0;

    /* The tick lands on IRQ 0 regardless of source: the LAPIC timer
       (APIC mode, vector 0x20) or the PIT via the 8259 (timer_start_pit).
       on_tick is source-agnostic. */
    irq_register(0, on_tick);
    kprintf("[timer] tick handler registered for %u Hz\n", hz);
}

/* Fallback tick source when there's no APIC: program PIT channel 0 to
   raise IRQ 0 at g_hz and unmask it on the 8259. */
void timer_start_pit(void) {
    uint32_t div = PIT_FREQ / g_hz;
    if (div > 0xFFFF) div = 0xFFFF;
    if (div == 0)     div = 1;

    /* 0x36 = channel 0, lo+hi access, mode 3, binary count. */
    outb(PIT_CMD,   0x36);
    outb(PIT_DATA0, (uint8_t)(div & 0xFF));
    outb(PIT_DATA0, (uint8_t)((div >> 8) & 0xFF));

    irq_unmask(0);
    kprintf("[timer] PIT %u Hz (div=%u) driving IRQ0\n", g_hz, div);
}

uint64_t timer_ticks(void) { return g_ticks; }
uint32_t timer_hz(void)    { return g_hz; }

void ksleep_ms(uint64_t ms) {
    if (g_hz == 0) return;
    /* Round up so a 1 ms sleep at 100 Hz still waits at least one tick. */
    uint64_t wait = (ms * g_hz + 999) / 1000;
    task_sleep_ticks(g_ticks + wait);   /* handles the pre-scheduler case too */
}
