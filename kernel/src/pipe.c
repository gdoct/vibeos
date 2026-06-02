#include "kernel.h"
#include "task.h"
#include "kmalloc.h"
#include "file.h"
#include "pipe.h"

/*
 * Anonymous pipes (ROADMAP §4 rung 2). See pipe.h for the model.
 *
 * A byte ring with reader/writer wait queues, mirroring the serial TTY's
 * blocking discipline. Now that user tasks run on every core (ROADMAP §"User
 * tasks on all cores"), a reader and writer can touch one pipe from two CPUs at
 * once, so local interrupt masking is no longer enough: the whole ring + the
 * counts + the wait queues are guarded by sched_lock, and the condition check,
 * the sleep, and the wake all share it (the *_locked wait-queue ops), which
 * closes the cross-core lost-wakeup race the same way the TTY and net stack do.
 */

#define PIPE_CAP   4096          /* ring capacity (one page) */

#define EAGAIN_    11
#define EPIPE_     32

struct pipe {
    uint8_t      buf[PIPE_CAP];
    uint32_t     head;           /* write cursor */
    uint32_t     tail;           /* read cursor */
    uint32_t     count;          /* bytes currently buffered */
    int          readers;        /* open read ends */
    int          writers;        /* open write ends */
    wait_queue_t rwq;            /* readers waiting for data */
    wait_queue_t wwq;            /* writers waiting for space */
};

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof *p);
    if (!p) return nullptr;
    kmemset(p, 0, sizeof *p);
    p->readers = 1;
    p->writers = 1;
    return p;
}

void pipe_free(pipe_t *p) { if (p) kfree(p); }

void pipe_detach(file_t *f) {
    pipe_t *p = f->pipe;
    if (!p) return;
    sched_lock();
    if (f->kind == FD_PIPE_RD) {
        if (--p->readers == 0) wait_queue_wake_all_locked(&p->wwq);  /* writers -> EPIPE */
    } else {
        if (--p->writers == 0) wait_queue_wake_all_locked(&p->rwq);  /* readers -> EOF */
    }
    int gone = (p->readers == 0 && p->writers == 0);
    sched_unlock();
    if (gone) pipe_free(p);
}

int pipe_read(pipe_t *p, void *buf, uint32_t n, int flags) {
    if (n == 0) return 0;
    uint8_t *out = (uint8_t *)buf;
    sched_lock();
    while (p->count == 0) {
        if (p->writers == 0) { sched_unlock(); return 0; }   /* EOF */
        if (flags & 04000 /* O_NONBLOCK */) { sched_unlock(); return -EAGAIN_; }
        wait_queue_sleep_locked(&p->rwq);                    /* re-checks under lock */
    }
    uint32_t i = 0;
    while (i < n && p->count > 0) {
        out[i++] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_CAP;
        p->count--;
    }
    wait_queue_wake_all_locked(&p->wwq);        /* freed space for writers */
    sched_unlock();
    return (int)i;
}

int pipe_write(pipe_t *p, const void *buf, uint32_t n, int flags) {
    if (n == 0) return 0;
    const uint8_t *in = (const uint8_t *)buf;
    sched_lock();
    uint32_t done = 0;
    while (done < n) {
        if (p->readers == 0) {           /* no SIGPIPE: just report the error */
            sched_unlock();
            return done ? (int)done : -EPIPE_;
        }
        if (p->count == PIPE_CAP) {      /* full: block (or report progress) */
            if (flags & 04000 /* O_NONBLOCK */) {
                sched_unlock();
                return done ? (int)done : -EAGAIN_;
            }
            wait_queue_sleep_locked(&p->wwq);
            continue;
        }
        while (done < n && p->count < PIPE_CAP) {
            p->buf[p->head] = in[done++];
            p->head = (p->head + 1) % PIPE_CAP;
            p->count++;
        }
        wait_queue_wake_all_locked(&p->rwq);    /* data available for readers */
    }
    sched_unlock();
    return (int)done;
}
