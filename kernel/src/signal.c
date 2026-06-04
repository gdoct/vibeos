#include "kernel.h"
#include "regs.h"
#include "task.h"
#include "paging.h"
#include "usermode.h"
#include "signal.h"

/*
 * Signals with Linux semantics (ROADMAP §3).
 *
 * Delivery happens on the way back to userspace: syscall_dispatch and
 * irq_dispatch call signals_deliver_*. A custom handler is entered on a
 * Linux-compatible rt_sigframe (so musl's __restore_rt + rt_sigreturn round-trip
 * works); a default action terminates (term/core), is ignored, or stops/conts
 * the process. kill/tgkill set a pending bit on the target and wake it (a
 * cross-core running target gets delivered by its own next timer tick).
 */

/* ---- canonical register snapshot (matches sigcontext greg order) ---- */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
} mctx_t;

/* Linux x86-64 user signal-frame layout (so libc restorers/handlers agree). */
typedef struct {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
    uint64_t eflags;
    uint16_t cs, gs, fs, __pad0;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;          /* &fpstate, 0 = none */
    uint64_t reserved[8];
} u_sigcontext_t;

typedef struct { uint64_t ss_sp; int32_t ss_flags; int32_t __pad; uint64_t ss_size; } u_stack_t;

typedef struct {
    uint64_t        uc_flags;
    uint64_t        uc_link;
    u_stack_t       uc_stack;
    u_sigcontext_t  uc_mcontext;
    uint8_t         uc_sigmask[128];   /* sigset_t */
} u_ucontext_t;

typedef struct {
    uint64_t     pretcode;             /* return address = sa_restorer */
    u_ucontext_t uc;
    uint8_t      info[128];            /* siginfo_t */
} u_rt_sigframe_t;

/* ---- frame <-> mctx adapters ---- */

static void mctx_from_syscall(const syscall_frame_t *f, mctx_t *m) {
    m->r8 = f->r8; m->r9 = f->r9; m->r10 = f->r10; m->r11 = f->r11;
    m->r12 = f->r12; m->r13 = f->r13; m->r14 = f->r14; m->r15 = f->r15;
    m->rdi = f->rdi; m->rsi = f->rsi; m->rbp = f->rbp; m->rbx = f->rbx;
    m->rdx = f->rdx; m->rax = f->rax; m->rcx = f->rcx;
    m->rsp = f->user_rsp; m->rip = f->rcx;   /* syscall: return rip is in rcx */
    m->eflags = f->r11;                      /* and user rflags in r11 */
}
static void mctx_to_syscall(const mctx_t *m, syscall_frame_t *f) {
    f->r8 = m->r8; f->r9 = m->r9; f->r10 = m->r10;
    f->r12 = m->r12; f->r13 = m->r13; f->r14 = m->r14; f->r15 = m->r15;
    f->rdi = m->rdi; f->rsi = m->rsi; f->rbp = m->rbp; f->rbx = m->rbx;
    f->rdx = m->rdx; f->rax = m->rax;
    f->rcx = m->rip;            /* sysret target */
    f->r11 = m->eflags;         /* user rflags */
    f->user_rsp = m->rsp;
}
static void mctx_from_regs(const regs_t *r, mctx_t *m) {
    m->r8 = r->r8; m->r9 = r->r9; m->r10 = r->r10; m->r11 = r->r11;
    m->r12 = r->r12; m->r13 = r->r13; m->r14 = r->r14; m->r15 = r->r15;
    m->rdi = r->rdi; m->rsi = r->rsi; m->rbp = r->rbp; m->rbx = r->rbx;
    m->rdx = r->rdx; m->rax = r->rax; m->rcx = r->rcx;
    m->rsp = r->rsp; m->rip = r->rip; m->eflags = r->rflags;
}
static void mctx_to_regs(const mctx_t *m, regs_t *r) {
    r->r8 = m->r8; r->r9 = m->r9; r->r10 = m->r10; r->r11 = m->r11;
    r->r12 = m->r12; r->r13 = m->r13; r->r14 = m->r14; r->r15 = m->r15;
    r->rdi = m->rdi; r->rsi = m->rsi; r->rbp = m->rbp; r->rbx = m->rbx;
    r->rdx = m->rdx; r->rax = m->rax; r->rcx = m->rcx;
    r->rsp = m->rsp; r->rip = m->rip; r->rflags = m->eflags;
}

