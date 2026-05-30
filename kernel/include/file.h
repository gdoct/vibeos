#ifndef VIBEOS_FILE_H
#define VIBEOS_FILE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Open-file objects (ROADMAP §4 rung 2).
 *
 * A `file_t` is the shared description behind a file descriptor: kind, the
 * backing VibeFS inode, the current byte offset (a dirent cursor for
 * directories), and the open flags. Descriptors in a process's fd table point
 * at these; fork() shares them (refcounted), matching Linux's "open file
 * description" semantics where a dup'd/inherited fd shares the offset.
 *
 * fds 0/1/2 are not file_t objects — they are handled implicitly as the serial
 * console by the syscall layer, so only real files/dirs occupy the pool.
 */

typedef enum {
    FD_NONE = 0,
    FD_FILE,        /* regular file */
    FD_DIR,         /* directory (offset is a dirent byte cursor) */
    FD_PIPE_RD,     /* read end of a pipe (uses `pipe`, not `ino`/`off`) */
    FD_PIPE_WR,     /* write end of a pipe */
} fd_kind_t;

struct pipe;            /* kernel/include/pipe.h */

typedef struct file {
    int       refcount;     /* 0 == free slot */
    fd_kind_t kind;
    uint32_t  ino;          /* backing VibeFS inode (FD_FILE/FD_DIR) */
    uint64_t  off;          /* file: byte offset; dir: dirent cursor */
    int       flags;        /* Linux open() flags */
    struct pipe *pipe;      /* pipe object (FD_PIPE_RD/WR) */
} file_t;

file_t *file_alloc(void);          /* refcount = 1, or NULL if the pool is full */
void    file_ref(file_t *f);       /* +1 */
void    file_unref(file_t *f);     /* -1; frees the slot at zero */

#ifdef __cplusplus
}
#endif

#endif
