#ifndef VIBEOS_TASK_H
#define VIBEOS_TASK_H

#include <stdint.h>
#include <stddef.h>
#include "signal.h"
#include "spinlock.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TASKS  24       /* kernel daemons (net worker/timer/servers) + user procs */
#define VFS_MAX_FD 32       /* per-process file descriptors (0/1/2 = console) */

typedef enum {
    TASK_NONE    = 0,
    TASK_READY   = 1,
    TASK_RUNNING = 2,
    TASK_BLOCKED = 3,   /* off the run queue until something wakes it */
    TASK_DEAD    = 4,   /* finished and reaped — slot reusable */
    TASK_ZOMBIE  = 5,   /* exited, awaiting parent wait4() to collect status */
} task_state_t;

struct vmspace;          /* kernel/include/paging.h */
struct syscall_frame;    /* kernel/include/usermode.h */
struct file;             /* kernel/include/file.h */

/*
 * A wait queue is just a FIFO of blocked tasks. wait_queue_sleep blocks
 * the caller on it; wake_one / wake_all move queued tasks back to READY.
 * Defined before task_t so a task can embed one (its child-exit queue).
 */
typedef struct wait_queue {
    struct task *head;
    struct task *tail;
} wait_queue_t;

typedef struct task {
    /* saved_rsp MUST be at offset 0 — context_switch.S touches it via
       (%rdi) without any C-side offsetof games. */
    uint64_t      saved_rsp;
    task_state_t  state;
    int           id;
    const char   *name;
    void        (*entry)(void *);
    void         *arg;
    uint64_t      stack_base;
    uint64_t      stack_top;

    /* Intrusive link used while the task sits on a wait queue or the
       timer sleeper list. Only meaningful when state == TASK_BLOCKED. */
    struct task  *wq_next;
    uint64_t      wake_tick;    /* absolute tick deadline for ksleep_ms */

    /* Per-CPU run-queue link + affinity (ROADMAP §3). rq_next threads this task
       through its home CPU's ready queue; only meaningful when state==READY.
       home_cpu is the CPU it last ran on — its scheduler re-queues it there, and
       an idle CPU steals it only when its own queue is empty. Both kernel and
       user tasks are stealable now (ROADMAP §"User tasks on all cores"); the
       ring-3 syscall/fd/pipe/mm paths carry their own locks. */
    struct task  *rq_next;
    int           home_cpu;

    /* User-process fields (NULL/0 for kernel threads). The scheduler loads
       `vm`'s CR3 on switch; brk_* track the heap; parent/exit_code/child_wq
       feed wait4 (a parent sleeps on child_wq until a child becomes ZOMBIE). */
    struct vmspace *vm;
    uint64_t      brk_start, brk_cur, brk_max;
    uint64_t      fs_base;      /* TLS base (arch_prctl ARCH_SET_FS); per-task FS_BASE MSR */
    struct task  *parent;
    int           exit_code;
    int           term_signal;  /* nonzero if killed by a signal (wait4 status) */
    int           stopped;      /* job-control stop (SIGSTOP/SIGTSTP) in effect */
    wait_queue_t  child_wq;

    /* Signal disposition table, masks, pending set, altstack (ROADMAP §3). */
    task_sigstate_t sig;

    /* Per-process fd table (ROADMAP §4 rung 2). Indices 0/1/2 are the serial
       console implicitly; 3+ point at refcounted open-file objects. fork() dups
       (copies) the table; clone(CLONE_FILES) shares it (refcounted). */
    struct fdtable *files;

    /* Threads (ROADMAP). A thread group shares a vmspace + fdtable + sighand and
       a tgid (the leader's id). `is_thread` marks a CLONE_THREAD task: it does
       not zombie at exit — it clears *clear_child_tid and futex-wakes it (how
       pthread_join detects exit), then frees. */
    int           tgid;             /* thread-group id (== getpid()); 0 = use id */
    int           is_thread;
    uint64_t      clear_child_tid;  /* CLONE_CHILD_CLEARTID address (user) or 0 */

    /* Current working directory (ROADMAP §4): an absolute, normalized path that
       relative path resolution is taken from. Inherited across fork + execve. */
    char          cwd[256];
} task_t;

/* A refcounted descriptor table so threads (CLONE_FILES) can share one. `ref` is
   touched with atomics; `lock` guards the fd[]/cloexec[] slot arrays so two
   threads of one process can install/close/dup descriptors on different cores
   without corrupting the table or double-unref'ing a slot. */
typedef struct fdtable {
    int           ref;
    spinlock_t    lock;
    struct file  *fd[VFS_MAX_FD];
    uint8_t       cloexec[VFS_MAX_FD];
} fdtable_t;

