#include "kernel.h"
#include "irq.h"
#include "task.h"
#include "tty.h"
#include "signal.h"
#include "paging.h"

/*
 * Serial TTY with a termios-driven line discipline (ROADMAP §"Interactive I/O").
 *
 * The discipline is governed by `g_tio`, a kernel `struct termios` that
 * userspace reads/sets via ioctl (TCGETS/TCSETS). In canonical mode (ICANON)
 * input is line-buffered: bytes accumulate in `g_line`, ERASE/KILL/EOF are
 * handled, and a committed line (Enter / EOF) is pushed to the `g_in` ring for
 * readers. In raw mode each byte is pushed immediately. ISIG turns the
 * INTR/QUIT/SUSP control chars into signals to the terminal's foreground process
 * group (`g_fg_pgrp`); ECHO controls echoing; OPOST/ONLCR govern output.
 *
 * tty_poll() runs in timer-IRQ context (on the BSP, the single owner of the line
 * state); tty_read()/tty_ioctl() run in task context. The IRQ-vs-task races are
 * covered the usual way (ROADMAP §2): the `g_in` ring + reader wake are touched
 * under sched_lock, so a commit on the BSP can't be missed by a reader on
 * another core. sched_lock disables interrupts, so taking it from tty_poll can't
 * re-enter the tick.
 */

#define TTY_BUF 512

/* errno values returned to userspace (negated). */
#define EINTR  4
#define EFAULT 14
#define EINVAL 22
#define ENOTTY 25

static char     g_in[TTY_BUF];           /* bytes available to readers */
static uint32_t g_head, g_tail;          /* ring: head==tail means empty */
static char     g_line[TTY_BUF];         /* canonical line being edited */
static uint32_t g_line_len;
static int      g_last_cr;               /* swallow the \n of a CR/LF pair */
static int      g_eof;                   /* pending canonical EOF(s): read returns 0 */
static wait_queue_t g_tty_wq;

static struct ktermios g_tio;
static struct winsize  g_winsize;
static int g_fg_pgrp;                    /* foreground process group (0 = none) */
static int g_sid;                        /* controlling session (0 = none) */

void tty_init(void) {
    g_tio.c_iflag = ICRNL | IXON;
    g_tio.c_oflag = OPOST | ONLCR;
    g_tio.c_cflag = B38400 | CS8 | CREAD | CLOCAL;
    g_tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    g_tio.c_cc[VINTR]  = 3;      /* ^C */
    g_tio.c_cc[VQUIT]  = 28;     /* ^\ */
    g_tio.c_cc[VERASE] = 0x7f;   /* DEL */
    g_tio.c_cc[VKILL]  = 21;     /* ^U */
    g_tio.c_cc[VEOF]   = 4;      /* ^D */
    g_tio.c_cc[VMIN]   = 1;
    g_tio.c_cc[VTIME]  = 0;
    g_tio.c_cc[VSTART] = 17;     /* ^Q */
    g_tio.c_cc[VSTOP]  = 19;     /* ^S */
    g_tio.c_cc[VSUSP]  = 26;     /* ^Z */
    g_tio.c_cc[VWERASE]= 23;     /* ^W */
    g_tio.c_cc[VLNEXT] = 22;     /* ^V */
    g_winsize.ws_row = 24;
    g_winsize.ws_col = 80;
}

/* ---- echo + output processing ---- */

/* Echo one input byte to the terminal. Printable bytes go out verbatim; a
   newline becomes CR/LF; other control bytes echo in caret notation (^X). */
static void echo_char(char c) {
    if (c == '\n' || c == '\r') { serial_putc('\r'); serial_putc('\n'); return; }
    if (c == '\t') { serial_putc('\t'); return; }
    if ((unsigned char)c < 0x20) { serial_putc('^'); serial_putc((char)(c + '@')); return; }
    if ((unsigned char)c == 0x7f) return;       /* DEL: handled by the eraser */
    serial_putc(c);
}

/* write(2) to the console: apply output processing (OPOST/ONLCR/OCRNL). */
int tty_write(const char *buf, uint32_t n) {
    int opost = (g_tio.c_oflag & OPOST) != 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = buf[i];
        if (opost) {
            if (c == '\n' && (g_tio.c_oflag & ONLCR)) { serial_putc('\r'); serial_putc('\n'); continue; }
            if (c == '\r' && (g_tio.c_oflag & OCRNL)) { serial_putc('\n'); continue; }
        }
        serial_putc(c);
    }
    return (int)n;
}

/* ---- input ring ---- */

static void ring_push(char c) {
    uint32_t next = (g_head + 1) % TTY_BUF;
    if (next != g_tail) {                /* drop on overflow rather than block */
        g_in[g_head] = c;
        g_head = next;
    }
}

/* Push the bytes of the current line (plus an optional terminator) into the read
   ring and wake readers — under sched_lock so a reader on another CPU sees the
   bytes and the wake atomically w.r.t. its own check (ROADMAP §2). */
