/*
 * VibeOS init — the first userspace process (ROADMAP §3/§4).
 *
 * A real (small) init: it forks + execs /bin/sh and waits for it, respawning
 * the shell whenever it exits, so the console is never dead. It also reaps any
 * orphaned children that reparent to it (PID-1 duty). Freestanding, no libc.
 */

#include <stdint.h>

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

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

int main(void) {
    puts1("init: VibeOS userspace up (PID 1), supervising /bin/sh\n");
    char *const argv[] = { (char *)"/bin/sh", (char *)0 };
    char *const envp[] = { (char *)"PATH=/bin", (char *)"HOME=/", (char *)0 };

    for (;;) {
        long pid = sys_fork();
        if (pid == 0) {                          /* child: become the shell */
            sys_execve("/bin/sh", argv, envp);
            puts1("init: cannot exec /bin/sh\n");
            __asm__ volatile("syscall" :: "a"(60L), "D"(1L) : "rcx", "r11");  /* exit(1) */
        }
        if (pid < 0) { puts1("init: fork failed\n"); break; }

        /* Reap children until the shell we spawned exits, then respawn it. */
        for (;;) {
            int status = 0;
            long w = sys_wait4(-1, &status, 0, (void *)0);
            if (w == pid) break;                 /* the shell exited: respawn */
            if (w < 0) break;                    /* no children left */
        }
        puts1("init: shell exited, respawning\n");
    }
    return 0;
}
