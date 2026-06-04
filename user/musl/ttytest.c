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
#include <time.h>
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

/* Saved at start so the SIGINT handler can hand the terminal back before exit. */
static pid_t g_orig_fg;

static void on_sigint(int sig) {
    (void)sig;
    write(1, "GOT_SIGINT\n", 11);          /* async-signal-safe */
    if (g_orig_fg > 0) tcsetpgrp(0, g_orig_fg);
    _exit(0);
}

/* `ttytest sigint`: become the foreground group, then block reading the tty. A
   ^C (VINTR) typed at the console must be turned into SIGINT and delivered to us
   (the foreground process group) by the line discipline — exercising signal
   delivery from IRQ context to a process group. The driver feeds a 0x03 byte. */
static int run_sigint(void) {
    g_orig_fg = tcgetpgrp(0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigint;             /* no SA_RESTART: read() will see EINTR */
    sigaction(SIGINT, &sa, NULL);

    setpgid(0, 0);                         /* own process group */
    tcsetpgrp(0, getpid());                /* claim the terminal foreground */

    write(1, "WAIT_SIGINT\n", 12);
    for (;;) {                             /* ^C fires on_sigint -> _exit */
        char c;
        ssize_t r = read(0, &c, 1);
        if (r < 0) continue;               /* EINTR: loop (handler exits on ^C) */
        if (r == 0) break;                 /* EOF */
    }
    if (g_orig_fg > 0) tcsetpgrp(0, g_orig_fg);
    return 0;
}

/* Idle child: a nanosleep loop, so a delivered signal lands on a syscall
   boundary (and a job-control stop parks it cleanly). Never returns. */
static void idle_child(void) {
    for (;;) {
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

/* `ttytest stop`: the full ^Z/stop/cont cycle the kernel must support for a
   shell's job control — a stopped child is reported to waitpid(WUNTRACED) as
   WIFSTOPPED, SIGCONT resumes it and is reported as WIFCONTINUED. Driven by
   kill() (deterministic; no input-timing dependence). */
static int run_stop(void) {
    printf("ttytest: stop/cont cycle\n");
    pid_t pid = fork();
    if (pid == 0) { idle_child(); _exit(0); }
    if (pid < 0) { check("fork", 0); goto done; }

    int st;
    if (kill(pid, SIGTSTP) != 0) check("kill(SIGTSTP)", 0);
    pid_t w = waitpid(pid, &st, WUNTRACED);
    check("waitpid(WUNTRACED) returns child", w == pid);
    check("WIFSTOPPED", WIFSTOPPED(st));
    check("WSTOPSIG == SIGTSTP", WIFSTOPPED(st) && WSTOPSIG(st) == SIGTSTP);

    if (kill(pid, SIGCONT) != 0) check("kill(SIGCONT)", 0);
    w = waitpid(pid, &st, WCONTINUED);
    check("waitpid(WCONTINUED) returns child", w == pid);
    check("WIFCONTINUED", WIFCONTINUED(st));

    kill(pid, SIGKILL);
    w = waitpid(pid, &st, 0);
    check("waitpid reaps killed child", w == pid && WIFSIGNALED(st));
done:
    printf("ttytest: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}

/* `ttytest ttou`: a background process group mutating the terminal (tcsetpgrp)
   must be sent SIGTTOU (which stops it). The parent becomes the foreground group,
   forks a child into its own (background) group, and confirms the child stops on
   SIGTTOU rather than completing the mutation. */
static int run_ttou(void) {
    printf("ttytest: background SIGTTOU\n");
    pid_t fg0 = tcgetpgrp(0);
    /* A shell blocks/ignores SIGTTOU around its own terminal handoff; otherwise
       moving itself out of the foreground group and then calling tcsetpgrp would
       stop the shell. Mirror that. */
    signal(SIGTTOU, SIG_IGN);
    setpgid(0, 0);                       /* parent: own group */
    tcsetpgrp(0, getpid());              /* parent: foreground (SIGTTOU ignored) */

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGTTOU, SIG_DFL);        /* child keeps default disposition -> stops */
        setpgid(0, 0);                   /* child: own (background) group */
        tcsetpgrp(0, getpid());          /* background mutation -> SIGTTOU -> stop */
        _exit(42);                       /* only reached if SIGTTOU did NOT fire */
    }
    if (pid < 0) { check("fork", 0); goto restore; }

    int st;
    pid_t w = waitpid(pid, &st, WUNTRACED);
    check("child stopped, not exited", w == pid && WIFSTOPPED(st));
    check("WSTOPSIG == SIGTTOU", WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTOU);

    kill(pid, SIGCONT);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
restore:
    tcsetpgrp(0, fg0 > 0 ? fg0 : getpgrp());
    setpgid(0, fg0 > 0 ? fg0 : 0);
    printf("ttytest: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "sigint") == 0) return run_sigint();
    if (argc > 1 && strcmp(argv[1], "stop") == 0)   return run_stop();
    if (argc > 1 && strcmp(argv[1], "ttou") == 0)   return run_ttou();

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
