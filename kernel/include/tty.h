#ifndef VIBEOS_TTY_H
#define VIBEOS_TTY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A minimal canonical (line-buffered) serial console (ROADMAP §3 B.4).
 *
 * tty_poll drains the UART receive FIFO and runs a tiny line discipline:
 * printable characters are echoed and accumulated; backspace erases; CR/LF
 * commits the line (plus a trailing '\n') to a cooked buffer and wakes any
 * blocked reader. It is called from the timer tick, so input latency is one
 * tick (~10 ms) — fine for typing and avoids wiring the UART RX IRQ.
 *
 * tty_read blocks until cooked input is available, then returns up to n bytes.
 * This backs read(0) for userspace.
 */
void tty_poll(void);
int  tty_read(char *buf, uint32_t n);

/* Inject a character into the console input stream (e.g. from a USB keyboard).
   Safe to call from any CPU; the BSP's tty_poll drains it through the line
   discipline. */
void tty_input(char c);

#ifdef __cplusplus
}
#endif

#endif
