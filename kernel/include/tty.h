#ifndef VIBEOS_TTY_H
#define VIBEOS_TTY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A serial console with a termios-driven line discipline (ROADMAP §"Interactive
 * I/O"). The discipline used to be hardwired canonical; it is now governed by a
 * `struct ktermios` that userspace queries/sets through ioctl, so an interactive
 * program (a line editor, bash readline) can put the terminal into raw mode and
 * do its own editing.
 *
 * tty_poll drains the UART receive FIFO (and USB-injected keystrokes) on the BSP
 * and runs the discipline: in canonical mode it echoes + accumulates a line,
 * handling erase/kill/EOF, and commits on Enter; in raw mode each byte is made
 * readable immediately. ISIG turns the INTR/QUIT/SUSP control chars into signals
 * to the terminal's foreground process group. It is called from the timer tick,
 * so input latency is one tick (~10 ms) — fine for typing, no UART RX IRQ.
 *
 * tty_read blocks until input is available, then returns up to n bytes (one line
 * in canonical mode). tty_write applies output processing (OPOST/ONLCR). Both
 * back the implicit console fds (0/1/2) and /dev/tty.
 */

/* Kernel `struct termios` (the 36-byte layout TCGETS/TCSETS exchange — NCCS=19,
   no embedded speed fields; musl's larger userspace struct overlays it and reads
   the baud from c_cflag). */
typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
#define KNCCS 19
struct ktermios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[KNCCS];
};

/* c_iflag */
#define ISTRIP  0000040
#define INLCR   0000100
#define IGNCR   0000200
#define ICRNL   0000400
#define IXON    0002000
/* c_oflag */
#define OPOST   0000001
#define ONLCR   0000004
#define OCRNL   0000010
#define ONLRET  0000040
/* c_cflag */
#define B38400  0000017
#define CS8     0000060
#define CREAD   0000200
#define CLOCAL  0004000
/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define NOFLSH  0000200
#define TOSTOP  0000400
#define IEXTEN  0100000

/* c_cc indices (Linux) */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSTART   8
#define VSTOP    9
#define VSUSP   10
#define VEOL    11
#define VWERASE 14
#define VLNEXT  15
#define VEOL2   16

/* ioctl request numbers (Linux x86 asm-generic/ioctls.h). */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TCSBRK     0x5409
#define TCXONC     0x540A
#define TCFLSH     0x540B
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCOUTQ   0x5411
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCNOTTY  0x5422
#define TIOCGSID   0x5429

struct winsize { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; };

void tty_init(void);       /* install the default termios + window size */
void tty_poll(void);
int  tty_read(char *buf, uint32_t n);
int  tty_write(const char *buf, uint32_t n);   /* applies OPOST/ONLCR */
int  tty_readable(void);   /* non-destructive: input ready? (poll/select) */
int  tty_inq(void);        /* bytes available to read (FIONREAD) */

/* Terminal ioctls. `arg` is a user-space pointer; copy_*_user handles it. Runs
   in the context of the calling task. Returns 0 or a negated errno. */
int  tty_ioctl(unsigned cmd, uint64_t arg);

/* Inject a character into the console input stream (e.g. from a USB keyboard).
   Safe to call from any CPU; the BSP's tty_poll drains it through the line
   discipline. */
void tty_input(char c);

#ifdef __cplusplus
}
#endif

#endif
