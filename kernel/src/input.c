#include "kernel.h"
#include "input.h"

/*
 * Input event ring — see input.h. A fixed power-of-two ring; the producer (usbd
 * worker) advances `tail` after writing a slot, the consumer (GUI server via
 * /dev/input read) advances `head` after copying. x86 store/load ordering makes
 * this safe without a lock for the single-producer / single-consumer case.
 */

#define RING 256                         /* events; power of two */
static input_event_t g_ev[RING];
static volatile uint32_t g_head, g_tail; /* head=consumer, tail=producer */
static volatile int g_grab;

static void push(const input_event_t *e) {
    if (!g_grab) return;
    uint32_t t = g_tail;
    uint32_t next = (t + 1) & (RING - 1);
    if (next == (g_head & (RING - 1))) return;   /* full: drop oldest-newest */
    g_ev[t & (RING - 1)] = *e;
    __asm__ volatile("" ::: "memory");           /* publish data before tail */
    g_tail = t + 1;
}

void input_push_mouse(int x, int y, int buttons) {
    input_event_t e = { INPUT_EV_MOUSE, (uint8_t)(buttons & 7), 0, 0,
                        (int16_t)x, (int16_t)y };
    push(&e);
}

void input_push_key(char c, int mods) {
    input_event_t e = { INPUT_EV_KEY, (uint8_t)mods, (uint8_t)c, 1, 0, 0 };
    push(&e);
}

int input_read(void *buf, uint32_t n) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t got = 0;
    while (got + sizeof(input_event_t) <= n) {
        uint32_t h = g_head;
        if (h == g_tail) break;                  /* empty */
        input_event_t e = g_ev[h & (RING - 1)];
        __asm__ volatile("" ::: "memory");
        g_head = h + 1;
        for (uint32_t i = 0; i < sizeof e; i++) out[got + i] = ((uint8_t *)&e)[i];
        got += sizeof e;
    }
    return (int)got;
}

int  input_grabbed(void)    { return g_grab; }
void input_set_grab(int on) { g_grab = on; }
