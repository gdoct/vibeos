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
static uint64_t g_active_cr3 = 0;   /* last CR3 we loaded (avoid redundant flushes) */

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

/* Claim a free task slot and give it a fresh guarded kernel stack. The slot is
   fully zeroed first, so a reused DEAD slot carries no stale fields (vm, brk,
   wq links). Caller fills entry/saved_rsp and sets state READY. */
static task_t *alloc_task_slot(const char *name) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_NONE || g_tasks[i].state == TASK_DEAD) {
            slot = i; break;
        }
    }
    if (slot < 0) panic("task: pool full");

    /* Stack lives in the kernel vmalloc window with an unmapped guard page
       below it, so an overflow faults cleanly. stack_base/top are virtual. */
    uint64_t base;
    uint64_t top = kstack_alloc(KSTACK_PAGES, &base);

    task_t *t = &g_tasks[slot];
    kmemset(t, 0, sizeof(*t));
    t->id         = slot;
    t->name       = name;
    t->stack_base = base;
    t->stack_top  = top;
    return t;
}

task_t *task_create(const char *name, void (*entry)(void *), void *arg) {
    task_t *t = alloc_task_slot(name);
    t->entry = entry;
    t->arg   = arg;

    /* Hand-craft the stack so context_switch's restore-and-ret lands at
       task_trampoline with zeroed callee-saved registers (see the original
       note): from low addr (saved_rsp) up: r15..rbx = 0, then the return
       address = task_trampoline. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = (uint64_t)task_trampoline;
    for (int j = 0; j < 6; j++) *--sp = 0;
    t->saved_rsp = (uint64_t)sp;

    t->state = TASK_READY;
    kprintf("[sched] task %d \"%s\" entry=%p stack=%lx..%lx\n",
            t->id, name, (void *)(uintptr_t)entry,
            (unsigned long)t->stack_base, (unsigned long)t->stack_top);
    return t;
}

/* Attach `vm` to the current task and make it the active address space now
   (keeping the CR3 tracker in sync). Used to move a task into a user address
   space before entering ring 3. */
void task_set_vmspace(struct vmspace *vm) {
    task_t *t = task_current();
    t->vm = vm;
    uint64_t cr3 = vm ? vm->pml4_phys : paging_kernel_pml4();
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    g_active_cr3 = cr3;
}

extern "C" void fork_child_return(void);

task_t *task_fork(const char *name, struct vmspace *vm,
                  const struct syscall_frame *frame) {
    task_t *parent = task_current();
    task_t *t = alloc_task_slot(name);
    t->vm        = vm;
    t->parent    = parent;
    t->brk_start = parent->brk_start;
    t->brk_cur   = parent->brk_cur;
    t->brk_max   = parent->brk_max;

    /* Craft the child kernel stack so context_switch restores 6 zeroed callee-
       saved regs and `ret`s into fork_child_return, which then finds a copy of
       the parent's syscall frame (with rax forced to 0) and sysrets to ring 3.
       Push order (high->low) mirrors syscall_frame_t, then the return address,
       then the callee-saved block (r15 ends up lowest = where saved_rsp points). */
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
    *--sp = (uint64_t)fork_child_return;      /* context_switch ret target */
    for (int j = 0; j < 6; j++) *--sp = 0;    /* callee-saved */
    t->saved_rsp = (uint64_t)sp;

    t->state = TASK_READY;
    kprintf("[sched] fork: %d \"%s\" -> child %d\n", parent->id, parent->name, t->id);
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
    /* Switch address space: a user process runs in its own PML4, kernel
       threads in the master tables. Skip the (TLB-flushing) CR3 reload when
       it would not change. The kernel half is identical across all PML4s, so
       the running kernel code/stack stay mapped across the switch. */
    uint64_t want_cr3 = next->vm ? next->vm->pml4_phys : paging_kernel_pml4();
    if (want_cr3 != g_active_cr3) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(want_cr3) : "memory");
        g_active_cr3 = want_cr3;
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

void task_exit_user(int code) {
    irq_disable();
    task_t *t = task_current();
    t->exit_code = code;
    task_t *p = t->parent;
    if (p && p->state != TASK_NONE && p->state != TASK_DEAD) {
        /* Become a zombie; the parent's wait4 collects the status and reaps. */
        t->state = TASK_ZOMBIE;
        wait_queue_wake_all(&p->child_wq);
    } else {
        /* No reaper: drop the slot. The address space is the active CR3 right
           now, so it can't be torn down here — leak it (only init/orphans). */
        t->state = TASK_DEAD;
    }
    kprintf("[sched] task %d \"%s\" exit(%d)\n", t->id, t->name, code);
    schedule_to_next();
    panic("task_exit_user: returned");
}

int task_wait(int *status) {
    task_t *p = task_current();
    for (;;) {
        uint64_t fl = irq_save();
        int have_child = 0;
        for (int i = 1; i < MAX_TASKS; i++) {
            task_t *c = &g_tasks[i];
            if (c->parent != p) continue;
            if (c->state == TASK_ZOMBIE) {
                int pid = c->id, code = c->exit_code;
                struct vmspace *vm = c->vm;
                c->state  = TASK_DEAD;      /* detach + free the slot */
                c->parent = nullptr;
                c->vm     = nullptr;
                irq_restore(fl);
                if (vm) vmspace_destroy(vm);   /* zombie never runs again */
                if (status) *status = code;
                return pid;
            }
            if (c->state != TASK_DEAD && c->state != TASK_NONE) have_child = 1;
        }
        if (!have_child) { irq_restore(fl); return -10; }   /* -ECHILD */
        wait_queue_sleep(&p->child_wq);     /* IF off; returns IF off when woken */
        irq_restore(fl);
    }
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
