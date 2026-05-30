/* §1.1 memory-safety smoke test (static musl).
 *
 *   faulttest          -> hand the kernel bad user pointers and check the
 *                         syscalls return -EFAULT instead of faulting in-kernel.
 *   faulttest segv      -> dereference an unmapped address; the kernel must kill
 *                         just this process (SIGSEGV) and keep running, so the
 *                         shell gets control back. (We never reach the print.)
 *   faulttest cow       -> fork and have parent+child each scribble over a big
 *                         shared buffer, proving copy-on-write isolates them.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "segv") == 0) {
        volatile int *p = (volatile int *)0x12345678UL;   /* unmapped */
        *p = 1;                                            /* write fault */
        printf("BUG: survived the segfault\n");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "cow") == 0) {
        char *buf = malloc(64 * 1024);
        memset(buf, 'P', 64 * 1024);          /* parent's pattern */
        pid_t pid = fork();
        if (pid == 0) {
            memset(buf, 'C', 64 * 1024);      /* child rewrites its private copy */
            _exit(buf[0] == 'C' ? 0 : 1);
        }
        int st = 0; wait(&st);
        int ok = (buf[0] == 'P' && buf[64 * 1024 - 1] == 'P');  /* parent intact */
        printf("cow: parent buf %s after child rewrote its copy (child status=%d)\n",
               ok ? "INTACT" : "CORRUPTED", WEXITSTATUS(st));
        return ok ? 0 : 1;
    }

    /* Default: bad-pointer syscalls must return EFAULT, not crash the kernel. */
    errno = 0;
    long r = write(1, (void *)0x12345678UL, 16);
    printf("write(badptr) = %ld errno=%d %s\n", r, errno,
           (r == -1 && errno == EFAULT) ? "(EFAULT, good)" : "(UNEXPECTED)");

    struct stat { char x[144]; } *bad = (void *)0x12345678UL;
    errno = 0;
    r = stat("/bin", (void *)bad);
    printf("stat(/bin, badptr) = %ld errno=%d %s\n", r, errno,
           (r == -1 && errno == EFAULT) ? "(EFAULT, good)" : "(UNEXPECTED)");
    return 0;
}
