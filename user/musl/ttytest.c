/*
 * ttytest — exercises the interactive-I/O ABI (ROADMAP §"Interactive I/O")
 * through musl: termios ioctls (isatty/tcgetattr/tcsetattr, raw mode),
 * TIOCGWINSZ, and job control (setpgid/getpgrp/getsid, tcsetpgrp/tcgetpgrp).
 * Built with musl-gcc as a static Linux binary — if these run unmodified, an
 * interactive line editor (bash readline) has what it needs.
 *
 * Deterministic and non-blocking: it never waits on input, and it restores the
 * terminal's foreground process group before exiting so the parent shell keeps
 * its terminal.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

static int passes, fails;
static void check(const char *what, int ok) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (ok) passes++; else fails++;
}

int main(void) {
    printf("ttytest: interactive-I/O ABI\n");

    /* --- isatty: console fds are terminals, a pipe is not --- */
    check("isatty(0)", isatty(0) == 1);
    check("isatty(1)", isatty(1) == 1);
    check("isatty(2)", isatty(2) == 1);
    int pfd[2];
    if (pipe(pfd) == 0) {
        check("isatty(pipe) == 0 (ENOTTY)", isatty(pfd[0]) == 0 && errno == ENOTTY);
        close(pfd[0]); close(pfd[1]);
    }

    /* --- tcgetattr: sane cooked defaults --- */
    struct termios saved, t;
    int got = (tcgetattr(0, &saved) == 0);
    check("tcgetattr(0)", got);
    check("default ICANON set", got && (saved.c_lflag & ICANON));
    check("default ECHO set",   got && (saved.c_lflag & ECHO));
    check("default ISIG set",   got && (saved.c_lflag & ISIG));
    check("default OPOST set",  got && (saved.c_oflag & OPOST));
    printf("  termios: iflag=%o oflag=%o lflag=%o VINTR=%d VEOF=%d\n",
           saved.c_iflag, saved.c_oflag, saved.c_lflag,
           saved.c_cc[VINTR], saved.c_cc[VEOF]);

    /* --- raw mode round-trip: cfmakeraw clears ICANON/ECHO/ISIG --- */
    t = saved;
    cfmakeraw(&t);
    int set_raw = (tcsetattr(0, TCSANOW, &t) == 0);
    check("tcsetattr raw", set_raw);
    struct termios r;
    if (set_raw && tcgetattr(0, &r) == 0) {
        check("raw clears ICANON", !(r.c_lflag & ICANON));
        check("raw clears ECHO",   !(r.c_lflag & ECHO));
        check("raw clears ISIG",   !(r.c_lflag & ISIG));
    } else {
        check("raw clears ICANON", 0);
    }
    /* restore cooked mode */
    check("tcsetattr restore", tcsetattr(0, TCSANOW, &saved) == 0);

    /* --- window size --- */
    struct winsize ws;
    if (ioctl(0, TIOCGWINSZ, &ws) == 0) {
        printf("  winsize: %d rows x %d cols\n", ws.ws_row, ws.ws_col);
        check("TIOCGWINSZ nonzero", ws.ws_row > 0 && ws.ws_col > 0);
    } else {
        check("TIOCGWINSZ", 0);
    }

    /* --- job control: groups, sessions, foreground group --- */
    pid_t pg = getpgrp();
    pid_t sid = getsid(0);
    printf("  pid=%d pgrp=%d sid=%d\n", (int)getpid(), (int)pg, (int)sid);
    check("getpgrp > 0", pg > 0);
    check("getsid > 0", sid > 0);

    pid_t fg0 = tcgetpgrp(0);
    check("tcgetpgrp(0) ok", fg0 > 0);

    /* Move ourselves into a new process group, claim the terminal, read it back,
       then restore. */
    pid_t mypid = getpid();
    int sp = (setpgid(0, mypid) == 0);
    check("setpgid(self) -> own group", sp);
    check("getpgrp == pid after setpgid", getpgrp() == mypid);
    int ts = (tcsetpgrp(0, mypid) == 0);
    check("tcsetpgrp(self)", ts);
    check("tcgetpgrp == self", tcgetpgrp(0) == mypid);

    /* kill(-pgrp, 0): existence check against our own group must succeed. */
    check("kill(-pgrp, 0)", kill(-mypid, 0) == 0);

    /* Restore the original foreground group so the shell keeps its terminal. */
    tcsetpgrp(0, fg0 > 0 ? fg0 : pg);
    setpgid(0, pg);

    printf("ttytest: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
