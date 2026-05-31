#ifndef VIBEOS_SYNTH_H
#define VIBEOS_SYNTH_H

#include <stdint.h>
#include "file.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Synthetic filesystems (ROADMAP §4): /dev char devices and a tiny /proc.
 * These paths have no VibeFS inode — the syscall layer consults synth_* before
 * touching the real fs. Paths passed in are absolute + normalized.
 */

/* Path classification. */
#define SYNTH_NONE  0      /* not a synthetic path — fall through to VibeFS */
#define SYNTH_DIR   1      /* a synthetic directory (/dev, /proc, /proc/<pid>) */
#define SYNTH_NODE  2      /* a synthetic file (/dev/null, /proc/<pid>/stat, ...) */

/* Classify `path`; for a node/dir, optionally returns size (0 for devices). */
int  synth_classify(const char *path, uint64_t *size_out);

/* Open a synthetic path into `f` (kind/dev/ino set). Returns 0, or -1 if the
   path is not synthetic, or -2 if it is synthetic but does not exist. */
int  synth_open(const char *path, file_t *f);

/* Device/proc read+write (dispatch on f->kind/dev). Linux-style byte counts. */
int  synth_read(file_t *f, void *buf, uint32_t n);
int  synth_write(file_t *f, const void *buf, uint32_t n);

/* Enumerate directory entry #index (0-based) of the synthetic dir `f`.
   Returns 1 and fills name/type (1=file,2=dir), 0 at end. */
int  synth_readdir(file_t *f, int index, char *name, uint32_t namesz, int *type);

#ifdef __cplusplus
}
#endif

#endif
