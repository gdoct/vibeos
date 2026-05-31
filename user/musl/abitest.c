/*
 * abitest — exercises the widened Linux ABI (ROADMAP) through musl: uname,
 * clock_gettime, getrandom (via getentropy), and getsockname on a bound socket.
 * Built with the x86_64-vibeos-musl cross compiler.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

int main(void) {
    struct utsname u;
    if (uname(&u) == 0)
        printf("uname: %s %s %s (%s)\n", u.sysname, u.release, u.machine, u.version);
    else
        printf("uname: FAILED\n");

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        printf("clock_gettime: %lld.%03lld s since boot\n",
               (long long)ts.tv_sec, (long long)(ts.tv_nsec / 1000000));
    else
        printf("clock_gettime: FAILED\n");

    unsigned char r[8];
    if (getentropy(r, sizeof r) == 0) {
        printf("getrandom: ");
        for (unsigned i = 0; i < sizeof r; i++) printf("%02x", r[i]);
        printf("\n");
    } else {
        printf("getrandom: FAILED\n");
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(4242);
    sa.sin_addr.s_addr = htonl(0x0A00020F);          /* 10.0.2.15 */
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) == 0) {
        struct sockaddr_in got;
        socklen_t gl = sizeof got;
        if (getsockname(s, (struct sockaddr *)&got, &gl) == 0)
            printf("getsockname: bound to %s:%d\n",
                   inet_ntoa(got.sin_addr), ntohs(got.sin_port));
        else
            printf("getsockname: FAILED\n");
    } else {
        printf("bind: FAILED\n");
    }
    close(s);

    /* access(2) — existence check. */
    printf("access(/bin/sh)=%d access(/nope)=%d\n",
           access("/bin/sh", F_OK), access("/nope", F_OK));

    /* statx(2) — modern stat, called raw (this musl is too old to declare it). */
    struct statx_min {
        unsigned mask, blksize; unsigned long long attributes;
        unsigned nlink, uid, gid; unsigned short mode, pad1;
        unsigned long long ino, size, blocks;
        unsigned char rest[128];
    } stx;
    long sr = syscall(332 /*statx*/, AT_FDCWD, "/bin/sh", 0, 0x7ff, &stx);
    if (sr == 0)
        printf("statx(/bin/sh): mode=%o size=%llu\n", stx.mode & 07777, stx.size);
    else
        printf("statx: FAILED (%ld)\n", sr);

    /* fcntl advisory lock — should succeed (no real locking). */
    int lf = open("/tmp_lockfile", O_RDWR | O_CREAT, 0644);
    if (lf >= 0) {
        struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };
        printf("fcntl F_SETLK=%d\n", fcntl(lf, F_SETLK, &fl));
        close(lf);
    }

    printf("abitest done\n");
    return 0;
}