/* ---- lifecycle ---- */

void signals_init(task_t *t) {
    kmemset(&t->sig, 0, sizeof t->sig);
}

void signals_fork(task_t *child, task_t *parent) {
    child->sig = parent->sig;     /* inherit handlers + mask + altstack */
    child->sig.pending = 0;       /* but not pending signals */
    child->sig.on_altstack = 0;
}

void signals_exec(task_t *t) {
    /* Reset caught signals to default; ignored stay ignored; keep the mask. */
    for (int i = 0; i < SIG_NSIG; i++) {
        if (t->sig.act[i].handler != SIG_IGN) t->sig.act[i].handler = SIG_DFL;
        t->sig.act[i].flags = 0;
        t->sig.act[i].restorer = 0;
        t->sig.act[i].mask = 0;
    }
    t->sig.alt_sp = t->sig.alt_size = 0;
    t->sig.alt_flags = 0;
    t->sig.on_altstack = 0;
}

/* ---- default dispositions ---- */

enum { DFL_TERM, DFL_IGN, DFL_CORE, DFL_STOP, DFL_CONT };

static int default_disp(int sig) {
    switch (sig) {
    case SIGCHLD: case SIGURG: case SIGWINCH:              return DFL_IGN;
    case SIGCONT:                                          return DFL_CONT;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:return DFL_STOP;
    case SIGSEGV: case SIGILL: case SIGFPE: case SIGABRT:
    case SIGBUS:  case SIGQUIT:                            return DFL_CORE;
    default:                                               return DFL_TERM;
    }
}

/* ---- raising ---- */

void signals_raise(task_t *t, int sig) {
    if (sig < 1 || sig > SIG_NSIG) return;

    /* SIGCONT resumes a stopped task regardless of its handler. */
    if (sig == SIGCONT && t->stopped) task_cont(t);

    if (t->sig.act[sig - 1].handler == SIG_IGN && !(SIGBIT(sig) & SIG_UNMASKABLE))
        return;                                   /* explicitly ignored: drop */
    if (t->sig.act[sig - 1].handler == SIG_DFL &&
        default_disp(sig) == DFL_IGN)
        return;                                   /* default-ignored: drop */

    /* Atomic: a kill on another core must not lose a concurrent set/clear of the
       target's pending mask (ROADMAP §"User tasks on all cores"). */
    __atomic_or_fetch(&t->sig.pending, SIGBIT(sig), __ATOMIC_ACQ_REL);

    /* Nudge the target so it delivers promptly. If it sleeps in an
       interruptible wait it bails with -EINTR and delivers on the way out; if it
       is running on another core its next timer tick delivers. */
    if (!(SIGBIT(sig) & t->sig.blocked) || (SIGBIT(sig) & SIG_UNMASKABLE))
        task_signal_wake(t);
}

/* True if the current task has a signal ready to deliver (used by interruptible
   kernel waits, e.g. tty_read, to return -EINTR). */
extern "C" int signals_pending_current(void) {
    task_t *t = task_current();
    if (!t) return 0;
    return (t->sig.pending & ~t->sig.blocked) != 0;
}

/* ---- handler frame setup ---- */

static int pick_signal(task_t *t) {
    uint64_t set = t->sig.pending & ~t->sig.blocked;
    if (!set) return 0;
    for (int s = 1; s <= SIG_NSIG; s++)
        if (set & SIGBIT(s)) return s;
    return 0;
}

/* Build the rt_sigframe on the user stack and redirect `m` into the handler.
   Returns 0 on success, -1 if the user stack is unusable (caller then forces a
   fatal default action). */
