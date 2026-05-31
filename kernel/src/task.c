#include "kernel.h"
#include "pmm.h"
#include "irq.h"
#include "paging.h"
#include "task.h"
#include "file.h"
#include "usermode.h"
#include "percpu.h"
#include "timer.h"
#include "smp.h"
#include "kmalloc.h"

/*
 * SMP scheduler (ROADMAP §1, stage B), xv6-style.
 *
 * Each CPU runs its own scheduler() loop on its boot/AP stack. Tasks never
 * switch directly to each other; they switch to/from the per-CPU scheduler
 * context. A single sched_lock protects all task state (the `state` field,
 * the wait queues, the sleeper list, the per-CPU run queues, and each CPU's
 * `current`) and is held *across* every context switch — handed off like a
 * baton: the side that resumes after a switch is responsible for releasing it
 * (task_trampoline / fork_child_return on first run; the scheduler / yielding
 * task otherwise).
 *
 * Run queues (ROADMAP §3): each CPU owns a FIFO of READY tasks. A task is
 * enqueued on its home CPU (the one it last ran on, for cache affinity); a
 * scheduler pops its own queue first and only steals from another CPU's queue
 * when its own is empty. Because the baton lock spans the context switch, a
 * task on a run queue can never be popped by another CPU before the switch that
 * saves its rsp has completed — so the lock, not a per-task on_cpu flag, is what
 * makes cross-CPU stealing safe. (The lock therefore stays global by design;
 * the win here is O(1) pick + affinity + *explicit* stealing over the old
 * O(MAX_TASKS) ready-scan, and a much shorter critical section per pick.)
 *
 * Interrupt state is tracked per-CPU (push_off/pop_off, à la xv6 push/popcli)
 * rather than stored in the lock, because the lock crosses context switches —
 * so the IF baton must follow the CPU, not the lock word.
 *
 * Scope: kernel and user tasks both run on any CPU (per-CPU TSS/GS + swapgs,
 * §2), so any ready task is stealable by any core.
 */

#define KSTACK_PAGES   4   /* 16 KiB kernel stack per task */

#define MSR_FS_BASE    0xC0000100u

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)));
}

extern "C" void context_switch(uint64_t *old_rsp, uint64_t new_rsp);
extern "C" __attribute__((noreturn)) void task_trampoline(void);
extern "C" void fork_child_return(void);

static task_t   g_tasks[MAX_TASKS];   /* slot 0 unused (no boot task in this model) */
static int      g_started = 0;

/* Per-CPU scheduler state. */
static uint64_t g_sched_rsp[SMP_MAX_CPUS];   /* the scheduler loop's saved rsp */
static task_t  *g_cpu_cur[SMP_MAX_CPUS];     /* running task (NULL while in scheduler) */

/* Per-CPU ready queues (ROADMAP §3). A FIFO of READY tasks per CPU, threaded
   through task->rq_next. All touched only under sched_lock — the same baton that
   crosses context switches, so a task pushed here can't be popped by another CPU
   until the switch that saves its rsp completes. The win over the old global
   array scan: O(1) pick, CPU affinity, and explicit (not incidental) stealing. */
static task_t  *g_runq_head[SMP_MAX_CPUS];
static task_t  *g_runq_tail[SMP_MAX_CPUS];

static void runq_push(int cpu, task_t *t) {
    t->rq_next = nullptr;
    if (g_runq_tail[cpu]) g_runq_tail[cpu]->rq_next = t;
    else                  g_runq_head[cpu] = t;
    g_runq_tail[cpu] = t;
}
static task_t *runq_pop(int cpu) {
    task_t *t = g_runq_head[cpu];
    if (t) {
        g_runq_head[cpu] = t->rq_next;
        if (!g_runq_head[cpu]) g_runq_tail[cpu] = nullptr;
        t->rq_next = nullptr;
    }
    return t;
}

/* Mark a task READY and enqueue it on a run queue (lock held). User tasks
   (bsp_only) always go on CPU 0's queue; kernel tasks go on their home CPU. */
static void enqueue_ready(task_t *t) {
    t->state = TASK_READY;
    runq_push(t->bsp_only ? 0 : t->home_cpu, t);
}

/* Steal the first *kernel* task from `cpu`'s run queue (lock held). User tasks
   are BSP-pinned, so they are never stolen — we walk past them. */
