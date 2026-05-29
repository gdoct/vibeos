#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "paging.h"
#include "task.h"
#include "usermode.h"

/*
 * Cooperative + timer-preemptive round-robin scheduler.
 *
 * Tasks live in a fixed-size pool. Slot 0 is the boot context (kmain);
 * slots 1..MAX_TASKS-1 hold tasks created via task_create. Each task
 * owns its own kernel stack and a saved RSP.
 *
 * Context switching is delegated to context_switch.S, which only saves
 * the SysV ABI's callee-saved registers. The caller-saved ones are
 * either spilled by the C compiler around the call (cooperative path)
 * or already on the kernel stack because we entered through an IRQ
 * stub (preemptive path) — so by the time the asm runs there is
 * nothing more for it to preserve.
 */

#define KSTACK_PAGES   4   /* 16 KiB kernel stack per task */

extern "C" void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern "C" __attribute__((noreturn)) void task_trampoline(void);

static task_t g_tasks[MAX_TASKS];
static int    g_current     = 0;
static int    g_idle        = -1;   /* always-runnable fallback task */
static int    g_initialized = 0;

task_t *task_current(void) { return &g_tasks[g_current]; }
int     sched_active(void) { return g_initialized; }

/* The idle task runs only when nothing else is READY. It enables
   interrupts and halts, so a CPU with no work draws no power and waits
   for the next IRQ (a timer tick or device completion) to make something
   runnable again. */
static void idle_entry(void *arg) {
    (void)arg;
    for (;;) __asm__ volatile("sti; hlt");
}

void sched_init(void) {
    kmemset(g_tasks, 0, sizeof(g_tasks));
    g_tasks[0].state = TASK_RUNNING;
    g_tasks[0].id    = 0;
    g_tasks[0].name  = "boot";
    g_current        = 0;
    g_initialized    = 1;
    kprintf("[sched] init: boot context = slot 0\n");

    task_t *idle = task_create("idle", idle_entry, nullptr);
    g_idle = idle->id;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_NONE || g_tasks[i].state == TASK_DEAD) {
            slot = i; break;
        }
    }
    if (slot < 0) panic("task_create: pool full");

    /* Stack lives in the kernel vmalloc window with an unmapped guard
       page below it, so an overflow faults cleanly instead of scribbling
       on a neighbour. stack_base/top are virtual addresses. */
    uint64_t base;
    uint64_t top = kstack_alloc(KSTACK_PAGES, &base);

    task_t *t = &g_tasks[slot];
    t->id         = slot;
    t->name       = name;
    t->entry      = entry;
    t->arg        = arg;
    t->stack_base = base;
    t->stack_top  = top;

    /* Hand-craft the stack so context_switch's restore-and-ret lands at
       task_trampoline with zeroed callee-saved registers.

       Layout from low addr (where saved_rsp will point) up:
           r15 = 0
           r14 = 0
           r13 = 0
           r12 = 0
           rbp = 0
           rbx = 0
           return address = task_trampoline
       — exactly what `pushq rbx; pushq rbp; pushq r12..r15` would have
       left if a previous context_switch had just been called from inside
       task_trampoline. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)task_trampoline;
    for (int j = 0; j < 6; j++) *--sp = 0;
    t->saved_rsp = (uint64_t)sp;

    t->state = TASK_READY;
    kprintf("[sched] task %d \"%s\" entry=%p stack=%lx..%lx\n",
            slot, name, (void *)(uintptr_t)entry,
            (unsigned long)base, (unsigned long)t->stack_top);
    return t;
}

/* Round-robin scan for a READY task after `from`, never returning the
   idle task — idle is only ever a deliberate fallback. -1 if none. */
static int pick_next_ready(int from) {
    int i = from;
    for (int n = 0; n < MAX_TASKS; n++) {
        i = (i + 1) % MAX_TASKS;
        if (i == g_idle) continue;
        if (g_tasks[i].state == TASK_READY) return i;
    }
    return -1;
}

