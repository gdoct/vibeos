#ifndef VIBEOS_SIGNAL_H
#define VIBEOS_SIGNAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * POSIX/Linux signals (ROADMAP §3).
 *
 * Per-task: a 64-entry disposition table, a blocked mask, a pending mask, and an
 * alternate stack. Signals are delivered on the way back to userspace (syscall
 * and IRQ return paths) — a custom handler runs on a Linux-compatible
 * rt_sigframe and returns through rt_sigreturn; a default action terminates the
 * process (term/core) or is ignored. kill/tgkill set a pending bit on the target
 * and, if it is running on another core, poke it with an IPI so it delivers
 * promptly.
 */

#define SIG_NSIG   64

/* Signal numbers we name (Linux x86-64). */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGWINCH 28

/* sa_handler sentinels. */
#define SIG_DFL  0UL
#define SIG_IGN  1UL

/* sa_flags. */
#define SA_SIGINFO   0x00000004UL
#define SA_RESTORER  0x04000000UL
#define SA_ONSTACK   0x08000000UL
#define SA_RESTART   0x10000000UL
#define SA_NODEFER   0x40000000UL
#define SA_RESETHAND 0x80000000UL

/* sigaltstack ss_flags. */
#define SS_ONSTACK 1
#define SS_DISABLE 2

/* sigprocmask how. */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIGBIT(s)   (1ULL << ((s) - 1))
/* SIGKILL and SIGSTOP can never be caught, blocked, or ignored. */
#define SIG_UNMASKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

typedef struct ksigaction {
    uint64_t handler;    /* sa_handler / sa_sigaction (SIG_DFL / SIG_IGN / VA) */
    uint64_t flags;      /* sa_flags */
    uint64_t restorer;   /* sa_restorer (return trampoline, set by libc) */
    uint64_t mask;       /* extra signals blocked while the handler runs */
} ksigaction_t;

/* Per-task signal state, embedded in task_t. */
typedef struct task_sigstate {
    ksigaction_t act[SIG_NSIG];
    uint64_t     blocked;     /* currently blocked mask */
    uint64_t     pending;     /* delivered-but-not-yet-handled mask */
    uint64_t     alt_sp;      /* sigaltstack base */
    uint64_t     alt_size;
    int          alt_flags;
    int          on_altstack; /* a handler is currently running on the altstack */
} task_sigstate_t;

struct task;
struct syscall_frame;
struct regs;

/* Lifecycle. */
void signals_init(struct task *t);                       /* fresh: all SIG_DFL */
void signals_fork(struct task *child, struct task *parent);
void signals_exec(struct task *t);                       /* reset caught -> DFL */

/* Make `sig` pending on `t` and nudge it (wake if blocked-sleeping, IPI if it is
   running on another CPU) so it delivers soon. */
void signals_raise(struct task *t, int sig);

/* Raise a synchronous signal on the current task from a CPU fault (exception.c).
   Sets up the handler / default action against the trap frame `r`. */
void signals_raise_fault(struct regs *r, int sig);

/* Delivery checkpoints (return to userspace). Each rewrites the saved user
   context to enter a handler, performs a default action (may not return), or
   does nothing if no signal is deliverable. */
void signals_deliver_syscall(struct syscall_frame *f);
void signals_deliver_regs(struct regs *r);

/* rt_sigreturn: restore the context saved when the handler was entered. Returns
   the value to leave in the user's rax (the interrupted syscall's result). */
uint64_t signals_sigreturn(struct syscall_frame *f);

/* Nonzero if the current task has a deliverable (pending & unblocked) signal.
   Interruptible kernel waits (tty_read) use this to return -EINTR. */
int signals_pending_current(void);

/* Replace the current task's blocked mask (KILL/STOP excepted); returns the
   previous mask. Used by rt_sigsuspend for the duration of its wait. */
uint64_t signals_set_blocked_current(uint64_t mask);

/* Syscalls (wired from syscall_dispatch). */
int64_t sys_rt_sigaction(int sig, const void *uact, void *uoldact, uint64_t sigsetsize);
int64_t sys_rt_sigprocmask(int how, const void *uset, void *uoldset, uint64_t sigsetsize);
int64_t sys_rt_sigpending(void *uset, uint64_t sigsetsize);
int64_t sys_sigaltstack(const void *uss, void *uoldss);
int64_t sys_kill(int pid, int sig);
int64_t sys_tgkill(int tgid, int tid, int sig);

#ifdef __cplusplus
}
#endif

#endif