static task_t *runq_steal(int cpu) {
    task_t *prev = nullptr, *t = g_runq_head[cpu];
    while (t) {
        if (!t->bsp_only) {
            if (prev) prev->rq_next = t->rq_next;
            else      g_runq_head[cpu] = t->rq_next;
            if (g_runq_tail[cpu] == t) g_runq_tail[cpu] = prev;
            t->rq_next = nullptr;
            return t;
        }
        prev = t; t = t->rq_next;
    }
    return nullptr;
}

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
/* Exported (§2): drivers whose completion IRQ may fire on a *different* CPU than
   the sleeper must check their condition and sleep under this lock, and wake
   under it too — local irq_save no longer excludes a remote waker. */
extern "C" void sched_lock(void) {
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

/* ---- descriptor tables (refcounted so threads can share one) ---- */

static fdtable_t *fdtable_alloc(void) {
    fdtable_t *ft = (fdtable_t *)kmalloc(sizeof *ft);
    if (!ft) panic("fdtable: out of memory");
    kmemset(ft, 0, sizeof *ft);
    ft->ref = 1;
    return ft;
}
static void fdtable_unref(fdtable_t *ft) {
    if (!ft) return;
    if (--ft->ref > 0) return;              /* still shared by another thread */
    for (int i = 0; i < VFS_MAX_FD; i++)
        if (ft->fd[i]) file_unref(ft->fd[i]);
    kfree(ft);
}

/* ---- task creation ---- */

static task_t *alloc_task_slot(const char *name) {
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++)
        if (g_tasks[i].state == TASK_NONE || g_tasks[i].state == TASK_DEAD) { slot = i; break; }
    if (slot < 0) panic("task: pool full");

    task_t *t = &g_tasks[slot];

    /* Kernel-stack reclamation (ROADMAP §1.1): a reaped slot keeps its kernel
       stack and the next task reuses it, so kstack_alloc's bump window only ever
       advances MAX_TASKS times total — no per-fork leak. The synthetic stack is
       rebuilt from the top by the caller, so stale contents below don't matter.
       A never-used slot gets a fresh stack the first time. */
    uint64_t base = t->stack_base, top = t->stack_top;
    if (!top) top = kstack_alloc(KSTACK_PAGES, &base);

    kmemset(t, 0, sizeof(*t));
    t->id = slot; t->name = name;
    t->stack_base = base; t->stack_top = top;
    t->cwd[0] = '/'; t->cwd[1] = '\0';     /* default cwd (§4) */
    t->files = fdtable_alloc();            /* fresh, empty; fork copies, clone shares */
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

    t->home_cpu = smp_cpu_index();    /* created here; stealing rebalances later */
    enqueue_ready(t);
    int id = t->id;
    sched_unlock();
    kprintf("[sched] task %d \"%s\" entry=%p\n", id, name, (void *)(uintptr_t)entry);
    return t;
}

void task_set_vmspace(struct vmspace *vm) {
    task_t *t = task_current();
    t->vm = vm;
    if (vm) t->bsp_only = 1;          /* became a user task: pin to the BSP (§3) */
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
    t->fs_base   = parent->fs_base;     /* child shares the TLS layout (AS copied) */
    t->mmap_next = parent->mmap_next;
    signals_fork(t, parent);            /* inherit handlers + mask (not pending) */

    /* Dup the fd table: the child shares each open-file object (and thus its
       offset) with the parent, à la Linux fork. The per-fd FD_CLOEXEC bit is a
       descriptor property, so it is copied too. */
    for (int i = 0; i < VFS_MAX_FD; i++) {
        t->files->fd[i] = parent->files->fd[i];
        t->files->cloexec[i] = parent->files->cloexec[i];
        if (t->files->fd[i]) file_ref(t->files->fd[i]);
    }
    for (unsigned i = 0; i < sizeof t->cwd; i++) t->cwd[i] = parent->cwd[i];  /* inherit cwd */

    /* Craft the child stack: context_switch -> fork_child_return, which finds a
       copy of the parent's syscall frame (rax forced to 0) and sysrets. The
       callee-saved regs are cloned too, so the child resumes with the parent's
       rbx/rbp/r12-r15 intact (else it would return to ring 3 with them zeroed). */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = frame->r15;
    *--sp = frame->r14;
    *--sp = frame->r13;
    *--sp = frame->r12;
    *--sp = frame->rbp;
    *--sp = frame->rbx;
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

    t->bsp_only = (vm != nullptr);    /* user process: BSP-pinned (§3) */
    t->home_cpu = 0;
    t->tgid = 0;                      /* a new process: tgid == its own id */
    enqueue_ready(t);
    int id = t->id, pid = parent->id;
    sched_unlock();
    kprintf("[sched] fork: %d \"%s\" -> child %d\n", pid, parent->name, id);
    return t;
}