static int setup_handler(task_t *t, int sig, ksigaction_t *sa, mctx_t *m) {
    uint64_t sp = m->rsp;
    int use_alt = (sa->flags & SA_ONSTACK) && t->sig.alt_sp && !t->sig.on_altstack
                  && !(t->sig.alt_flags & SS_DISABLE);
    if (use_alt) sp = t->sig.alt_sp + t->sig.alt_size;

    u_rt_sigframe_t fr;
    kmemset(&fr, 0, sizeof fr);
    fr.pretcode = sa->restorer;
    fr.uc.uc_stack.ss_sp    = t->sig.alt_sp;
    fr.uc.uc_stack.ss_size  = t->sig.alt_size;
    fr.uc.uc_stack.ss_flags = t->sig.on_altstack ? SS_ONSTACK : t->sig.alt_flags;
    u_sigcontext_t *sc = &fr.uc.uc_mcontext;
    sc->r8=m->r8; sc->r9=m->r9; sc->r10=m->r10; sc->r11=m->r11;
    sc->r12=m->r12; sc->r13=m->r13; sc->r14=m->r14; sc->r15=m->r15;
    sc->rdi=m->rdi; sc->rsi=m->rsi; sc->rbp=m->rbp; sc->rbx=m->rbx;
    sc->rdx=m->rdx; sc->rax=m->rax; sc->rcx=m->rcx; sc->rsp=m->rsp; sc->rip=m->rip;
    sc->eflags=m->eflags;
    *(uint64_t *)fr.uc.uc_sigmask = t->sig.blocked;   /* mask to restore */
    *(uint32_t *)fr.info = (uint32_t)sig;             /* siginfo.si_signo */

    /* Place the frame: 16-align the body, then drop 8 so the handler entry rsp
       is 8 (mod 16), as the SysV ABI requires at a call boundary. */
    sp -= sizeof fr;
    sp &= ~15ULL;
    sp -= 8;
    if (copy_to_user(t->vm, sp, &fr, sizeof fr) < 0) return -1;

    /* Block this signal (+ sa->mask) for the duration of the handler. */
    t->sig.blocked |= sa->mask;
    if (!(sa->flags & SA_NODEFER)) t->sig.blocked |= SIGBIT(sig);
    if (use_alt) t->sig.on_altstack = 1;

    m->rip = sa->handler;
    m->rsp = sp;
    m->rdi = (uint64_t)sig;                                  /* arg1: signo */
    m->rsi = sp + __builtin_offsetof(u_rt_sigframe_t, info); /* arg2: siginfo* */
    m->rdx = sp + __builtin_offsetof(u_rt_sigframe_t, uc);   /* arg3: ucontext* */

    if (sa->flags & SA_RESETHAND) sa->handler = SIG_DFL;
    return 0;
}

/* Core delivery against a canonical mctx. Returns 1 if a handler was set up
   (caller writes `m` back to its frame), 0 if nothing was delivered. May not
   return (fatal default action / stop). */
static int deliver(task_t *t, mctx_t *m) {
    for (;;) {
        int sig = pick_signal(t);
        if (!sig) return 0;
        __atomic_and_fetch(&t->sig.pending, ~SIGBIT(sig), __ATOMIC_ACQ_REL);
        ksigaction_t *sa = &t->sig.act[sig - 1];

        if (sa->handler == SIG_IGN) continue;     /* shouldn't be pending, but be safe */
        if (sa->handler != SIG_DFL) {             /* custom handler */
            if (setup_handler(t, sig, sa, m) == 0) return 1;
            sa->handler = SIG_DFL;                /* bad stack: fall through to fatal */
        }
        switch (default_disp(sig)) {              /* SIG_DFL */
        case DFL_IGN:  continue;
        case DFL_CONT: continue;
        case DFL_STOP: task_stop_current(sig); continue;   /* resumes on SIGCONT */
        case DFL_TERM:
        case DFL_CORE: task_exit_signal(sig);     /* does not return */
        }
    }
}

