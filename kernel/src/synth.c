#include "kernel.h"
#include "synth.h"
#include "task.h"
#include "tty.h"
#include "csprng.h"
#include "fb.h"
#include "input.h"

/*
 * Synthetic /dev and /proc (ROADMAP §4). See synth.h.
 *
 * /dev/{null,zero,full,random,urandom,tty} are char devices with the obvious
 * semantics. /proc has a directory per live task plus a "self" alias; each holds
 * a "stat" file rendered on the fly from the task table. Nothing here is backed
 * by VibeFS — the syscall layer routes synthetic paths to these helpers.
 */

/* /dev device subtypes (stored in file_t.dev). FB0/INPUT live in synth.h since
   the syscall + GUI layers reference them. */
enum { DEV_NULL = 1, DEV_ZERO, DEV_FULL, DEV_RANDOM, DEV_URANDOM, DEV_TTY,
       DEV_FB0 = SYNTH_DEV_FB0, DEV_INPUT = SYNTH_DEV_INPUT };
/* /proc per-pid file subtypes. */
enum { PROC_STAT = 1 };
/* Directory subtypes (file_t.dev for FD_DEVDIR). */
enum { DIR_DEV = 1, DIR_PROC, DIR_PROCPID };

static const struct { const char *name; int dev; } g_devs[] = {
    { "null", DEV_NULL }, { "zero", DEV_ZERO }, { "full", DEV_FULL },
    { "random", DEV_RANDOM }, { "urandom", DEV_URANDOM }, { "tty", DEV_TTY },
    { "fb0", DEV_FB0 }, { "input", DEV_INPUT },
};
#define N_DEVS ((int)(sizeof g_devs / sizeof g_devs[0]))

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* Minimal string builders (no kernel snprintf). Return the new length. */
static int put_str(char *d, int o, int cap, const char *s) {
    while (*s && o < cap - 1) d[o++] = *s++;
    return o;
}
static int put_int(char *d, int o, int cap, long v) {
    char rev[24]; int k = 0;
    unsigned long u = v < 0 ? (d[o < cap - 1 ? o++ : o] = '-', (unsigned long)(-v)) : (unsigned long)v;
    if (!u) rev[k++] = '0';
    while (u) { rev[k++] = (char)('0' + u % 10); u /= 10; }
    while (k && o < cap - 1) d[o++] = rev[--k];
    return o;
}

