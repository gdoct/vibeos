#include "kernel.h"
#include "file.h"
#include "pipe.h"

/*
 * Open-file object pool (ROADMAP §4 rung 2). See file.h.
 *
 * A fixed pool, like the task and kstack pools — cheap, with an obvious failure
 * mode (allocation returns NULL -> the caller reports -ENFILE). Single global
 * lock-free use is fine today: every caller is the BSP-pinned user-syscall path.
 */

#define MAX_OPEN_FILES 64

static file_t g_files[MAX_OPEN_FILES];

file_t *file_alloc(void) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (g_files[i].refcount == 0) {
            file_t *f = &g_files[i];
            f->refcount = 1;
            f->kind = FD_NONE;
            f->ino = 0;
            f->off = 0;
            f->flags = 0;
            f->dev = 0;
            f->pipe = nullptr;
            f->sock = nullptr;
            return f;
        }
    }
    return nullptr;
}

void file_ref(file_t *f) {
    if (f) f->refcount++;
}

void file_unref(file_t *f) {
    if (!f) return;
    if (f->refcount <= 0) panic("file_unref: double free");
    if (--f->refcount == 0) {
        if (f->kind == FD_PIPE_RD || f->kind == FD_PIPE_WR)
            pipe_detach(f);             /* drop this end; frees pipe at zero */
        f->kind = FD_NONE;
        f->pipe = nullptr;
    }
}
