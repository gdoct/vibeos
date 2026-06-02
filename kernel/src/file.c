#include "kernel.h"
#include "file.h"
#include "pipe.h"
#include "net.h"

/*
 * Open-file object pool (ROADMAP §4 rung 2). See file.h.
 *
 * A fixed pool, like the task and kstack pools — cheap, with an obvious failure
 * mode (allocation returns NULL -> the caller reports -ENFILE). The refcount is
 * the slot's free/busy flag *and* its sharing count, so with user tasks running
 * on every core (ROADMAP §"User tasks on all cores") it is manipulated only with
 * atomics: allocation claims a slot with a 0->1 compare-exchange, and ref/unref
 * are atomic add/sub. This makes the pool lock-free yet SMP-safe.
 */

#define MAX_OPEN_FILES 64

static file_t g_files[MAX_OPEN_FILES];

file_t *file_alloc(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        int expected = 0;
        /* Claim the slot atomically: only the CPU that flips 0->1 owns it, so two
           cores allocating at once can never hand out the same file_t. */
        if (__atomic_compare_exchange_n(&g_files[i].refcount, &expected, 1,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            file_t *f = &g_files[i];
            f->kind = FD_NONE;
            f->ino = 0;
            f->off = 0;
            f->flags = 0;
            f->dev = 0;
            f->pipe = nullptr;
            f->sock = nullptr;
            f->aux1 = 0;
            f->aux2 = 0;
            spin_lock_init(&f->off_lock);
            return f;
        }
    }
    return nullptr;
}

void file_ref(file_t *f) {
    if (f) __atomic_add_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL);
}

void file_unref(file_t *f) {
    if (!f) return;
    int nv = __atomic_sub_fetch(&f->refcount, 1, __ATOMIC_ACQ_REL);
    if (nv < 0) panic("file_unref: double free");
    if (nv == 0) {
        /* Last reference: tear down the backing object exactly once. Doing the
           socket close here (rather than at the call sites via a racy
           `refcount == 1` test) makes it atomic with the final decrement and also
           closes sockets still open at process/thread exit. */
        if (f->kind == FD_PIPE_RD || f->kind == FD_PIPE_WR)
            pipe_detach(f);             /* drop this end; frees pipe at zero */
        else if (f->kind == FD_SOCKET && f->sock)
            ksock_close(f->sock);
        f->kind = FD_NONE;
        f->pipe = nullptr;
        f->sock = nullptr;
        /* refcount is already 0 here, which is exactly the free marker file_alloc
           scans for; the field stores 0, so the slot is reusable from now on. */
    }
}