/* Parse a positive decimal; returns -1 if not all-digits / empty. */
static int parse_uint(const char *s) {
    if (!*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

/* Split "/a/b/c" into up to 4 components (after the leading '/'). Returns count.
   Writes pointers into the caller's mutable copy `work`. */
static int split(char *work, char *comp[4]) {
    int n = 0;
    char *p = work;
    while (*p == '/') p++;
    while (*p && n < 4) {
        comp[n++] = p;
        while (*p && *p != '/') p++;
        if (*p) { *p++ = '\0'; while (*p == '/') p++; }
    }
    return n;
}

/* Map a /proc directory component ("self" or a pid) to a live task id, or 0. */
static int proc_pid(const char *comp) {
    if (streq(comp, "self")) return task_current()->id;
    int pid = parse_uint(comp);
    if (pid > 0 && task_by_id(pid)) return pid;
    return 0;
}

int synth_classify(const char *path, uint64_t *size_out) {
    if (size_out) *size_out = 0;
    char work[256];
    unsigned i = 0;
    for (; path[i] && i < sizeof work - 1; i++) work[i] = path[i];
    work[i] = '\0';
    char *comp[4];
    int n = split(work, comp);

    if (n == 0) return SYNTH_NONE;                  /* "/" is the real root */

    if (streq(comp[0], "dev")) {
        if (n == 1) return SYNTH_DIR;               /* /dev */
        if (n == 2) {
            for (int k = 0; k < N_DEVS; k++)
                if (streq(comp[1], g_devs[k].name)) return SYNTH_NODE;
        }
        return SYNTH_NONE;
    }
    if (streq(comp[0], "proc")) {
        if (n == 1) return SYNTH_DIR;               /* /proc */
        if (n == 2) return proc_pid(comp[1]) ? SYNTH_DIR : SYNTH_NONE;   /* /proc/<pid> */
        if (n == 3 && proc_pid(comp[1]) && streq(comp[2], "stat"))
            return SYNTH_NODE;                      /* /proc/<pid>/stat */
        return SYNTH_NONE;
    }
    return SYNTH_NONE;
}

int synth_open(const char *path, file_t *f) {
    char work[256];
    unsigned i = 0;
    for (; path[i] && i < sizeof work - 1; i++) work[i] = path[i];
    work[i] = '\0';
    char *comp[4];
    int n = split(work, comp);
    if (n == 0) return -1;

    if (streq(comp[0], "dev")) {
        if (n == 1) { f->kind = FD_DEVDIR; f->dev = DIR_DEV; f->off = 0; return 0; }
        if (n == 2) {
            for (int k = 0; k < N_DEVS; k++)
                if (streq(comp[1], g_devs[k].name)) {
                    f->kind = FD_DEV; f->dev = g_devs[k].dev; f->off = 0;
                    if (f->dev == DEV_INPUT) input_set_grab(1);  /* GUI takes input */
                    return 0;
                }
            return -2;
        }
        return -1;
    }
    if (streq(comp[0], "proc")) {
        if (n == 1) { f->kind = FD_DEVDIR; f->dev = DIR_PROC; f->off = 0; return 0; }
        if (n == 2) {
            int pid = proc_pid(comp[1]);
            if (!pid) return -2;
            f->kind = FD_DEVDIR; f->dev = DIR_PROCPID; f->ino = (uint32_t)pid; f->off = 0;
            return 0;
        }
        if (n == 3 && streq(comp[2], "stat")) {
            int pid = proc_pid(comp[1]);
            if (!pid) return -2;
            f->kind = FD_PROC; f->dev = PROC_STAT; f->ino = (uint32_t)pid; f->off = 0;
            return 0;
        }
        return -1;
    }
    return -1;
}

/* ---- device read/write ---- */

int synth_read(file_t *f, void *buf, uint32_t n) {
    uint8_t *p = (uint8_t *)buf;
    if (f->kind == FD_DEV) {
        switch (f->dev) {
        case DEV_NULL:    return 0;                          /* EOF */
        case DEV_ZERO:
        case DEV_FULL:    for (uint32_t i = 0; i < n; i++) p[i] = 0; return (int)n;
        case DEV_RANDOM:
        case DEV_URANDOM: csprng_bytes(p, n); return (int)n;
        case DEV_TTY:     return tty_read((char *)buf, n);
        case DEV_FB0: {                                  /* geometry header */
            synth_fbinfo_t fi;
            fb_device_t *fb = fb_get();
            if (!fb) return -1;
            fi.width = fb->width; fi.height = fb->height;
            fi.pitch = fb->pitch; fi.bpp = 32;
            fi.size  = fb_size_bytes();
            uint32_t c = n < sizeof fi ? n : (uint32_t)sizeof fi;
            for (uint32_t i = 0; i < c; i++) p[i] = ((uint8_t *)&fi)[i];
            return (int)c;
        }
        case DEV_INPUT:   return input_read(buf, n);     /* event ring (non-blocking) */
        }
        return -1;
    }
    if (f->kind == FD_PROC) {
        /* Render the file into a scratch buffer, then serve [off, off+n). */
        char tmp[256];
        int len = 0;
        if (f->dev == PROC_STAT) {
            task_t *t = task_by_id((int)f->ino);
            if (!t) return 0;
            int ppid = t->parent ? t->parent->id : 0;
            /* pid (comm) state ppid ... — a minimal Linux /proc/<pid>/stat prefix. */
            char st = (t->state == 3) ? 'S' : 'R';   /* BLOCKED->S, else R */
            int o = 0, cap = (int)sizeof tmp;
            o = put_int(tmp, o, cap, t->id);
            o = put_str(tmp, o, cap, " (");
            o = put_str(tmp, o, cap, t->name ? t->name : "?");
            o = put_str(tmp, o, cap, ") ");
            if (o < cap - 1) tmp[o++] = st;
            o = put_str(tmp, o, cap, " ");
            o = put_int(tmp, o, cap, ppid);
            o = put_str(tmp, o, cap, " 0 0 0 -1 0 0 0 0 0 0 0\n");
            tmp[o] = '\0';
            len = o;
        }
        if (f->off >= (uint64_t)len) return 0;
        uint32_t avail = (uint32_t)(len - f->off);
        uint32_t c = n < avail ? n : avail;
        for (uint32_t i = 0; i < c; i++) p[i] = (uint8_t)tmp[f->off + i];
        f->off += c;
        return (int)c;
    }
    return -1;
}

int synth_write(file_t *f, const void *buf, uint32_t n) {
    (void)buf;
    if (f->kind == FD_DEV) {
        switch (f->dev) {
        case DEV_NULL:
        case DEV_ZERO:
        case DEV_RANDOM:
        case DEV_URANDOM: return (int)n;                     /* discard */
        case DEV_FULL:    return -28;                        /* -ENOSPC */
        case DEV_TTY:     serial_write((const char *)buf, n); return (int)n;
        }
    }
    return -1;
}

/* ---- directory enumeration ---- */

int synth_readdir(file_t *f, int index, char *name, uint32_t namesz, int *type) {
    if (f->kind != FD_DEVDIR) return 0;

    if (f->dev == DIR_DEV) {
        if (index >= N_DEVS) return 0;
        const char *nm = g_devs[index].name;
        unsigned i = 0; for (; nm[i] && i < namesz - 1; i++) name[i] = nm[i]; name[i] = '\0';
        *type = 3;       /* char device */
        return 1;
    }
    if (f->dev == DIR_PROC) {
        /* entries: "self" first, then one per live task id. */
        if (index == 0) {
            const char *nm = "self"; unsigned i = 0;
            for (; nm[i] && i < namesz - 1; i++) { name[i] = nm[i]; }
            name[i] = '\0';
            *type = 2; return 1;
        }
        int want = index - 1, seen = 0;
        for (int id = 1; id < MAX_TASKS; id++) {
            if (!task_by_id(id)) continue;
            if (seen++ == want) {
                int v = id, k = 0; char rev[12];
                if (!v) rev[k++] = '0';
                while (v) { rev[k++] = (char)('0' + v % 10); v /= 10; }
                unsigned i = 0; while (k && i < namesz - 1) name[i++] = rev[--k]; name[i] = '\0';
                *type = 2; return 1;
            }
        }
        return 0;
    }
    if (f->dev == DIR_PROCPID) {
        if (index == 0) {
            const char *nm = "stat"; unsigned i = 0;
            for (; nm[i] && i < namesz - 1; i++) { name[i] = nm[i]; }
            name[i] = '\0';
            *type = 1; return 1;
        }
        return 0;
    }
    return 0;
}
