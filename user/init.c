/*
 * MyOS init — the first userspace program (ROADMAP §3, Milestone B).
 *
 * Freestanding: no libc. Talks to the kernel through the Linux x86_64 syscall
 * ABI (rax = number; args in rdi, rsi, rdx, r10, r8, r9; `syscall` clobbers
 * rcx and r11). Demonstrates the process model: fork a child, the child
 * execve's /bin/hello, and the parent wait4's for its exit status.
 */

#include <stdint.h>

static long sys_write(int fd, const void *buf, unsigned long n) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(n) : "rcx", "r11", "memory");
    return ret;
}
static long sys_getpid(void) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(39L) : "rcx", "r11", "memory");
    return ret;
}
static long sys_fork(void) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(57L) : "rcx", "r11", "memory");
    return ret;
}
static long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(59L), "D"(path), "S"(argv), "d"(envp) : "rcx", "r11", "memory");
    return ret;
}
static long sys_wait4(int pid, int *status, int options, void *rusage) {
    long ret;
    register long r10 __asm__("r10") = (long)rusage;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(61L), "D"((long)pid), "S"(status), "d"((long)options), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}
__attribute__((noreturn))
static void sys_exit(int code) {
    __asm__ volatile("syscall" :: "a"(60L), "D"((long)code) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

/* "<pfx><n><sfx>" in one write so parent/child lines don't interleave. */
static void emit(const char *pfx, unsigned long n, const char *sfx) {
    char buf[96]; unsigned i = 0;
    for (const char *p = pfx; *p && i < 80; p++) buf[i++] = *p;
    char num[21]; int j = (int)sizeof(num); num[--j] = '\0';
    if (n == 0) num[--j] = '0';
    while (n) { num[--j] = (char)('0' + n % 10); n /= 10; }
    for (const char *p = &num[j]; *p && i < 90; p++) buf[i++] = *p;
    for (const char *p = sfx; *p && i < 95; p++) buf[i++] = *p;
    sys_write(1, buf, i);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    puts1("hello from ring 3 (userspace init)\n");
    emit("  pid = ", (unsigned long)sys_getpid(), "\n");

    long pid = sys_fork();
    if (pid == 0) {
        /* Child: replace this image with /bin/hello. */
        char *const av[] = { (char *)"/bin/hello", (char *)0 };
        sys_execve("/bin/hello", av, (char *const *)0);
        puts1("  [child] execve failed\n");     /* only reached on failure */
        sys_exit(127);
    }

    /* Parent: reap the child and report its exit status. */
    int status = 0;
    long w = sys_wait4((int)pid, &status, 0, (void *)0);
    emit("  [parent] reaped child pid=", (unsigned long)w, "");
    emit(", exit status=", (unsigned long)((status >> 8) & 0xff), "\n");
    return 0;
}
