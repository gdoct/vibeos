#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "timer.h"
#include "task.h"

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

/* Tasks sleeping in ksleep_ms, linked through task_t::wq_next. Unsorted:
   we scan the whole list each tick, which is fine for a handful of
   sleepers. Touched only with interrupts disabled (here in the IRQ, and
   under irq_save in ksleep_ms). */
static task_t *g_sleepers = nullptr;

static void wake_due_sleepers(void) {
    task_t **pp = &g_sleepers;
    while (*pp) {
        task_t *t = *pp;
        if (t->wake_tick <= g_ticks) {
            *pp = t->wq_next;       /* unlink */
            t->wq_next = nullptr;
            sched_make_ready(t);
        } else {
            pp = &t->wq_next;
        }
    }
}

static void on_tick(uint8_t irq, regs_t *regs) {
    (void)irq; (void)regs;
    g_ticks++;
    wake_due_sleepers();
    sched_tick();   /* no-op until sched_init has run */
}

void timer_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    uint32_t div = PIT_FREQ / hz;
    if (div > 0xFFFF) div = 0xFFFF;
    if (div == 0)     div = 1;

    /* 0x36 = channel 0, lo+hi access, mode 3, binary count. */
    outb(PIT_CMD,   0x36);
    outb(PIT_DATA0, (uint8_t)(div & 0xFF));
    outb(PIT_DATA0, (uint8_t)((div >> 8) & 0xFF));

    g_hz = hz;
    g_ticks = 0;

    irq_register(0, on_tick);
    irq_unmask(0);

    kprintf("[timer] PIT %u Hz (div=%u)\n", hz, div);
}

uint64_t timer_ticks(void) { return g_ticks; }
uint32_t timer_hz(void)    { return g_hz; }

void ksleep_ms(uint64_t ms) {
    if (g_hz == 0) return;
    /* Round up so a 1 ms sleep at 100 Hz still waits at least one tick. */
    uint64_t wait = (ms * g_hz + 999) / 1000;

    /* Before the scheduler exists there's no task to block, so fall back
       to a halt-spin. This path is only hit during early boot. */
    if (!sched_active()) {
        uint64_t target = g_ticks + wait;
        while (g_ticks < target) __asm__ volatile("hlt");
        return;
    }

    uint64_t f = irq_save();
    task_t *t = task_current();
    t->wake_tick = g_ticks + wait;
    t->wq_next = g_sleepers;        /* push onto sleeper list */
    g_sleepers = t;
    sched_block_and_switch();       /* woken by wake_due_sleepers at deadline */
    irq_restore(f);
}
