/*
 * /bin/truntest — exercises ftruncate(2)/truncate(2) end-to-end against VibeFS.
 *
 * Covers the paths that the shell builtins can't reach: growing (sparse, must
 * read back as zeros), shrinking within the direct blocks, and shrinking a file
 * large enough to span single-indirect blocks (exercises fs.c bfree_at). Prints
 * one PASS/FAIL line per check; verified via the serial log.
 */

#include <stdint.h>

#define O_WRONLY 0x1
#define O_RDWR   0x2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define SEEK_SET 0

static long sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long sc3(long n, long a, long b, long c) {
    long r; register long r10 __asm__("r10")=0; (void)r10;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
static long sc4(long n, long a, long b, long c, long d) {
    long r; register long r10 __asm__("r10")=d;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory"); return r;
}

static long sys_write(int fd, const void *b, unsigned long n){ return sc3(1,fd,(long)b,(long)n); }
static long sys_read (int fd, void *b, unsigned long n)      { return sc3(0,fd,(long)b,(long)n); }
static long sys_close(int fd)                               { return sc1(3,fd); }
static long sys_lseek(int fd, long off, int w)             { return sc3(8,fd,off,w); }
static long sys_openat(const char *p, long fl, long m)     { return sc4(257,-100,(long)p,fl,m); }
static long sys_ftruncate(int fd, long len)                { return sc3(77,fd,len,0); }
static long sys_truncate(const char *p, long len)          { return sc3(76,(long)p,len,0); }
static long sys_unlink(const char *p)                      { return sc1(87,(long)p); }
__attribute__((noreturn)) static void sys_exit(int c){ sc1(60,c); __builtin_unreachable(); }

static unsigned long slen(const char *s){ unsigned long n=0; while(s[n])n++; return n; }
static void puts1(const char *s){ sys_write(1,s,slen(s)); }
static int nfail = 0;
static void check(const char *name, int ok){
    puts1(ok ? "  PASS " : "  FAIL "); puts1(name); puts1("\n"); if(!ok) nfail++;
}

int main(void) {
    puts1("[truntest] start\n");
    char buf[2048];

    long fd = sys_openat("/truntest.tmp", O_RDWR|O_CREAT|O_TRUNC, 0644);
    check("open create", fd >= 0);
    if (fd < 0) sys_exit(1);

    /* --- write 10 bytes, shrink to 4, read back --- */
    sys_write((int)fd, "ABCDEFGHIJ", 10);
    check("ftruncate shrink->4", sys_ftruncate((int)fd, 4) == 0);
    sys_lseek((int)fd, 0, SEEK_SET);
    long n = sys_read((int)fd, buf, sizeof buf);
    check("shrunk size == 4", n == 4);
    check("shrunk data 'ABCD'", buf[0]=='A'&&buf[1]=='B'&&buf[2]=='C'&&buf[3]=='D');

    /* --- grow to 8: bytes 4..7 must read back as zero (sparse) --- */
    check("ftruncate grow->8", sys_ftruncate((int)fd, 8) == 0);
    sys_lseek((int)fd, 0, SEEK_SET);
    n = sys_read((int)fd, buf, sizeof buf);
    check("grown size == 8", n == 8);
    check("grow zero-extends", buf[4]==0&&buf[5]==0&&buf[6]==0&&buf[7]==0);
    check("grow keeps prefix", buf[0]=='A'&&buf[3]=='D');

    /* --- large file spanning single-indirect, then shrink (exercises bfree_at) --- */
    static char big[70000];
    for (int i = 0; i < (int)sizeof big; i++) big[i] = (char)((i*7+3)&0xff);
    sys_lseek((int)fd, 0, SEEK_SET);
    sys_ftruncate((int)fd, 0);
    long w = sys_write((int)fd, big, sizeof big);
    check("write 70000 (>direct reach)", w == (long)sizeof big);
    check("ftruncate large shrink->100", sys_ftruncate((int)fd, 100) == 0);
    sys_lseek((int)fd, 0, SEEK_SET);
    n = sys_read((int)fd, buf, sizeof buf);
    int ok = (n == 100);
    for (int i = 0; i < 100 && ok; i++) ok = (buf[i] == (char)((i*7+3)&0xff));
    check("large-shrink data intact", ok);

    /* re-grow across the freed region: must read zeros, not stale indirect data */
    sys_ftruncate((int)fd, 60000);
    sys_lseek((int)fd, 55000, SEEK_SET);
    n = sys_read((int)fd, buf, 64);
    ok = (n == 64);
    for (int i = 0; i < 64 && ok; i++) ok = (buf[i] == 0);
    check("re-grown hole reads zero", ok);

    /* --- path-based truncate to 0 --- */
    check("truncate(path,0)", sys_truncate("/truntest.tmp", 0) == 0);
    sys_lseek((int)fd, 0, SEEK_SET);
    n = sys_read((int)fd, buf, sizeof buf);
    check("truncated size == 0", n == 0);

    sys_close((int)fd);
    sys_unlink("/truntest.tmp");

    if (nfail == 0) puts1("[truntest] ALL PASS\n");
    else            puts1("[truntest] FAILURES\n");
    return nfail;
}
