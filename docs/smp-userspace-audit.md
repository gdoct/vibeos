# Audit: un-pinning user tasks from the BSP (ROADMAP §"User tasks on all cores")

**Goal.** Today every task with an address space is `bsp_only=1` and pinned to
CPU 0; the ring-3 syscall / tty / pipe / fd paths are deliberately lock-free on
that assumption. This document is the complete inventory of what must change to
let user tasks (and the threads of one process) run on multiple cores at once.

**Method.** Five parallel read-only sweeps over file/fd, char-device/IPC,
process-lifecycle/memory, scheduler/percpu, and net/time/rng/gui, cross-checked
against the actual code. Two load-bearing facts were verified directly:

- `sched_lock` is a **real multicore spinlock** (`__atomic_exchange_n` acquire /
  `__atomic_store_n` release + per-CPU `push_off`/`pop_off`), held across context
  switches as a baton — [task.c:133-141](../kernel/src/task.c#L133-L141). So
  "move this under `sched_lock`" is a valid fix everywhere, and anything already
  under it is already multicore-safe.
- `tlb_shootdown_all()` is **fully implemented** (IPI broadcast + ack-wait) but
  its **only caller is the boot self-test** — [smp.c:57-74](../kernel/src/smp.c#L57-L74).
  No memory-mutation path ever invokes it.

## Severity framing

Two distinct classes — fix them in this order:

- **GATE (memory-corruption / UAF / panic).** Lost-update on a refcount, a
  double-allocated pool slot, a torn ring cursor, or a stale TLB entry pointing
  at a freed physical page. These crash the kernel or silently corrupt memory.
  *All* of these must be fixed before flipping a single user task to `bsp_only=0`.
- **CORRECT (wrong result, no crash).** A racy file offset, a colliding
  ephemeral port, a dropped signal bit. POSIX-incorrect but memory-safe; fix for
  real multi-threaded workloads but they won't panic the kernel.

`irq_save`/`irq_restore` only masks the **local** CPU's interrupts — it does
**nothing** against a second core. Every "protected by irq_save only" below is
effectively unprotected once userspace is multicore.

---

## GATE-0 — Cross-core TLB shootdown (the big one)

The single most important finding. Threads share a page table (`CLONE_VM`). Every
page-table mutation flushes only the **local** TLB and never the other cores'.

- `map_at` / `unmap_at`: local `invlpg` only — [paging.c:84](../kernel/src/paging.c#L84), [paging.c:98](../kernel/src/paging.c#L98)
- `paging_cow_fault`: local `invlpg` only — [paging.c:296](../kernel/src/paging.c#L296)
- `vmspace_fork`: local CR3 reload only — [paging.c:455](../kernel/src/paging.c#L455)
- Reached from every memory syscall with no shootdown: `sys_brk`, `sys_mmap`,
  `sys_munmap`, `sys_mprotect`, `sys_mremap` — [syscall.c](../kernel/src/syscall.c), and the COW path in [exception.c:43-46](../kernel/src/exception.c#L43-L46).

**Race.** Thread A on CPU0 `munmap`s a page (flushes CPU0). Thread B on CPU1 still
has the stale PTE cached and writes through it to a physical page that has since
been freed and reallocated → silent cross-process memory corruption.

**Fix.** After any unmap / protection-downgrade / COW demotion on a vmspace with
`ref > 1`, call the existing `tlb_shootdown_all()` (must run with interrupts on,
i.e. outside the `sched_lock` critical section). The mechanism already exists and
is self-tested at boot — it just needs to be wired into the mutation paths. A
later optimization is a targeted shootdown (only the CPUs running threads of this
vmspace) instead of broadcast.

---

## GATE-1 — Reference counts (non-atomic read-modify-write → leak / double-free)

All of these are plain `++`/`--` on a shared word. Two cores lose an update →
either a leak (count stuck high) or a double-free / use-after-free (count hits 0
twice). Fix: `__atomic_add_fetch`/`__atomic_sub_fetch`, **or** hold `sched_lock`.

| State | Location | Failure |
|---|---|---|
| `file_t.refcount` | [file.c:36](../kernel/src/file.c#L36), [file.c:41-42](../kernel/src/file.c#L41) | UAF of a `g_files` slot (`panic("file_unref: double free")` already guards the symptom) |
| `g_refcnt[pa]` (COW phys-page) | [paging.c:267-292](../kernel/src/paging.c#L267-L292) | double-free of a physical page |
| `vmspace_t.ref` | [paging.c:400](../kernel/src/paging.c#L400), [paging.c:466](../kernel/src/paging.c#L466) | address space leaked on concurrent thread exit |
| `fdtable_t.ref` | [task.c:162-164](../kernel/src/task.c#L162), [task.c:306](../kernel/src/task.c#L306) | fd table + all its files leaked on concurrent exit |

---

## GATE-2 — Global file pool & fd table

- **`g_files[]` allocation is lock-free** — scan-for-`refcount==0`-then-claim with
  no lock: [file.c:15-32](../kernel/src/file.c#L15). Comment admits it:
  *"lock-free use is fine today: every caller is the BSP-pinned user-syscall path."*
  Two cores claim the same slot → one `file_t` handed to two opens.
  **Fix:** a global `file_lock` spinlock around the scan-and-claim window.
- **Per-process fd table array** `files->fd[]` / `cloexec[]` — `fd_install`'s
  scan-and-insert and `sys_close`'s read-then-null are lock-free across threads
  that share the table (`CLONE_FILES`): [syscall.c:317-329](../kernel/src/syscall.c#L317), [syscall.c:783-791](../kernel/src/syscall.c#L783).
  Two `dup`s grab the same slot; two `close`s double-unref.
  **Fix:** a spinlock *inside* `fdtable_t` (so it's shared with the table itself,
  surviving `CLONE_FILES`), held across allocate-and-install and across close.
- **Socket close** checks `refcount==1` then calls `ksock_close` outside any lock —
  [syscall.c:783-791](../kernel/src/syscall.c#L783): two closers both see `1` →
  double `ksock_close`. Folds into the refcount + fd-table fix.

---

## GATE-3 — Pipes (irq_save-only → needs sched_lock)

The whole pipe discipline relies on local interrupt masking and the BSP pin.
Comment: *"User tasks are BSP-pinned, so the only contention is same-CPU
preemption, which IF-off closes."* — [pipe.c:12-15](../kernel/src/pipe.c#L12).

- ring cursors/count `head`/`tail`/`count` torn by a concurrent reader+writer —
  [pipe.c:59-106](../kernel/src/pipe.c#L59) → buffer overrun / double-read
- `readers`/`writers` lost-decrement in `pipe_detach` — [pipe.c:45-57](../kernel/src/pipe.c#L45)
- `wait_queue_wake_all` on `rwq`/`wwq` called under `irq_save`, not `sched_lock` —
  [pipe.c:50](../kernel/src/pipe.c#L50), [pipe.c:74](../kernel/src/pipe.c#L74), [pipe.c:102](../kernel/src/pipe.c#L102) → wait-queue linked-list corruption

**Fix:** replace `irq_save`/`irq_restore` with `sched_lock`/`sched_unlock` around
the buffer transaction, and use the `*_locked` wait-queue variants
([task.c:613-614](../kernel/src/task.c#L613)) so the condition-check + sleep + wake
share the lock (the pattern `tty.c` and `net.c` already follow).

---

## GATE-4 — TTY line discipline & input ring (single-BSP-consumer assumption)

These encode "the consumer runs only on the BSP" in their comments:

- **TTY cooked-ring read** is mostly under `sched_lock`, but the emptiness check
  sits *before* the lock and `g_line`/`g_line_len` are unguarded on the
  assumption *"tty_poll only runs on the BSP"* — [tty.c:36-49](../kernel/src/tty.c#L36), [tty.c:104-126](../kernel/src/tty.c#L104). A reader on an AP can miss a wake.
  **Fix:** move the emptiness check inside `sched_lock`; move `g_line_len = 0`
  inside the locked region.
- **TTY injection ring** `g_inject` (USB worker → tty_poll) is SPSC and lacks an
  acquire barrier on the consume side — [tty.c:81-102](../kernel/src/tty.c#L81).
- **`/dev/input` ring** `g_ev`/`g_head`/`g_tail` is explicitly *"single-producer
  (usbd) / single-consumer (GUI server on the BSP), lock-free"* —
  [input.c:1-54](../kernel/src/input.c#L1). If the GUI server runs on an AP: head
  lost-update (event read twice) + missing acquire barrier (stale payload).
  **Fix:** spinlock or atomic head/tail with acquire/release barriers.
- **`g_grab`** (set on `/dev/input` open, read by USB worker) is a bare `volatile`
  RMW — [input.c:14](../kernel/src/input.c#L14), [input.c:52-53](../kernel/src/input.c#L52).
  **Fix:** `__atomic` load/store with acquire/release.

`/dev/fb0` mmap is **safe** — geometry is immutable after boot and pixel writes go
to MMIO the hardware arbitrates; no kernel structure is shared
([fb.c](../kernel/src/drivers/fb.c), [syscall.c:532-539](../kernel/src/syscall.c#L532)).

---

## GATE-5 — Scheduler hygiene

- `task_running_cpu()` scans `g_cpu_cur[]` with **no lock** — [task.c:534-538](../kernel/src/task.c#L534). Called from signal delivery; races a concurrent context switch and returns a stale CPU. **Fix:** take `sched_lock` for the scan.

Everything else in the scheduler is already correct for multicore: `sched_lock`
itself, the per-CPU run queues, wait-queue sleep/wake (lock held across the
switch closes the lost-wakeup race — [task.c:613-614](../kernel/src/task.c#L613)),
and futex ([task.c:495-525](../kernel/src/task.c#L495)).

---

## CORRECT — wrong result, no crash (fix for real multi-threading)

| State | Location | Effect | Fix |
|---|---|---|---|
| `file_t.off` (shared via fork/dup) | [syscall.c:333-378](../kernel/src/syscall.c#L333), [syscall.c:793-808](../kernel/src/syscall.c#L793), getdents [syscall.c:1017-1071](../kernel/src/syscall.c#L1017) | duplicate/overlapping reads; O_APPEND interleave | per-file offset lock around read-IO-write |
| `task.brk_cur` | [syscall.c:200-213](../kernel/src/syscall.c#L200) | lost-update → overlapping heap | atomic CAS or `sched_lock` |
| `task.mmap_next` (bump ptr) | [syscall.c:522](../kernel/src/syscall.c#L522), [syscall.c:614](../kernel/src/syscall.c#L614) | two mmaps get the same VA | `__atomic_fetch_add` |
| `task.cwd[256]` | [syscall.c:1083-1101](../kernel/src/syscall.c#L1083) | torn path on concurrent chdir | per-task lock |
| `sig.pending` / `sig.blocked` | [signal.c:146](../kernel/src/signal.c#L146), [signal.c:205](../kernel/src/signal.c#L205) | dropped signal / lost mask bit | atomic OR/AND or `sched_lock` |
| `g_ephemeral` port | [net.c:292](../kernel/src/net.c#L292), [net.c:315](../kernel/src/net.c#L315), [net.c:747](../kernel/src/net.c#L747) | colliding/duplicate ephemeral ports | atomic increment |
| `g_tcp_timer_pending` / `g_tcp_work` | [net.c:509-510](../kernel/src/net.c#L509), read at [net.c:1357](../kernel/src/net.c#L1357) | missed TCP timer wake → stalled retransmit | read inside `sched_lock` / atomic acquire |

---

## Already safe (no change needed)

`sched_lock`, per-CPU run queues, wait-queue sleep/wake, futex; TCP/UDP PCBs, ARP
cache, and all net protocol state (under `sched_lock`, or handed to the net worker
which already runs on any core); ChaCha20 CSPRNG (its own spinlock,
[csprng.c:99-104](../kernel/src/csprng.c#L99)); `g_ticks` timebase (single BSP
writer, [timer.c:19-29](../kernel/src/drivers/timer.c#L19)); `/dev/fb0` mmap.

## Stale comments to correct (claim "user tasks run on any core" — they don't)

[percpu.h:11](../kernel/include/percpu.h#L11), [task.c:39](../kernel/src/task.c#L39),
[task.c:356](../kernel/src/task.c#L356), [usermode.S:21](../kernel/src/usermode.S#L21),
[syscall.c:40](../kernel/src/syscall.c#L40). The per-CPU TSS/GS/swapgs
infrastructure they describe *is* in and correct; only the "so user tasks run on
any core" conclusion is premature while `bsp_only` is enforced.

## The pin itself (flip last, after all GATEs land)

`bsp_only`: declared [task.h:61-64](../kernel/include/task.h#L61); written at
[task.c:220](../kernel/src/task.c#L220), [task.c:274](../kernel/src/task.c#L274),
[task.c:330](../kernel/src/task.c#L330), [main.c:67](../kernel/src/main.c#L67);
enforced at [task.c:88-92](../kernel/src/task.c#L88) (`enqueue_ready`) and
[task.c:94-109](../kernel/src/task.c#L94) (`runq_steal`). Removing the two
enforcement reads lets user tasks home to their `home_cpu` and be stolen like any
kernel task — the per-CPU TSS/GS already route ring-3 traps correctly. Flip this
**only after** GATE-0…5 are done; a partial flip is a memory-corruption bug, not a
clean panic.

## Suggested implementation order

1. **GATE-0** TLB shootdown — wire `tlb_shootdown_all()` into the mmap/munmap/
   mprotect/COW/fork paths (gated on `vmspace.ref > 1`).
2. **GATE-1** refcounts → atomics (`file`, `g_refcnt`, `vmspace`, `fdtable`).
3. **GATE-2** `file_lock` + per-`fdtable` lock.
4. **GATE-3** pipes onto `sched_lock` + `*_locked` wait queues.
5. **GATE-4** tty/input barriers + locking; **GATE-5** `task_running_cpu`.
6. CORRECT-class fixes (offset/brk/mmap_next/cwd/signal/net).
7. Fix the stale comments; **then** drop the `bsp_only` enforcement and test under
   `-smp 4` with a multi-threaded musl binary hammering fork/mmap/pipe.