static void switch_to(int from, int to) {
    if (to == from) return;
    task_t *prev = &g_tasks[from];
    task_t *next = &g_tasks[to];
    /* A task we're leaving goes back on the run queue only if it was
       actually running — a task that blocked or exited has already set
       its own state and must keep it. */
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    g_current   = to;
    /* Track the running task's kernel stack for ring-3 entry: the TSS rsp0
       (used when an IRQ/exception hits in ring 3) and the syscall stub's
       stack both follow it. Harmless for kernel-only tasks. The boot/idle
       slots have no allocated stack (stack_top == 0); skip those. */
    if (next->stack_top) {
        tss_set_rsp0(next->stack_top);
        g_kernel_rsp = next->stack_top;
    }
    context_switch(&prev->saved_rsp, next->saved_rsp);
}

/*
 * Pick the next READY task round-robin and switch to it. If none is
 * READY but the current task can keep running, stay. Otherwise fall back
 * to the idle task — which always exists once sched_init has run, so we
 * never lose the thread of execution.
 *
 * Must be called with interrupts disabled.
 */
static void schedule_to_next(void) {
    int from = g_current;
    int next = pick_next_ready(from);
    if (next < 0) {
        if (g_tasks[from].state == TASK_RUNNING) return;
        next = g_idle;
    }
    switch_to(from, next);
}

void task_yield(void) {
    if (!g_initialized) return;
    uint64_t f = irq_save();
    schedule_to_next();
    irq_restore(f);
}

void task_exit(void) {
    irq_disable();
    int id = g_current;
    g_tasks[id].state = TASK_DEAD;
    kprintf("[sched] task %d \"%s\" exited\n", id, g_tasks[id].name);
    schedule_to_next();
    panic("task_exit: schedule_to_next returned");
}

void sched_block_and_switch(void) {
    /* Caller holds IF=0 and has linked us onto a wake source. */
    g_tasks[g_current].state = TASK_BLOCKED;
    schedule_to_next();
}

void sched_make_ready(task_t *t) {
    if (t->state == TASK_BLOCKED) t->state = TASK_READY;
}

/* --- wait queues --- */

void wait_queue_sleep(wait_queue_t *wq) {
    task_t *t = task_current();
    t->wq_next = nullptr;
    if (wq->tail) wq->tail->wq_next = t;
    else          wq->head = t;
    wq->tail = t;
    sched_block_and_switch();
}

void wait_queue_wake_one(wait_queue_t *wq) {
    uint64_t f = irq_save();
    task_t *t = wq->head;
    if (t) {
        wq->head = t->wq_next;
        if (!wq->head) wq->tail = nullptr;
        t->wq_next = nullptr;
        sched_make_ready(t);
    }
    irq_restore(f);
}

void wait_queue_wake_all(wait_queue_t *wq) {
    uint64_t f = irq_save();
    task_t *t = wq->head;
    while (t) {
        task_t *nxt = t->wq_next;
        t->wq_next = nullptr;
        sched_make_ready(t);
        t = nxt;
    }
    wq->head = wq->tail = nullptr;
    irq_restore(f);
}

void sched_tick(void) {
    /* Called from the timer IRQ with IF=0. We can call into the same
       switching machinery cooperative yield uses — the new task's RFLAGS
       will be restored by iretq when irq_common unwinds. */
    if (!g_initialized) return;
    schedule_to_next();
}

/*
 * The first thing a brand-new task runs. Set up by task_create as the
 * "return target" baked into the synthetic stack frame.
 */
extern "C" __attribute__((noreturn))
void task_trampoline(void) {
    /* We arrived here via `ret` from context_switch, which was called
       with IF=0 (either cooperative cli, or inside an IRQ). The task
       expects to run with interrupts on. */
    irq_enable();
    task_t *t = task_current();
    t->entry(t->arg);
    task_exit();
    __builtin_unreachable();
}
