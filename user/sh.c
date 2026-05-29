/*
 * /bin/sh — a tiny interactive shell for VibeOS (ROADMAP §3 B.4).
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
            puts1("builtins: help, exit\n"
                  "run a program by name (hello) or path (/bin/hello)\n");
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