int task_tgid(task_t *t) { return t->tgid ? t->tgid : t->id; }

task_t *task_clone(const char *name, struct vmspace *vm,
                   const struct syscall_frame *frame, uint64_t user_stack,
                   uint64_t tls, int share_files, int is_thread) {
    task_t *parent = task_current();
    sched_lock();
    task_t *t = alloc_task_slot(name);
    t->vm        = vm;                          /* caller ref'd it for CLONE_VM */
    t->parent    = parent;
    t->brk_start = parent->brk_start;
    t->brk_cur   = parent->brk_cur;
    t->brk_max   = parent->brk_max;
    t->fs_base   = tls ? tls : parent->fs_base; /* CLONE_SETTLS */
    t->mmap_next = parent->mmap_next;
    t->is_thread = is_thread;
    t->tgid      = is_thread ? task_tgid(parent) : 0;
    signals_fork(t, parent);

    if (share_files) {                          /* CLONE_FILES: share the fdtable */
        fdtable_unref(t->files);                /* drop the fresh empty one */
        t->files = parent->files;
        t->files->ref++;
    } else {
        for (int i = 0; i < VFS_MAX_FD; i++) {
            t->files->fd[i] = parent->files->fd[i];
            t->files->cloexec[i] = parent->files->cloexec[i];
            if (t->files->fd[i]) file_ref(t->files->fd[i]);
        }
    }
    for (unsigned i = 0; i < sizeof t->cwd; i++) t->cwd[i] = parent->cwd[i];

    /* Same fork_child_return frame as fork, but the child resumes on its own
       stack (user_stack) with rax=0. */
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = frame->r15; *--sp = frame->r14; *--sp = frame->r13; *--sp = frame->r12;
    *--sp = frame->rbp; *--sp = frame->rbx;
    *--sp = user_stack;                         /* <-- child's user rsp */
    *--sp = frame->r11; *--sp = frame->rcx;
    *--sp = frame->r9;  *--sp = frame->r8;  *--sp = frame->r10;
    *--sp = frame->rdx; *--sp = frame->rsi; *--sp = frame->rdi;
    *--sp = 0;                                  /* child sees clone() == 0 */
    *--sp = (uint64_t)fork_child_return;
    for (int j = 0; j < 6; j++) *--sp = 0;
    t->saved_rsp = (uint64_t)sp;

    t->bsp_only = 1;                            /* user task: BSP-pinned (§3) */
    t->home_cpu = 0;
    enqueue_ready(t);
    int id = t->id;
    sched_unlock();
    kprintf("[sched] clone: %d \"%s\" -> %s %d\n",
            parent->id, parent->name, is_thread ? "thread" : "child", id);
    return t;
}

/* ---- the scheduler ---- */

static void apply_task_mm(task_t *t) {
    uint64_t cr3 = t->vm ? t->vm->pml4_phys : paging_kernel_pml4();
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
    /* Always latch this CPU's kernel stack (syscall entry %gs:kernel_rsp + the
       TSS rsp0 a ring-3 trap lands on). Done for every task, not just ones with
       a vm: a task can become a user task after it is first scheduled (init
       creates its address space inside init_launch), and on first syscall there
       may have been no reschedule to set it. Harmless for kernel threads. */
    percpu_set_kernel_stack(t->stack_top);
    if (t->vm) wrmsr(MSR_FS_BASE, t->fs_base);   /* restore this task's TLS base */
}

/* Pick a READY task for CPU `cpu`: prefer this CPU's own run queue (warm cache),
   and only when it is empty steal one ready task from another CPU's queue
   (explicit work-stealing, ROADMAP §3). §2 user tasks run on any core, so any
   task is stealable. Lock held. */
