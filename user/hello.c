/*
 * /bin/hello — a second userspace program, used to demonstrate execve
 * (ROADMAP §3 Milestone B). init forks, the child execve's this, and the
 * parent wait4's for its exit status (42).
 */

#include <stdint.h>

static long sys_write(int fd, const void *buf, unsigned long n) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(n)
                     : "rcx", "r11", "memory");
    return ret;
}

static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void puts1(const char *s) { sys_write(1, s, slen(s)); }

int main(int argc, char **argv) {
    puts1("    [hello] fresh execve image running");
    if (argc > 0 && argv && argv[0]) { puts1(", argv[0]="); puts1(argv[0]); }
    puts1("\n");
    return 42;
}
