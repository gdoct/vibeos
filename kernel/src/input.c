#include "kernel.h"
#include "input.h"
#include "spinlock.h"

/*
 * Input event ring — see input.h. A fixed power-of-two ring; the producer (usbd
 * worker) advances `tail` after writing a slot, the consumer (/dev/input read)
 * advances `head` after copying. The single producer publishes the slot data
 * before the tail bump (release); the consumer reads the tail before the slot
 * (acquire). Now that user tasks run on every core (ROADMAP §"User tasks on all
 * cores") two tasks could read /dev/input at once, so the consumer side takes a
 * lock to keep `head` single-writer; grab state is an atomic flag shared with the
 * USB worker on another core.
 */

#define RING 256                         /* events; power of two */
static input_event_t g_ev[RING];
static volatile uint32_t g_head, g_tail; /* head=consumer, tail=producer */
static volatile int g_grab;
static spinlock_t g_rd_lock = SPINLOCK_INIT;   /* serializes /dev/input readers */

static void push(const input_event_t *e) {
    if (!__atomic_load_n(&g_grab, __ATOMIC_ACQUIRE)) return;
    uint32_t t = g_tail;
    uint32_t next = (t + 1) & (RING - 1);
    if (next == (g_head & (RING - 1))) return;   /* full: drop oldest-newest */
    g_ev[t & (RING - 1)] = *e;
    __atomic_thread_fence(__ATOMIC_RELEASE);     /* publish data before tail */
    g_tail = t + 1;
}

void input_push_mouse(int x, int y, int buttons) {
    input_event_t e = { INPUT_EV_MOUSE, (uint8_t)(buttons & 7), 0, 0,
                        (int16_t)x, (int16_t)y };
    push(&e);
}

void input_push_key(char c, int mods, int pressed) {
    input_event_t e = { INPUT_EV_KEY, (uint8_t)mods, (uint8_t)c,
                        (uint8_t)(pressed ? 1 : 0), 0, 0 };
    push(&e);
}

int input_read(void *buf, uint32_t n) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    spin_lock(&g_rd_lock);                       /* keep `head` single-writer */
    while (got + sizeof(input_event_t) <= n) {
        uint32_t h = g_head;
        if (h == g_tail) break;                  /* empty */
        __atomic_thread_fence(__ATOMIC_ACQUIRE); /* tail seen -> slot data valid */
        input_event_t e = g_ev[h & (RING - 1)];
        g_head = h + 1;
        for (uint32_t i = 0; i < sizeof e; i++) out[got + i] = ((uint8_t *)&e)[i];
        got += sizeof e;
    }
    spin_unlock(&g_rd_lock);
    return (int)got;
}

int  input_grabbed(void)    { return __atomic_load_n(&g_grab, __ATOMIC_ACQUIRE); }
void input_set_grab(int on) { __atomic_store_n(&g_grab, on, __ATOMIC_RELEASE); }