static task_t *pick_ready(int cpu) {
    task_t *t = runq_pop(cpu);
    if (t) return t;
    int n = smp_cpu_count();
    for (int i = 1; i < n; i++) {                 /* scan the other CPUs in order */
        int victim = (cpu + i) % n;
        t = runq_steal(victim);                   /* steals kernel tasks only */
        if (t) return t;
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
            t->home_cpu = c;          /* re-home to where it last ran (affinity) */
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
    if (t) enqueue_ready(t);          /* back on this CPU's queue, then yield */
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
    if (t->state == TASK_BLOCKED) enqueue_ready(t);
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
static void wq_wake_one_locked(wait_queue_t *wq);   /* defined below; used by futex */

static void task_exit_common(int code, int sig) {
    task_t *t = task_current();
    if (sig) kprintf("[sched] task %d \"%s\" killed by signal %d\n", t->id, t->name, sig);
    else     kprintf("[sched] task %d \"%s\" exit(%d)\n", t->id, t->name, code);
    /* Release this task's reference to its descriptor table before becoming a
       zombie (the address space is reaped later by the parent's wait4). With a
       shared table (threads), the files stay open until the last reference. */
    fdtable_unref(t->files);
    t->files = nullptr;
    sched_lock();
    t->exit_code   = code;
    t->term_signal = sig;
    task_t *p = t->parent;
    if (p && p->state != TASK_NONE && p->state != TASK_DEAD) {
        t->state = TASK_ZOMBIE;       /* parent's wait4 reaps */
        wq_wake_all_locked(&p->child_wq);
    } else {
        t->state = TASK_DEAD;
    }
    sched();
    panic("task_exit: returned");
}

void task_exit_user(int code)  { task_exit_common(code, 0); __builtin_unreachable(); }
void task_exit_signal(int sig) { task_exit_common(0, sig);  __builtin_unreachable(); }

/* A CLONE_THREAD task exiting: it has no waiter (pthread_join uses the futex on
   clear_child_tid, handled by the caller), so it does not become a zombie — it
   releases its descriptor table + address-space reference and dies. Because it
   may be dropping the last AS reference while running on it, switch CR3 to the
   kernel master tables first so vmspace_destroy can free the page tables. */
void task_exit_thread(void) {
    task_t *t = task_current();
    kprintf("[sched] thread %d (tgid %d) exited\n", t->id, task_tgid(t));
    fdtable_unref(t->files);
    t->files = nullptr;
    struct vmspace *vm = t->vm;
    vmspace_switch(nullptr);          /* CR3 -> kernel master; AS no longer active */
    t->vm = nullptr;
    vmspace_destroy(vm);              /* drop this thread's ref; frees at 0 */
    sched_lock();
    t->state = TASK_DEAD;
    sched();
    panic("task_exit_thread: returned");
}

/* ---- futex (ROADMAP): wait/wake keyed on a word's physical address ---- */

#define FUTEX_BUCKETS 32
static struct { uint64_t key; wait_queue_t wq; } g_futex[FUTEX_BUCKETS];

/* Find the bucket for `key` (an empty wq means the slot is reusable). Lock held. */
static wait_queue_t *futex_bucket(uint64_t key) {
    int free_slot = -1;
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        if (g_futex[i].wq.head && g_futex[i].key == key) return &g_futex[i].wq;
        if (free_slot < 0 && !g_futex[i].wq.head) free_slot = i;
    }
    if (free_slot < 0) return nullptr;
    g_futex[free_slot].key = key;
    return &g_futex[free_slot].wq;
}

/* Sleep the current task on `key` (caller holds sched_lock). */
void futex_sleep_on(uint64_t key) {
    wait_queue_t *wq = futex_bucket(key);
    if (!wq) return;                  /* table full: treat as a spurious wake */
    wq_sleep_locked(wq);
}

/* Wake up to `n` waiters on `key` (caller holds sched_lock). Returns count hint. */
int futex_wake_key(uint64_t key, int n) {
    for (int i = 0; i < FUTEX_BUCKETS; i++)
        if (g_futex[i].wq.head && g_futex[i].key == key) {
            int woke = 0;
            while (g_futex[i].wq.head && woke < n) { wq_wake_one_locked(&g_futex[i].wq); woke++; }
            return woke;
        }
    return 0;
}

task_t *task_by_id(int id) {
    if (id < 1 || id >= MAX_TASKS) return nullptr;
    task_t *t = &g_tasks[id];
    if (t->state == TASK_NONE || t->state == TASK_DEAD) return nullptr;
    return t;
}

int task_running_cpu(task_t *t) {
    for (int c = 0; c < SMP_MAX_CPUS; c++)
        if (g_cpu_cur[c] == t) return c;
    return -1;
}

/* Nudge a task that may be asleep so it returns to userspace and delivers a
   freshly-raised signal. Waking a BLOCKED task makes it READY; the blocking
   primitive re-checks and (for interruptible waits like tty_read) bails with
   -EINTR, after which signal delivery runs on the syscall return. */
void task_signal_wake(task_t *t) {
    sched_lock();
    if (t->stopped) { sched_unlock(); return; }   /* stopped: leave for SIGCONT */
    make_ready_locked(t);
    sched_unlock();
}

/* Job-control stop: park the current task until task_cont. */
void task_stop_current(void) {
    sched_lock();
    task_t *t = g_cpu_cur[smp_cpu_index()];
    t->stopped = 1;
    t->state = TASK_BLOCKED;
    sched();                          /* resumes here once continued */
    sched_unlock();
}

void task_cont(task_t *t) {
    sched_lock();
    if (t->stopped) {
        t->stopped = 0;
        if (t->state == TASK_BLOCKED) enqueue_ready(t);
    }
    sched_unlock();
}

int task_wait(int *status, int nohang, int wpid) {
    sched_lock();
    task_t *p = g_cpu_cur[smp_cpu_index()];
    for (;;) {
        int have_child = 0;
        for (int i = 1; i < MAX_TASKS; i++) {
            task_t *c = &g_tasks[i];
            if (c->parent != p) continue;
            if (wpid > 0 && c->id != wpid) continue;   /* waitpid(pid): only that child */
            if (c->state == TASK_ZOMBIE) {
                int pid = c->id;
                /* Encode a Linux wait status: WIFSIGNALED (low 7 bits = signal)
                   if killed by a signal, else WIFEXITED ((code & 0xff) << 8). */
                int wstatus = c->term_signal ? (c->term_signal & 0x7f)
                                             : ((c->exit_code & 0xff) << 8);
                struct vmspace *vm = c->vm;
                c->state = TASK_DEAD; c->parent = nullptr; c->vm = nullptr;
                sched_unlock();
                if (vm) vmspace_destroy(vm);   /* zombie won't run again */
                if (status) *status = wstatus;
                return pid;
            }
            if (c->state != TASK_DEAD && c->state != TASK_NONE) have_child = 1;
        }
        if (!have_child) { sched_unlock(); return -10; }   /* -ECHILD */
        if (nohang) { sched_unlock(); return 0; }          /* WNOHANG: nothing ready */
        wq_sleep_locked(&p->child_wq);
    }
}

/* ---- wait queues (public; acquire the lock) ---- */

static void wq_wake_one_locked(wait_queue_t *wq) {
    task_t *t = wq->head;
    if (t) { wq->head = t->wq_next; if (!wq->head) wq->tail = nullptr; t->wq_next = nullptr; make_ready_locked(t); }
}

void wait_queue_sleep(wait_queue_t *wq)    { sched_lock(); wq_sleep_locked(wq); sched_unlock(); }
void wait_queue_wake_one(wait_queue_t *wq) { sched_lock(); wq_wake_one_locked(wq); sched_unlock(); }
void wait_queue_wake_all(wait_queue_t *wq) { sched_lock(); wq_wake_all_locked(wq); sched_unlock(); }

/* Locked variants — caller already holds sched_lock. The condition check, the
   sleep, and the wake share the lock, closing the cross-CPU lost-wakeup race. */
extern "C" void wait_queue_sleep_locked(wait_queue_t *wq)    { wq_sleep_locked(wq); }
extern "C" void wait_queue_wake_one_locked(wait_queue_t *wq) { wq_wake_one_locked(wq); }
extern "C" void wait_queue_wake_all_locked(wait_queue_t *wq) { wq_wake_all_locked(wq); }

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
            *pp = t->wq_next; t->wq_next = nullptr; enqueue_ready(t);
        } else {
            pp = &t->wq_next;
        }
    }
    task_t *cur = g_cpu_cur[smp_cpu_index()];
    if (cur && cur->state == TASK_RUNNING) {
        enqueue_ready(cur);           /* re-queue on this CPU, then preempt */
        sched();                      /* resumes here when rescheduled */
    }
    sched_unlock();
}
