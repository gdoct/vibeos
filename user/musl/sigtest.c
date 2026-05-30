/* §3 signals test (static musl): handler delivery, blocking/pending, default
 * termination (WIFSIGNALED), killing another process, and catching a SIGSEGV. */
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/wait.h>

static volatile sig_atomic_t got_usr1 = 0;
static void on_usr1(int s) { got_usr1 = s; }

static sigjmp_buf jb;
static void on_segv(int s) { (void)s; siglongjmp(jb, 1); }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. catch SIGUSR1 via a handler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    printf("1. handler ran: %s\n", got_usr1 == SIGUSR1 ? "YES" : "NO");

    /* 2. block SIGUSR1: it should stay pending, not delivered. */
    sigset_t set, pend;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    got_usr1 = 0;
    raise(SIGUSR1);
    sigpending(&pend);
    printf("2. blocked: pending=%s, not-delivered=%s\n",
           sigismember(&pend, SIGUSR1) ? "YES" : "NO",
           got_usr1 == 0 ? "YES" : "NO");

    /* 3. unblock -> delivered now. */
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    printf("3. after unblock, delivered: %s\n", got_usr1 == SIGUSR1 ? "YES" : "NO");

    /* 4. default action terminates: child raises SIGTERM with no handler. */
    pid_t pid = fork();
    if (pid == 0) { raise(SIGTERM); _exit(123); }
    int st = 0; waitpid(pid, &st, 0);
    printf("4. child default-term: WIFSIGNALED=%d sig=%d\n",
           WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);

    /* 5. kill a process spinning in userspace. */
    pid = fork();
    if (pid == 0) { for (;;) { } }
    kill(pid, SIGTERM);
    st = 0; waitpid(pid, &st, 0);
    printf("5. killed spinner: WIFSIGNALED=%d sig=%d\n",
           WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0);

    /* 6. catch a SIGSEGV and recover via siglongjmp. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segv;
    sigaction(SIGSEGV, &sa, NULL);
    if (sigsetjmp(jb, 1) == 0) {
        volatile int *p = (volatile int *)0x1;   /* unmapped */
        *p = 5;
        printf("6. BUG: no fault\n");
    } else {
        printf("6. SIGSEGV caught and recovered: YES\n");
    }

    printf("sigtest done\n");
    return 0;
}
