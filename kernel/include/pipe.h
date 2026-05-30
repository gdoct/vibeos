#ifndef VIBEOS_PIPE_H
#define VIBEOS_PIPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Anonymous pipes (ROADMAP §4 rung 2 — shell pipelines).
 *
 * A pipe is a fixed-size byte ring with two ends, each fronted by a file_t
 * (FD_PIPE_RD / FD_PIPE_WR). The pipe tracks how many of each end are still
 * open: when the last writer closes, blocked readers wake to EOF; when the last
 * reader closes, blocked writers get -EPIPE (we have no SIGPIPE). The end-open
 * counts are tied to the file_t lifetime, so fork() — which just bumps the
 * file_t refcount — keeps an end "open" until its last descriptor is closed.
 *
 * Reads/writes block via the scheduler's wait queues. To avoid a lost wakeup,
 * the buffer check and the sleep run with interrupts off (user tasks are
 * BSP-pinned, so same-CPU preemption is the only race to close).
 */

struct file;            /* kernel/include/file.h */
typedef struct pipe pipe_t;

pipe_t *pipe_create(void);                 /* readers=writers=1, or NULL on OOM */
void    pipe_free(pipe_t *p);              /* unconditional free (creation-error path) */

/* Called from file_unref when a pipe-end file_t hits refcount 0: drops that
   end's open count, wakes the other side, and frees the pipe once both ends
   are gone. */
void    pipe_detach(struct file *f);

/* Blocking unless O_NONBLOCK is set in `flags`. read returns 0 at EOF;
   write returns -EPIPE if all readers are gone. */
int     pipe_read (pipe_t *p, void *buf, uint32_t n, int flags);
int     pipe_write(pipe_t *p, const void *buf, uint32_t n, int flags);

#ifdef __cplusplus
}
#endif

#endif
