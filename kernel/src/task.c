#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "paging.h"
#include "task.h"
#include "usermode.h"
#include "timer.h"
#include "smp.h"

/*
 * SMP scheduler (ROADMAP §1, stage B), xv6-style.
 *
 * Each CPU runs its own scheduler() loop on its boot/AP stack. Tasks never
 * switch directly to each other; they switch to/from the per-CPU scheduler
 * context. A single sched_lock protects all task state (the `state` field,
 * the wait queues, the sleeper list, and each CPU's `current`) and is held
 * *across* every context switch — handed off like a baton: the side that
 * resumes after a switch is responsible for releasing it (task_trampoline /
 * fork_child_return on first run; the scheduler / yielding task otherwise).
 *
 * Interrupt state is tracked per-CPU (push_off/pop_off, à la xv6 push/popcli)
 * rather than stored in the lock, because the lock crosses context switches —
 * so the IF baton must follow the CPU, not the lock word.
 *
 * Scope: kernel tasks run on any CPU; user tasks are pinned to the BSP, so the
 * ring-3 syscall/IRQ path keeps using one global kernel-stack latch (no swapgs
 * yet). Lifting that is the follow-on.
 */

#define KSTACK_PAGES   4   /* 16 KiB kernel stack per task */

extern "C" void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern "C" __attribute__((noreturn)) void task_trampoline(void);
extern "C" void fork_child_return(void);

static task_t   g_tasks[MAX_TASKS];   /* slot 0 unused (no boot task in this model) */
static int      g_started = 0;

/* Per-CPU scheduler state. */
static uint64_t g_sched_rsp[SMP_MAX_CPUS];   /* the scheduler loop's saved rsp */
static task_t  *g_cpu_cur[SMP_MAX_CPUS];     /* running task (NULL while in scheduler) */

/* Tasks asleep in ksleep, linked through wq_next; under sched_lock. */
static task_t  *g_sleepers = nullptr;

/* ---- the one lock guarding task state, held across switches ---- */
static volatile uint32_t sched_locked = 0;
static int g_ncli[SMP_MAX_CPUS];     /* push_off nesting depth, per CPU */
static int g_intena[SMP_MAX_CPUS];   /* was IF set before the first push_off */