/* Initialize the SMP scheduler. Call once on the BSP before creating tasks
   and before any CPU enters scheduler(). */
void    sched_init(void);

/* Per-CPU scheduler loop (never returns). Each CPU (BSP last, after creating
   the initial tasks; each AP from ap_entry) calls this to start scheduling. */
__attribute__((noreturn))
void    scheduler(void);

task_t *task_create(const char *name, void (*entry)(void *), void *arg);
task_t *task_current(void);   /* the task running on the calling CPU */
void    task_yield(void);

/* Create a child of the current task for fork(): a new task in address space
   `vm` whose kernel stack is crafted to return to ring 3 (rax = 0) with the
   parent's saved user state in `frame`. Inherits the parent's brk window. */
task_t *task_fork(const char *name, struct vmspace *vm,
                  const struct syscall_frame *frame);

/* clone(2): create a task that may share the caller's address space, fd table,
   and thread group (ROADMAP). `share_files` shares the fdtable; `vm` is the
   address space (shared for CLONE_VM, a fresh COW copy otherwise); `is_thread`
   marks CLONE_THREAD; the child returns to userspace with rsp=`user_stack` and,
   if `tls`!=0, FS_BASE=`tls`. */
task_t *task_clone(const char *name, struct vmspace *vm,
                   const struct syscall_frame *frame, uint64_t user_stack,
                   uint64_t tls, int share_files, int is_thread);

/* thread-group id of `t` (== getpid for a thread); falls back to its own id. */
int     task_tgid(task_t *t);

/* Attach a user address space to the current task and switch CR3 to it. */
void    task_set_vmspace(struct vmspace *vm);

__attribute__((noreturn))
void    task_exit(void);

/* User-process exit: records the code and, if a parent exists, becomes a
   ZOMBIE and wakes the parent's wait; otherwise reaps immediately. */
__attribute__((noreturn))
void    task_exit_user(int code);

/* A thread (CLONE_THREAD) exiting: no zombie — releases its fd-table + AS refs
   and dies. The caller handles CLONE_CHILD_CLEARTID + futex wake first. */
__attribute__((noreturn))
void    task_exit_thread(void);

/* futex primitives (caller holds sched_lock; key = the word's physical addr). */
void    futex_sleep_on(uint64_t key);
int     futex_wake_key(uint64_t key, int n);

/* Terminate the current user task because of signal `sig` (ROADMAP §3): like
   task_exit_user but the wait4 status reports WIFSIGNALED instead of an exit
   code. Does not return. */
__attribute__((noreturn))
void    task_exit_signal(int sig);

/* Signal-support helpers (used by signal.c). */
task_t *task_by_id(int id);          /* live task with this pid, or NULL */
int     task_running_cpu(task_t *t); /* CPU index if RUNNING, else -1 */
void    task_signal_wake(task_t *t); /* nudge a blocked task so it can deliver */
void    task_stop_current(void);     /* job-control stop until task_cont */
void    task_cont(task_t *t);        /* undo a stop */

/* Reap one ZOMBIE child of the current task, freeing its address space and
   slot and returning its pid (with *status set to the exit code). Blocks if
   children exist but none has exited yet; returns -ECHILD if there are none. */
int     task_wait(int *status, int nohang, int wpid);  /* wpid>0: that child only; nohang: 0 if none ready */

/* Timer-driven: wake due sleepers and preempt the running task. Called from
   the tick IRQ on every CPU. No-op until sched_init has run. */
void    task_tick(void);

/* Block the current task until timer tick `wake_tick` (backs ksleep_ms). */
void    task_sleep_ticks(uint64_t wake_tick);

/* True once sched_init has run and blocking primitives are usable. */
int     sched_active(void);

/* --- wait queues (block on a condition; another path wakes you) --- */
void    wait_queue_sleep(wait_queue_t *wq);
void    wait_queue_wake_one(wait_queue_t *wq);
void    wait_queue_wake_all(wait_queue_t *wq);

/* The scheduler lock, and wait-queue ops that assume it is already held. A
   device whose completion IRQ can fire on another CPU must check its condition
   and sleep under sched_lock (and wake under it) so the check/sleep/wake are
   serialized — see virtio_blk and tty (ROADMAP §2). */
void    sched_lock(void);
void    sched_unlock(void);
void    wait_queue_sleep_locked(wait_queue_t *wq);
void    wait_queue_wake_one_locked(wait_queue_t *wq);
void    wait_queue_wake_all_locked(wait_queue_t *wq);

#ifdef __cplusplus
}
#endif

#endif