void signals_deliver_syscall(syscall_frame_t *f) {
    task_t *t = task_current();
    if (!t || !t->vm) return;
    if (!(t->sig.pending & ~t->sig.blocked)) return;
    mctx_t m;
    mctx_from_syscall(f, &m);
    if (deliver(t, &m)) mctx_to_syscall(&m, f);
}

void signals_deliver_regs(regs_t *r) {
    task_t *t = task_current();
    if (!t || !t->vm) return;
    if ((r->cs & 3) != 3) return;                 /* only when returning to ring 3 */
    if (!(t->sig.pending & ~t->sig.blocked)) return;
    mctx_t m;
    mctx_from_regs(r, &m);
    if (deliver(t, &m)) mctx_to_regs(&m, r);
}

void signals_raise_fault(regs_t *r, int sig) {
    task_t *t = task_current();
    __atomic_or_fetch(&t->sig.pending, SIGBIT(sig), __ATOMIC_ACQ_REL);
    mctx_t m;
    mctx_from_regs(r, &m);
    /* A synchronous fault can't be ignored away: if the handler is default or
       the signal is blocked, force the fatal/default action now. */
    ksigaction_t *sa = &t->sig.act[sig - 1];
    if (sa->handler == SIG_DFL || sa->handler == SIG_IGN ||
        (SIGBIT(sig) & t->sig.blocked)) {
        task_exit_signal(sig);                    /* does not return */
    }
    if (deliver(t, &m)) mctx_to_regs(&m, r);      /* enter the handler; retry */
}

uint64_t signals_sigreturn(syscall_frame_t *f) {
    task_t *t = task_current();
    uint64_t ucaddr = f->user_rsp;                /* &ucontext (see frame layout) */
    u_ucontext_t uc;
    if (copy_from_user(&uc, t->vm, ucaddr, sizeof uc) < 0)
        task_exit_signal(SIGSEGV);                /* corrupt frame */
    u_sigcontext_t *sc = &uc.uc_mcontext;
    mctx_t m;
    m.r8=sc->r8; m.r9=sc->r9; m.r10=sc->r10; m.r11=sc->r11;
    m.r12=sc->r12; m.r13=sc->r13; m.r14=sc->r14; m.r15=sc->r15;
    m.rdi=sc->rdi; m.rsi=sc->rsi; m.rbp=sc->rbp; m.rbx=sc->rbx;
    m.rdx=sc->rdx; m.rax=sc->rax; m.rcx=sc->rcx; m.rsp=sc->rsp; m.rip=sc->rip;
    m.eflags=sc->eflags;
    mctx_to_syscall(&m, f);
    t->sig.blocked = *(uint64_t *)uc.uc_sigmask & ~SIG_UNMASKABLE;
    t->sig.on_altstack = 0;
    return m.rax;
}

/* ---- syscalls ---- */

#define EFAULT_ 14
#define EINVAL_ 22
#define ESRCH_  3
#define EPERM_  1

/* userspace struct sigaction (kernel ABI: handler, flags, restorer, mask). */
struct k_user_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

int64_t sys_rt_sigaction(int sig, const void *uact, void *uoldact, uint64_t sigsetsize) {
    if (sig < 1 || sig > SIG_NSIG || sigsetsize != 8) return -EINVAL_;
    if (sig == SIGKILL || sig == SIGSTOP) {
        if (uact) return -EINVAL_;                 /* can't change these */
    }
    task_t *t = task_current();
    ksigaction_t *cur = &t->sig.act[sig - 1];

    if (uoldact) {
        struct k_user_sigaction old = { cur->handler, cur->flags, cur->restorer, cur->mask };
        if (copy_to_user(t->vm, (uint64_t)(uintptr_t)uoldact, &old, sizeof old) < 0)
            return -EFAULT_;
    }
    if (uact) {
        struct k_user_sigaction na;
        if (copy_from_user(&na, t->vm, (uint64_t)(uintptr_t)uact, sizeof na) < 0)
            return -EFAULT_;
        cur->handler  = na.handler;
        cur->flags    = na.flags;
        cur->restorer = na.restorer;
        cur->mask     = na.mask & ~SIG_UNMASKABLE;
    }
    return 0;
}