static void push_off(void) {
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    int c = smp_cpu_index();
    if (g_ncli[c] == 0) g_intena[c] = (int)((f >> 9) & 1);
    g_ncli[c]++;
}
static void pop_off(void) {
    int c = smp_cpu_index();
    if (--g_ncli[c] == 0 && g_intena[c]) __asm__ volatile("sti");
}
static void sched_lock(void) {
    push_off();
    while (__atomic_exchange_n(&sched_locked, 1u, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}
/* Exported so fork_child_return (usermode.S) can release the baton on a child's
   first run, and so timer.c can wrap the tick under the same lock. */
extern "C" void sched_unlock(void) {
    __atomic_store_n(&sched_locked, 0u, __ATOMIC_RELEASE);
    pop_off();
}

task_t *task_current(void) { return g_cpu_cur[smp_cpu_index()]; }
int     sched_active(void) { return g_started; }

void sched_init(void) {
    kmemset(g_tasks, 0, sizeof(g_tasks));
    g_started = 1;
    kprintf("[sched] SMP scheduler initialized\n");
}

/* ---- task creation ---- */

static task_t *alloc_task_slot(const char *name) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (g_tasks[i].state == TASK_NONE || g_tasks[i].state == TASK_DEAD) { slot = i; break; }
    if (slot < 0) panic("task: pool full");

    uint64_t base;
    uint64_t top = kstack_alloc(KSTACK_PAGES, &base);

    task_t *t = &g_tasks[slot];
    kmemset(t, 0, sizeof(*t));
    t->id = slot; t->name = name;
    t->stack_base = base; t->stack_top = top;
    return t;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg) {
    sched_lock();
    task_t *t = alloc_task_slot(name);
    t->entry = entry;
    t->arg   = arg;

    /* Synthetic stack: context_switch restores 6 zeroed callee-saved regs and
       `ret`s into task_trampoline. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)task_trampoline;
    for (int j = 0; j < 6; j++) *--sp = 0;
    t->saved_rsp = (uint64_t)sp;

    t->state = TASK_READY;
    int id = t->id;
    sched_unlock();
    kprintf("[sched] task %d \"%s\" entry=%p\n", id, name, (void *)(uintptr_t)entry);
    return t;
}

void task_set_vmspace(struct vmspace *vm) {
    task_t *t = task_current();
    t->vm = vm;
    uint64_t cr3 = vm ? vm->pml4_phys : paging_kernel_pml4();
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

task_t *task_fork(const char *name, struct vmspace *vm,
                  const struct syscall_frame *frame) {
    task_t *parent = task_current();
    sched_lock();
    task_t *t = alloc_task_slot(name);
    t->vm        = vm;
    t->parent    = parent;
    t->brk_start = parent->brk_start;
    t->brk_cur   = parent->brk_cur;
    t->brk_max   = parent->brk_max;

    /* Craft the child stack: context_switch -> fork_child_return, which finds a
       copy of the parent's syscall frame (rax forced to 0) and sysrets. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = frame->user_rsp;
    *--sp = frame->r11;
    *--sp = frame->rcx;
    *--sp = frame->r9;
    *--sp = frame->r8;
    *--sp = frame->r10;
    *--sp = frame->rdx;
    *--sp = frame->rsi;
    *--sp = frame->rdi;
    *--sp = 0;                                /* child sees fork() == 0 */
    *--sp = (uint64_t)fork_child_return;
    for (int j = 0; j < 6; j++) *--sp = 0;
    t->saved_rsp = (uint64_t)sp;

    t->state = TASK_READY;
    int id = t->id, pid = parent->id;
    sched_unlock();
    kprintf("[sched] fork: %d \"%s\" -> child %d\n", pid, parent->name, id);
    return t;
}

/* ---- the scheduler ---- */

static void apply_task_mm(task_t *t) {
    uint64_t cr3 = t->vm ? t->vm->pml4_phys : paging_kernel_pml4();
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    if (t->vm) {                 /* user task (BSP-pinned): set the kernel stack */
        tss_set_rsp0(t->stack_top);
        g_kernel_rsp = t->stack_top;
    }
}

/* Pick a READY task. User tasks (vm != NULL) only run on the BSP for now. */
static task_t *pick_ready(int cpu) {
    for (int i = 1; i < MAX_TASKS; i++) {
        task_t *t = &g_tasks[i];
        if (t->state != TASK_READY) continue;
        if (t->vm && cpu != 0) continue;
        return t;
    }
    return nullptr;
}

/* Switch from the current task back to this CPU's scheduler. Caller holds the
   lock; it is still held when the task is later resumed here. */
static void sched(void) {
    int c = smp_cpu_index();
    context_switch(&g_cpu_cur[c]->saved_rsp, g_sched_rsp[c]);
}

__attribute__((noreturn))
void scheduler(void) {
    int c = smp_cpu_index();
    g_cpu_cur[c] = nullptr;
    for (;;) {
        __asm__ volatile("sti");          /* let IRQs make tasks runnable */
        sched_lock();
        task_t *t = pick_ready(c);
        if (t) {
            t->state = TASK_RUNNING;
            g_cpu_cur[c] = t;
            apply_task_mm(t);
            context_switch(&g_sched_rsp[c], t->saved_rsp);  /* run t (lock held) */
            g_cpu_cur[c] = nullptr;       /* t returned via sched() */
        }
        sched_unlock();
        if (!t) __asm__ volatile("hlt");  /* nothing runnable; wait for an IRQ */
    }
}

extern "C" __attribute__((noreturn))
void task_trampoline(void) {
    sched_unlock();                  /* release the baton the scheduler handed us */
    task_t *t = task_current();
    t->entry(t->arg);
    task_exit();
    __builtin_unreachable();
}

/* ---- yield / exit / wait ---- */

void task_yield(void) {
    if (!g_started) return;
    sched_lock();
    task_t *t = g_cpu_cur[smp_cpu_index()];
    if (t) t->state = TASK_READY;
    sched();
    sched_unlock();
}

void task_exit(void) {
    task_t *t = task_current();
    kprintf("[sched] task %d \"%s\" exited\n", t->id, t->name);
    sched_lock();
    t->state = TASK_DEAD;
    sched();
    panic("task_exit: returned");
}

/* wait-queue internals (assume sched_lock held). */
static void make_ready_locked(task_t *t) {
    if (t->state == TASK_BLOCKED) t->state = TASK_READY;
}
static void wq_sleep_locked(wait_queue_t *wq) {
    task_t *t = g_cpu_cur[smp_cpu_index()];
    t->wq_next = nullptr;
    if (wq->tail) wq->tail->wq_next = t; else wq->head = t;
    wq->tail = t;
    t->state = TASK_BLOCKED;
    sched();                          /* lock held; held again when resumed */
}
static void wq_wake_all_locked(wait_queue_t *wq) {
    task_t *t = wq->head;
    while (t) { task_t *n = t->wq_next; t->wq_next = nullptr; make_ready_locked(t); t = n; }
    wq->head = wq->tail = nullptr;
}

void task_exit_user(int code) {
    task_t *t = task_current();
    kprintf("[sched] task %d \"%s\" exit(%d)\n", t->id, t->name, code);
    sched_lock();
    t->exit_code = code;
    task_t *p = t->parent;
    if (p && p->state != TASK_NONE && p->state != TASK_DEAD) {
        t->state = TASK_ZOMBIE;       /* parent's wait4 reaps */
        wq_wake_all_locked(&p->child_wq);
    } else {
        t->state = TASK_DEAD;
    }
    sched();
    panic("task_exit_user: returned");
}

int task_wait(int *status) {
    sched_lock();
    task_t *p = g_cpu_cur[smp_cpu_index()];
    for (;;) {
        int have_child = 0;
        for (int i = 1; i < MAX_TASKS; i++) {
            task_t *c = &g_tasks[i];
            if (c->parent != p) continue;
            if (c->state == TASK_ZOMBIE) {
                int pid = c->id, code = c->exit_code;
                struct vmspace *vm = c->vm;
                c->state = TASK_DEAD; c->parent = nullptr; c->vm = nullptr;
                sched_unlock();
                if (vm) vmspace_destroy(vm);   /* zombie won't run again */
                if (status) *status = code;
                return pid;
            }
            if (c->state != TASK_DEAD && c->state != TASK_NONE) have_child = 1;
        }
        if (!have_child) { sched_unlock(); return -10; }   /* -ECHILD */
        wq_sleep_locked(&p->child_wq);
    }
}

/* ---- wait queues (public; acquire the lock) ---- */

void wait_queue_sleep(wait_queue_t *wq)    { sched_lock(); wq_sleep_locked(wq); sched_unlock(); }
void wait_queue_wake_one(wait_queue_t *wq) {
    sched_lock();
    task_t *t = wq->head;
    if (t) { wq->head = t->wq_next; if (!wq->head) wq->tail = nullptr; t->wq_next = nullptr; make_ready_locked(t); }
    sched_unlock();
}
void wait_queue_wake_all(wait_queue_t *wq) { sched_lock(); wq_wake_all_locked(wq); sched_unlock(); }

/* ---- timer-driven sleep + preemption (called from timer.c) ---- */

void task_sleep_ticks(uint64_t wake_tick) {
    if (!g_started) {                 /* pre-scheduler (early boot, BSP only) */
        while (timer_ticks() < wake_tick) __asm__ volatile("hlt");
        return;
    }
    sched_lock();
    task_t *t = g_cpu_cur[smp_cpu_index()];
    t->wake_tick = wake_tick;
    t->wq_next = g_sleepers; g_sleepers = t;
    t->state = TASK_BLOCKED;
    sched();
    sched_unlock();
}

/* Per-tick: wake due sleepers and preempt the running task. From the timer
   IRQ (IF off). */
void task_tick(void) {
    if (!g_started) return;
    sched_lock();
    uint64_t now = timer_ticks();
    task_t **pp = &g_sleepers;
    while (*pp) {
        task_t *t = *pp;
        if (t->state == TASK_BLOCKED && t->wake_tick <= now) {
            *pp = t->wq_next; t->wq_next = nullptr; t->state = TASK_READY;
        } else {
            pp = &t->wq_next;
        }
    }
    task_t *cur = g_cpu_cur[smp_cpu_index()];
    if (cur && cur->state == TASK_RUNNING) {
        cur->state = TASK_READY;
        sched();                      /* preempt; resumes here when rescheduled */
    }
    sched_unlock();
}
