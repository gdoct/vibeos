/*
 * VibeOS init — the first userspace process (ROADMAP §3, Milestone B).
 *
 * Minimal: print a banner and hand control to the shell by replacing this
 * image with /bin/sh. (A fuller init would fork+exec the shell in a loop and
 * respawn it; for now exec is enough — when the shell exits, the system halts.)
 * Freestanding, no libc.
 */

#include <stdint.h>

static long sys_write(int fd, const void *buf, unsigned long n) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return r;
}
static long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    long r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(59L), "D"(path), "S"(argv), "d"(envp) : "rcx", "r11", "memory");
    return r;
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

int main(void) {
    puts1("init: VibeOS userspace up, starting /bin/sh\n");
    char *const argv[] = { (char *)"/bin/sh", (char *)0 };
    sys_execve("/bin/sh", argv, (char *const *)0);
    puts1("init: cannot exec /bin/sh\n");
    return 1;
}