int64_t sys_rt_sigprocmask(int how, const void *uset, void *uoldset, uint64_t sigsetsize) {
    if (sigsetsize != 8) return -EINVAL_;
    task_t *t = task_current();
    if (uoldset) {
        uint64_t old = t->sig.blocked;
        if (copy_to_user(t->vm, (uint64_t)(uintptr_t)uoldset, &old, 8) < 0) return -EFAULT_;
    }
    if (uset) {
        uint64_t set;
        if (copy_from_user(&set, t->vm, (uint64_t)(uintptr_t)uset, 8) < 0) return -EFAULT_;
        switch (how) {
        case SIG_BLOCK:   t->sig.blocked |= set; break;
        case SIG_UNBLOCK: t->sig.blocked &= ~set; break;
        case SIG_SETMASK: t->sig.blocked = set; break;
        default: return -EINVAL_;
        }
        t->sig.blocked &= ~SIG_UNMASKABLE;         /* can't block KILL/STOP */
    }
    return 0;
}

int64_t sys_rt_sigpending(void *uset, uint64_t sigsetsize) {
    if (sigsetsize != 8) return -EINVAL_;
    task_t *t = task_current();
    uint64_t p = t->sig.pending;
    if (copy_to_user(t->vm, (uint64_t)(uintptr_t)uset, &p, 8) < 0) return -EFAULT_;
    return 0;
}

int64_t sys_sigaltstack(const void *uss, void *uoldss) {
    task_t *t = task_current();
    if (uoldss) {
        u_stack_t old = { t->sig.alt_sp, t->sig.alt_flags, 0, t->sig.alt_size };
        if (!t->sig.alt_sp) old.ss_flags |= SS_DISABLE;
        if (t->sig.on_altstack) old.ss_flags |= SS_ONSTACK;
        if (copy_to_user(t->vm, (uint64_t)(uintptr_t)uoldss, &old, sizeof old) < 0)
            return -EFAULT_;
    }
    if (uss) {
        if (t->sig.on_altstack) return -16;        /* -EPERM-ish: in use */
        u_stack_t ss;
        if (copy_from_user(&ss, t->vm, (uint64_t)(uintptr_t)uss, sizeof ss) < 0)
            return -EFAULT_;
        if (ss.ss_flags & SS_DISABLE) {
            t->sig.alt_sp = t->sig.alt_size = 0; t->sig.alt_flags = 0;
        } else {
            t->sig.alt_sp = ss.ss_sp; t->sig.alt_size = ss.ss_size; t->sig.alt_flags = 0;
        }
    }
    return 0;
}

static int64_t do_kill(int pid, int sig) {
    if (sig < 0 || sig > SIG_NSIG) return -EINVAL_;

    /* Process-group / broadcast targets (ROADMAP §"Interactive I/O"):
         pid  > 0 : that process
         pid == 0 : the caller's process group
         pid <  -1: the process group -pid                                       */
    if (pid <= 0) {
        int pgrp = (pid == 0) ? task_pgid(task_current()) : -pid;
        if (sig == 0) return 0;                    /* existence check (best effort) */
        tasks_signal_pgrp(pgrp, sig);
        return 0;
    }

    task_t *target = task_by_id(pid);
    if (!target || !target->vm) return -ESRCH_;
    if (sig == 0) return 0;                        /* existence check */
    signals_raise(target, sig);
    return 0;
}

int64_t sys_kill(int pid, int sig)            { return do_kill(pid, sig); }
int64_t sys_tgkill(int tgid, int tid, int sig) { (void)tgid; return do_kill(tid, sig); }
