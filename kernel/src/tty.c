#include "kernel.h"
#include "irq.h"
#include "task.h"
#include "tty.h"
#include "signal.h"

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

/* Commit the in-progress line (with a trailing newline) and wake readers. The
   cooked ring + wake are done under sched_lock so a reader on another CPU sees
   the bytes and the wake atomically w.r.t. its own check (ROADMAP §2). tty_poll
   only runs on the BSP, so g_line itself needs no lock. */
static void commit_line(void) {
    serial_putc('\r');
    serial_putc('\n');
    sched_lock();
    for (uint32_t i = 0; i < g_line_len; i++) cooked_push(g_line[i]);
    cooked_push('\n');
    wait_queue_wake_all_locked(&g_tty_wq);
    sched_unlock();
    g_line_len = 0;
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

/* Characters injected by another source (the USB keyboard driver, ROADMAP).
   tty_input() runs in the USB worker (possibly an AP); tty_poll() drains it on
   the BSP so the line-discipline state (g_line) stays single-CPU. SPSC ring. */
#define INJ_N 64
static char              g_inject[INJ_N];
static volatile uint32_t g_inj_head, g_inj_tail;

void tty_input(char c) {
    uint32_t next = (g_inj_head + 1) % INJ_N;
    if (next == g_inj_tail) return;          /* full: drop */
    g_inject[g_inj_head] = c;
    __atomic_thread_fence(__ATOMIC_RELEASE); /* publish the byte before the head */
    g_inj_head = next;
}

void tty_poll(void) {
    while (serial_rx_ready())
        input_char(serial_getc());
    while (g_inj_tail != g_inj_head) {        /* drain injected (USB) keystrokes */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);  /* head seen -> byte is valid */
        input_char(g_inject[g_inj_tail]);
        g_inj_tail = (g_inj_tail + 1) % INJ_N;
    }
}

/* Non-destructive: is there a committed line ready to read? (poll/select) */
int tty_readable(void) {
    sched_lock();
    int r = (g_head != g_tail);
    sched_unlock();
    return r;
}

int tty_read(char *buf, uint32_t n) {
    if (n == 0) return 0;
    /* Check emptiness and sleep under sched_lock, paired with commit_line's
       push+wake, so a commit on the BSP can't be missed by a reader on another
       core (ROADMAP §2). */
    sched_lock();
    while (g_head == g_tail) {           /* block until a line is committed */
        if (signals_pending_current()) { /* interrupt the read so the signal lands */
            sched_unlock();
            return -4;                   /* -EINTR */
        }
        wait_queue_sleep_locked(&g_tty_wq);
    }
    uint32_t i = 0;
    while (i < n && g_head != g_tail) {
        char c = g_cooked[g_tail];
        g_tail = (g_tail + 1) % TTY_BUF;
        buf[i++] = c;
        if (c == '\n') break;            /* canonical: one line per read */
    }
    sched_unlock();
    return (int)i;
}