static void deliver_line(int with_newline) {
    sched_lock();
    for (uint32_t i = 0; i < g_line_len; i++) ring_push(g_line[i]);
    if (with_newline) ring_push('\n');
    wait_queue_wake_all_locked(&g_tty_wq);
    sched_unlock();
    g_line_len = 0;
}

/* Push a single byte to the read ring + wake (raw mode). */
static void deliver_raw(char c) {
    sched_lock();
    ring_push(c);
    wait_queue_wake_all_locked(&g_tty_wq);
    sched_unlock();
}

/* Discard pending input (canonical line + read ring) — used on a flushing signal
   and on TCFLSH. */
static void flush_input(void) {
    sched_lock();
    g_head = g_tail = 0;
    g_eof = 0;
    sched_unlock();
    g_line_len = 0;
}

/* Raise a terminal-generated signal at the foreground process group. No-op until
   job control is configured (a process has done tcsetpgrp); the simple shell
   never sets a foreground group, so its behaviour is unchanged. */
static void tty_signal(int sig) {
    if (g_fg_pgrp) tasks_signal_pgrp(g_fg_pgrp, sig);
}

static void erase_one(void) {
    if (g_line_len == 0) return;
    g_line_len--;
    if ((g_tio.c_lflag & ECHO) && (g_tio.c_lflag & ECHOE)) {
        serial_putc('\b'); serial_putc(' '); serial_putc('\b');
    }
}

