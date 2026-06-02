---
name: smp-userspace-multicore
description: User tasks now run on all cores (BSP pin removed); the ring-3 paths were made SMP-safe
metadata:
  type: project
---

As of 2026-06-02 the `bsp_only` pin is gone — user tasks are work-stolen onto every core. The previously lock-free ring-3 paths were hardened (full map: [docs/smp-userspace-audit.md](../../../docs/smp-userspace-audit.md)):

- **Atomic refcounts**: `file_t.refcount` (CAS-claim in `file_alloc`), `fdtable.ref`, `vmspace.ref`, and arena page `g_refcnt` (CAS dec-if-positive). `file_unref` now also closes the socket/pipe at the last ref.
- **Per-`fdtable_t` spinlock** for fd install/close/dup/dup2/fork-copy/exec-cloexec; `fd_get_ref`/`fd_take` helpers.
- **Per-`vmspace_t` spinlock** guarding page-table mutation (`vmspace_map`/`unmap`) and COW-fault repair; `mmap_next` moved from `task_t` into the shared `vmspace_t` (atomic bump).
- **Cross-core TLB shootdown** (`tlb_shootdown_user` in smp.c, serialized, enables IF for the ack-wait) called from `munmap`/`mprotect`/`fork` only when `vm->ref > 1`.
- **Pipes** moved from `irq_save` onto `sched_lock` + `*_locked` wait queues; **`/dev/input`** got a consumer lock + acquire/release barriers + atomic grab flag; **net** PCB pools got `g_net_lock`, ephemeral port is atomic.

Verified under `-smp 4`: `cputest` (4 CPU-bound forks) spreads 5 samples each across cpu 0/1/2/3; `threadtest` (4 threads, shared AS+fdtable) reaches counter=80000 exactly; pipe/sig/fault/abi tests clean, no panics.

`tty_poll` stays BSP-only (timer.c gates it to CPU 0) so the line-discipline `g_line` needs no lock. Still per-task (not per-process) and thus a latent thread-semantics gap, not a safety bug: `brk` and `cwd`. Build/test with `make all && make image` — see [[build-run-image-gotcha]].
