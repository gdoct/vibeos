#include "kernel.h"
#include "irq.h"
#include "task.h"
#include "tty.h"

/*
 * Canonical serial TTY (ROADMAP §3 B.4). See tty.h for the model.
 *
 * Two buffers:
 *   - `line` holds the line currently being edited (not yet readable).
 *   - `cooked` is a ring of bytes committed on Enter, waiting to be read.
 * tty_poll() runs in timer-IRQ context; tty_read() runs in task context and
 * blocks on `g_tty_wq`. The IRQ-vs-task races are covered the usual way:
 * tty_read checks emptiness and sleeps with interrupts disabled, so a commit
 * can't slip in between the check and the sleep.
 */

#define TTY_BUF 512

static char     g_cooked[TTY_BUF];
static uint32_t g_head, g_tail;          /* ring: head==tail means empty */
static char     g_line[TTY_BUF];
static uint32_t g_line_len;
static int      g_last_cr;               /* swallow the \n of a CR/LF pair */
static wait_queue_t g_tty_wq;

static void cooked_push(char c) {
    uint32_t next = (g_head + 1) % TTY_BUF;
    if (next != g_tail) {                /* drop on overflow rather than block */
        g_cooked[g_head] = c;
        g_head = next;
    }
}

/* Commit the in-progress line (with a trailing newline) and wake readers. */
static void commit_line(void) {
    serial_putc('\r');
    serial_putc('\n');
    for (uint32_t i = 0; i < g_line_len; i++) cooked_push(g_line[i]);
    cooked_push('\n');
    g_line_len = 0;
    wait_queue_wake_all(&g_tty_wq);
}

static void input_char(char c) {
    if (c == '\r') {
        commit_line();
        g_last_cr = 1;
        return;
    }
    if (c == '\n') {
        if (!g_last_cr) commit_line();   /* lone LF (e.g. piped input) */
        g_last_cr = 0;
        return;
    }
    g_last_cr = 0;

    if (c == 0x7F || c == 0x08) {        /* DEL / backspace */
        if (g_line_len > 0) {
            g_line_len--;
            serial_putc('\b'); serial_putc(' '); serial_putc('\b');
        }
        return;
    }

    if ((unsigned char)c >= 0x20 && (unsigned char)c < 0x7F) {
        if (g_line_len < TTY_BUF - 1) {
            g_line[g_line_len++] = c;
            serial_putc(c);              /* echo */
        }
    }
}

void tty_poll(void) {
    while (serial_rx_ready())
        input_char(serial_getc());
}

int tty_read(char *buf, uint32_t n) {
    if (n == 0) return 0;
    uint64_t f = irq_save();
    while (g_head == g_tail)             /* block until a line is committed */
        wait_queue_sleep(&g_tty_wq);     /* returns with IF still off */
    uint32_t i = 0;
    while (i < n && g_head != g_tail) {
        char c = g_cooked[g_tail];
        g_tail = (g_tail + 1) % TTY_BUF;
        buf[i++] = c;
        if (c == '\n') break;            /* canonical: one line per read */
    }
    irq_restore(f);
    return (int)i;
}