static void input_char(char c) {
    cc_t *cc = g_tio.c_cc;

    if (g_tio.c_iflag & ISTRIP) c &= 0x7f;

    /* CR/LF translation (iflag). */
    if (c == '\r') {
        if (g_tio.c_iflag & IGNCR) { g_last_cr = 1; return; }   /* drop CR */
        if (g_tio.c_iflag & ICRNL) c = '\n';
    } else if (c == '\n') {
        if (g_tio.c_iflag & INLCR) c = '\r';
    }

    /* Signal-generating control chars (ISIG). */
    if (g_tio.c_lflag & ISIG) {
        if (cc[VINTR] && c == (char)cc[VINTR]) {
            if (g_tio.c_lflag & ECHO) echo_char(c);
            if (!(g_tio.c_lflag & NOFLSH)) flush_input();
            tty_signal(SIGINT);
            return;
        }
        if (cc[VQUIT] && c == (char)cc[VQUIT]) {
            if (g_tio.c_lflag & ECHO) echo_char(c);
            if (!(g_tio.c_lflag & NOFLSH)) flush_input();
            tty_signal(SIGQUIT);
            return;
        }
        if (cc[VSUSP] && c == (char)cc[VSUSP]) {
            if (g_tio.c_lflag & ECHO) echo_char(c);
            if (!(g_tio.c_lflag & NOFLSH)) flush_input();
            tty_signal(SIGTSTP);
            return;
        }
    }

    /* Raw mode: every byte is immediately readable. */
    if (!(g_tio.c_lflag & ICANON)) {
        if (g_tio.c_lflag & ECHO) echo_char(c);
        deliver_raw(c);
        g_last_cr = (c == '\r');
        return;
    }

    /* ---- canonical line discipline ---- */
    if (c == '\r' || c == '\n') {           /* line terminator (post-translation) */
        if (c == '\n' && g_last_cr) { g_last_cr = 0; return; }  /* swallow LF of CRLF */
        g_last_cr = (c == '\r');
        if (g_tio.c_lflag & ECHO) echo_char('\n');
        deliver_line(1);                    /* include the newline in the line */
        return;
    }
    g_last_cr = 0;

    if (cc[VEOF] && c == (char)cc[VEOF]) {  /* ^D: deliver line now (EOF if empty) */
        if (g_line_len == 0) {
            sched_lock();
            g_eof++;
            wait_queue_wake_all_locked(&g_tty_wq);
            sched_unlock();
        } else {
            deliver_line(0);                /* partial line, no newline */
        }
        return;
    }
    if (cc[VERASE] && c == (char)cc[VERASE]) { erase_one(); return; }
    if (cc[VKILL] && c == (char)cc[VKILL]) {
        while (g_line_len) erase_one();
        return;
    }
    if (cc[VWERASE] && c == (char)cc[VWERASE]) {
        while (g_line_len && g_line[g_line_len - 1] == ' ') erase_one();
        while (g_line_len && g_line[g_line_len - 1] != ' ') erase_one();
        return;
    }

    if (g_line_len < TTY_BUF - 1) {
        g_line[g_line_len++] = c;
        if (g_tio.c_lflag & ECHO) echo_char(c);
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

/* Non-destructive: is there input ready to read? (poll/select) */
int tty_readable(void) {
    sched_lock();
    int r = (g_head != g_tail) || g_eof;
    sched_unlock();
    return r;
}

int tty_inq(void) {
    sched_lock();
    int n = (int)((g_head - g_tail) % TTY_BUF);
    if (n < 0) n += TTY_BUF;
    sched_unlock();
    return n;
}

int tty_read(char *buf, uint32_t n) {
    if (n == 0) return 0;

    /* Job control: a background process reading its controlling terminal gets
       SIGTTIN (whose default action stops it). Only enforced once a foreground
       group exists — the simple shell never sets one, so it reads freely. */
    if (g_fg_pgrp) {
        task_t *cur = task_current();
        if (cur && task_pgid(cur) != g_fg_pgrp) {
            tasks_signal_pgrp(task_pgid(cur), SIGTTIN);
            return -EINTR;
        }
    }

    int canonical = (g_tio.c_lflag & ICANON) != 0;

    /* Check emptiness and sleep under sched_lock, paired with deliver_*'s
       push+wake, so a commit on the BSP can't be missed by a reader on another
       core (ROADMAP §2). */
    sched_lock();
    while (g_head == g_tail) {
        if (g_eof) { g_eof--; sched_unlock(); return 0; }   /* EOF */
        if (signals_pending_current()) {                     /* deliver the signal */
            sched_unlock();
            return -EINTR;
        }
        wait_queue_sleep_locked(&g_tty_wq);
    }
    uint32_t i = 0;
    while (i < n && g_head != g_tail) {
        char c = g_in[g_tail];
        g_tail = (g_tail + 1) % TTY_BUF;
        buf[i++] = c;
        if (canonical && c == '\n') break;   /* canonical: one line per read */
    }
    sched_unlock();
    return (int)i;
}

/* ---- terminal ioctls ---- */

/* POSIX: a process in a background process group that mutates the terminal
   (tcsetpgrp / tcsetattr / window size) is sent SIGTTOU, which stops its group —
   unless SIGTTOU is blocked or ignored (as a shell does during its own handoff),
   in which case the operation proceeds. Returns 1 if the caller should stop. */
static int bg_ttou(task_t *t) {
    if (!g_fg_pgrp || !t) return 0;               /* no job control configured */
    if (task_pgid(t) == g_fg_pgrp) return 0;      /* foreground: allowed */
    if (t->sig.blocked & SIGBIT(SIGTTOU)) return 0;
    if (t->sig.act[SIGTTOU - 1].handler == SIG_IGN) return 0;
    return 1;
}

int tty_ioctl(unsigned cmd, uint64_t arg) {
    task_t *t = task_current();
    vmspace_t *vm = t ? t->vm : nullptr;

    /* Background terminal-control ops raise SIGTTOU and stop the caller's group. */
    switch (cmd) {
    case TCSETS: case TCSETSW: case TCSETSF:
    case TIOCSPGRP: case TIOCSWINSZ:
        if (bg_ttou(t)) { tasks_signal_pgrp(task_pgid(t), SIGTTOU); return -EINTR; }
        break;
    default: break;
    }

    switch (cmd) {
    case TCGETS:
        if (copy_to_user(vm, arg, &g_tio, sizeof g_tio) < 0) return -EFAULT;
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct ktermios nt;
        if (copy_from_user(&nt, vm, arg, sizeof nt) < 0) return -EFAULT;
        if (cmd == TCSETSF) flush_input();   /* discard pending input on a flush set */
        g_tio = nt;
        return 0;
    }
    case TCFLSH:
        flush_input();
        return 0;
    case TCSBRK:                              /* drain (we don't buffer output) */
    case TCXONC:
        return 0;
    case TIOCGWINSZ:
        if (copy_to_user(vm, arg, &g_winsize, sizeof g_winsize) < 0) return -EFAULT;
        return 0;
    case TIOCSWINSZ: {
        struct winsize ws;
        if (copy_from_user(&ws, vm, arg, sizeof ws) < 0) return -EFAULT;
        g_winsize = ws;
        tty_signal(SIGWINCH);
        return 0;
    }
    case FIONREAD:
    case TIOCOUTQ: {
        int v = (cmd == FIONREAD) ? tty_inq() : 0;
        if (copy_to_user(vm, arg, &v, sizeof v) < 0) return -EFAULT;
        return 0;
    }
    case TIOCGPGRP: {
        int pg = g_fg_pgrp ? g_fg_pgrp : (t ? task_pgid(t) : 0);
        if (copy_to_user(vm, arg, &pg, sizeof pg) < 0) return -EFAULT;
        return 0;
    }
    case TIOCSPGRP: {
        int pg;
        if (copy_from_user(&pg, vm, arg, sizeof pg) < 0) return -EFAULT;
        if (pg <= 0) return -EINVAL;
        g_fg_pgrp = pg;
        return 0;
    }
    case TIOCGSID: {
        int sid = g_sid ? g_sid : (t ? task_sid(t) : 0);
        if (copy_to_user(vm, arg, &sid, sizeof sid) < 0) return -EFAULT;
        return 0;
    }
    case TIOCSCTTY:                           /* become controlling terminal */
        if (t) {
            g_sid = task_sid(t);
            g_fg_pgrp = task_pgid(t);
        }
        return 0;
    case TIOCNOTTY:
        g_sid = 0; g_fg_pgrp = 0;
        return 0;
    default:
        return -ENOTTY;
    }
}
