/*
 * MyOS init — the first userspace program (ROADMAP §3, Phase 5A).
 *
 * Freestanding: no libc. It talks to the kernel directly through the Linux
 * x86_64 syscall ABI (rax = number; args in rdi, rsi, rdx, r10, r8, r9;
 * `syscall` clobbers rcx and r11). For Milestone A it proves the round trip:
 * write to stdout, exercise the stack the loader built (argv), then exit.
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

static long sys_getpid(void) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(39L) : "rcx", "r11", "memory");
    return ret;
}

__attribute__((noreturn))
static void sys_exit(int code) {
    __asm__ volatile("syscall" :: "a"(60L), "D"((long)code) : "rcx", "r11", "memory");
    __builtin_unreachable();
}

static unsigned long slen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static void puts1(const char *s) { sys_write(1, s, slen(s)); }

/* Tiny unsigned-to-decimal, just enough to print the pid. */
static void put_ulong(unsigned long v) {
    char buf[21];
    int i = (int)sizeof(buf);
    buf[--i] = '\0';
    if (v == 0) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    puts1(&buf[i]);
}

int main(int argc, char **argv) {
    puts1("hello from ring 3 (userspace init)\n");

    puts1("  pid = ");
    put_ulong((unsigned long)sys_getpid());
    puts1("\n");

    if (argc > 0 && argv && argv[0]) {
        puts1("  argv[0] = ");
        puts1(argv[0]);
        puts1("\n");
    }

    return 0;
}
