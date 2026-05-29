#ifndef VIBEOS_TASK_H
#define VIBEOS_TASK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_TASKS  8

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

    /* User-process fields (NULL/0 for kernel threads). The scheduler loads
       `vm`'s CR3 on switch; brk_* track the heap; parent/exit_code/child_wq
       feed wait4 (a parent sleeps on child_wq until a child becomes ZOMBIE). */
    struct vmspace *vm;
    uint64_t      brk_start, brk_cur, brk_max;
    uint64_t      fs_base;      /* TLS base (arch_prctl ARCH_SET_FS); per-task FS_BASE MSR */
    uint64_t      mmap_next;    /* bump pointer for anonymous mmap() in the user half */
    struct task  *parent;
    int           exit_code;
    wait_queue_t  child_wq;
} task_t;

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

/* Attach a user address space to the current task and switch CR3 to it. */
void    task_set_vmspace(struct vmspace *vm);

__attribute__((noreturn))
void    task_exit(void);

/* User-process exit: records the code and, if a parent exists, becomes a
   ZOMBIE and wakes the parent's wait; otherwise reaps immediately. */
__attribute__((noreturn))
void    task_exit_user(int code);

/* Reap one ZOMBIE child of the current task, freeing its address space and
   slot and returning its pid (with *status set to the exit code). Blocks if
   children exist but none has exited yet; returns -ECHILD if there are none. */
int     task_wait(int *status);

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

#ifdef __cplusplus
}
#endif

#endif
