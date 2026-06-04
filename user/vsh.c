/*
 * /bin/vsh — the "vibe shell", a tiny interactive shell for VibeOS (ROADMAP §3 B.4).
 *
 * This is the gap-filler shell: it stands in as the default until a real shell
 * (mksh, see packages/mksh) is ported, after which /bin/sh is symlinked to
 * /bin/mksh and vsh remains available at /bin/vsh as a freestanding fallback.
 *
 * Reads a line from the serial console (read(0), backed by the kernel TTY),
 * splits it into words, and runs the command by fork + execve + wait4. A bare
 * name is resolved against /bin (so `hello` runs /bin/hello); a path with '/'
 * is used as-is. Builtins: help, exit. Freestanding, no libc.
 */

#include <stdint.h>

static long sys_read(int fd, void *buf, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(0L), "D"((long)fd), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return r;
}
static long sys_write(int fd, const void *buf, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return r;
}
static long sys_chdir(const char *path) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(80L), "D"(path) : "rcx", "r11", "memory");
    return r;
}
static long sys_getcwd(char *buf, unsigned long size) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(79L), "D"(buf), "S"(size) : "rcx", "r11", "memory");
    return r;
}
static long sys_openat(int dfd, const char *path, long flags, long mode) {
    long r;
    register long r10 __asm__("r10") = mode;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(257L), "D"((long)dfd), "S"(path), "d"(flags), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static long sys_close(int fd) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(3L), "D"((long)fd) : "rcx", "r11", "memory");
    return r;
}
static long sys_getdents64(int fd, void *buf, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(217L), "D"((long)fd), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return r;
}
static long sys_symlink(const char *target, const char *link) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(88L), "D"(target), "S"(link) : "rcx", "r11", "memory");
    return r;
}
static long sys_readlink(const char *path, char *buf, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(89L), "D"(path), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return r;
}
static long sys_mkdir(const char *path, long mode) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(83L), "D"(path), "S"(mode) : "rcx", "r11", "memory");
    return r;
}
static long sys_unlink(const char *path) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(87L), "D"(path) : "rcx", "r11", "memory");
    return r;
}
static long sys_rmdir(const char *path) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(84L), "D"(path) : "rcx", "r11", "memory");
    return r;
}
static long sys_rename(const char *old, const char *neww) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(82L), "D"(old), "S"(neww) : "rcx", "r11", "memory");
    return r;
}
static long sys_fork(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(57L) : "rcx", "r11", "memory");
    return r;
}
static long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(59L), "D"(path), "S"(argv), "d"(envp) : "rcx", "r11", "memory");
    return r;
}
static long sys_wait4(int pid, int *status, int options, void *rusage) {
    long r;
    register long r10 __asm__("r10") = (long)rusage;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(61L), "D"((long)pid), "S"(status), "d"((long)options), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
__attribute__((noreturn))
static void sys_exit(int code) {
    __asm__ volatile("syscall" :: "a"(60L), "D"((long)code) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }
static int  streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* tiny unsigned decimal */
static void put_uint(unsigned long v) {
    char b[21]; int i = (int)sizeof(b); b[--i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    puts1(&b[i]);
}

int main(void) {
    char line[256];
    char path[128];

    puts1("VibeOS shell. type 'help'.\n");
    for (;;) {
        puts1("vibe$ ");
        long n = sys_read(0, line, sizeof(line) - 1);
        if (n <= 0) break;                       /* console closed */
        if (line[n - 1] == '\n') n--;            /* strip newline */
        line[n] = 0;

        /* Split into argv on spaces (in place). */
        char *argv[16];
        int argc = 0;
        char *p = line;
        while (*p && argc < 15) {
            while (*p == ' ') *p++ = 0;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
        }
        argv[argc] = 0;
        if (argc == 0) continue;

        if (streq(argv[0], "exit")) { puts1("bye\n"); sys_exit(0); }
        if (streq(argv[0], "help")) {
            puts1("builtins: help, exit, cd, pwd, echo, cat, ls, ln -s, readlink,\n"
                  "          mkdir, touch, rm, rmdir, mv\n"
                  "run a program by name (hello) or path (/bin/hello)\n");
            continue;
        }
        if (streq(argv[0], "cd")) {
            const char *dir = argc > 1 ? argv[1] : "/";
            if (sys_chdir(dir) < 0) { puts1("cd: "); puts1(dir); puts1(": no such directory\n"); }
            continue;
        }
        if (streq(argv[0], "pwd")) {
            char cwd[256];
            if (sys_getcwd(cwd, sizeof cwd) >= 0) { puts1(cwd); puts1("\n"); }
            continue;
        }
        if (streq(argv[0], "ln")) {                  /* ln -s target link */
            if (argc == 4 && streq(argv[1], "-s")) {
                if (sys_symlink(argv[2], argv[3]) < 0) puts1("ln: symlink failed\n");
            } else puts1("usage: ln -s target link\n");
            continue;
        }
        if (streq(argv[0], "readlink")) {
            if (argc > 1) {
                char tgt[256]; long m = sys_readlink(argv[1], tgt, sizeof tgt - 1);
                if (m >= 0) { tgt[m] = 0; puts1(tgt); puts1("\n"); }
                else puts1("readlink: not a symlink\n");
            }
            continue;
        }
        if (streq(argv[0], "id")) {
            long uid, gid;
            __asm__ volatile("syscall" : "=a"(uid) : "a"(102L) : "rcx", "r11", "memory");
            __asm__ volatile("syscall" : "=a"(gid) : "a"(104L) : "rcx", "r11", "memory");
            puts1("uid="); put_uint((unsigned long)uid);
            puts1(" gid="); put_uint((unsigned long)gid); puts1("\n");
            continue;
        }
        if (streq(argv[0], "mkdir")) {
            if (argc < 2) { puts1("usage: mkdir dir...\n"); continue; }
            for (int i = 1; i < argc; i++)
                if (sys_mkdir(argv[i], 0755) < 0) { puts1("mkdir: "); puts1(argv[i]); puts1(": failed\n"); }
            continue;
        }
        if (streq(argv[0], "touch")) {
            if (argc < 2) { puts1("usage: touch file...\n"); continue; }
            for (int i = 1; i < argc; i++) {
                long fd = sys_openat(-100, argv[i], 0x41 /*O_WRONLY|O_CREAT*/, 0644);
                if (fd < 0) { puts1("touch: "); puts1(argv[i]); puts1(": failed\n"); }
                else sys_close((int)fd);
            }
            continue;
        }
        if (streq(argv[0], "rm")) {
            if (argc < 2) { puts1("usage: rm file...\n"); continue; }
            for (int i = 1; i < argc; i++)
                if (sys_unlink(argv[i]) < 0) { puts1("rm: "); puts1(argv[i]); puts1(": failed\n"); }
            continue;
        }
        if (streq(argv[0], "rmdir")) {
            if (argc < 2) { puts1("usage: rmdir dir...\n"); continue; }
            for (int i = 1; i < argc; i++)
                if (sys_rmdir(argv[i]) < 0) { puts1("rmdir: "); puts1(argv[i]); puts1(": failed\n"); }
            continue;
        }
        if (streq(argv[0], "mv")) {
            if (argc != 3) { puts1("usage: mv old new\n"); continue; }
            if (sys_rename(argv[1], argv[2]) < 0) puts1("mv: failed\n");
            continue;
        }
        if (streq(argv[0], "echo")) {
            for (int i = 1; i < argc; i++) { puts1(argv[i]); if (i + 1 < argc) puts1(" "); }
            puts1("\n");
            continue;
        }
        if (streq(argv[0], "cat")) {
            for (int i = 1; i < argc; i++) {
                long fd = sys_openat(-100, argv[i], 0 /*O_RDONLY*/, 0);
                if (fd < 0) { puts1("cat: "); puts1(argv[i]); puts1(": cannot open\n"); continue; }
                char buf[512]; long m;
                while ((m = sys_read((int)fd, buf, sizeof buf)) > 0) sys_write(1, buf, (unsigned long)m);
                sys_close((int)fd);
            }
            continue;
        }
        if (streq(argv[0], "ls")) {
            const char *dir = argc > 1 ? argv[1] : ".";
            long fd = sys_openat(-100, dir, 0x10000 /*O_DIRECTORY*/, 0);
            if (fd < 0) { puts1("ls: "); puts1(dir); puts1(": cannot open\n"); continue; }
            char dbuf[1024]; long m;
            while ((m = sys_getdents64((int)fd, dbuf, sizeof dbuf)) > 0) {
                long off = 0;
                while (off < m) {
                    /* struct linux_dirent64: ino(8) off(8) reclen(2) type(1) name... */
                    unsigned short reclen = *(unsigned short *)(dbuf + off + 16);
                    const char *name = dbuf + off + 19;
                    puts1(name); puts1("  ");
                    off += reclen;
                }
            }
            puts1("\n");
            sys_close((int)fd);
            continue;
        }

        /* Resolve the program path: bare name -> /bin/<name>. */
        const char *prog = argv[0];
        int has_slash = 0;
        for (const char *q = prog; *q; q++) if (*q == '/') { has_slash = 1; break; }
        if (!has_slash) {
            int i = 0; const char *pre = "/bin/";
            while (pre[i]) { path[i] = pre[i]; i++; }
            int j = 0;
            while (prog[j] && i < (int)sizeof(path) - 1) path[i++] = prog[j++];
            path[i] = 0;
            prog = path;
        }

        long pid = sys_fork();
        if (pid == 0) {
            sys_execve(prog, argv, (char *const *)0);
            puts1("sh: command not found: ");
            puts1(argv[0]);
            puts1("\n");
            sys_exit(127);
        } else if (pid > 0) {
            int status = 0;
            sys_wait4((int)pid, &status, 0, (void *)0);
            int code = (status >> 8) & 0xff;
            if (code != 0) {
                puts1("[exit ");
                put_uint((unsigned long)code);
                puts1("]\n");
            }
        } else {
            puts1("sh: fork failed\n");
        }
    }
    return 0;
}
