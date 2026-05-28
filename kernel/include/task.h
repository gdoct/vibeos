#ifndef MYOS_TASK_H
#define MYOS_TASK_H

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
    TASK_DEAD    = 4,
} task_state_t;

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
} task_t;

/*
 * A wait queue is just a FIFO of blocked tasks. wait_queue_sleep blocks
 * the caller on it; wake_one / wake_all move queued tasks back to READY.
 */
typedef struct wait_queue {
    task_t *head;
    task_t *tail;
} wait_queue_t;

/* Convert the currently-running kmain context into task slot 0. Must
   be called before any task_create / task_yield. */
void    sched_init(void);

task_t *task_create(const char *name, void (*entry)(void *), void *arg);
task_t *task_current(void);
void    task_yield(void);

__attribute__((noreturn))
void    task_exit(void);

/* Called from the timer IRQ. No-op until sched_init has run. */
void    sched_tick(void);

/* True once sched_init has run and blocking primitives are usable. */
int     sched_active(void);

/* --- primitives the wait-queue and sleep machinery build on --- */

/* Mark the current task BLOCKED and switch away. MUST be called with
   interrupts disabled and the task already linked onto whatever queue
   will wake it. Returns (with interrupts still disabled) once the task
   has been made READY again and rescheduled. */
void    sched_block_and_switch(void);

/* If t is BLOCKED, move it back to READY. Safe to call from IRQ context;
   does not switch. */
void    sched_make_ready(task_t *t);

/* --- wait queues --- */

/* Block the current task on wq. Call with interrupts disabled; returns
   with interrupts still disabled once woken. */
void    wait_queue_sleep(wait_queue_t *wq);

/* Wake the task at the head of wq (FIFO) / all tasks on wq. Safe from
   any interrupt state; neither switches. */
void    wait_queue_wake_one(wait_queue_t *wq);
void    wait_queue_wake_all(wait_queue_t *wq);

#ifdef __cplusplus
}
#endif

#endif
