/* §2 multicore test (static musl): fork several CPU-bound children and have
 * each report, via getcpu(2), which core it is running on. With user tasks no
 * longer pinned to the BSP, the samples should include cores other than 0. */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>

static unsigned getcpu_(void) {
    unsigned cpu = 0xffffffffu;
    syscall(SYS_getcpu, &cpu, (void *)0, (void *)0);
    return cpu;
}

int main(void) {
    const int N = 4;
    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        if (p == 0) {
            setvbuf(stdout, NULL, _IONBF, 0);          /* unbuffered: see each sample */
            for (int k = 0; k < 5; k++) {
                volatile unsigned long x = 0;          /* burn time to get rescheduled */
                for (unsigned long j = 0; j < 4000000UL; j++) x += j;
                printf("child %d sample %d -> cpu %u\n", i, k, getcpu_());
            }
            _exit(0);
        }
    }
    for (int i = 0; i < N; i++) wait((void *)0);
    printf("cputest done\n");
    return 0;
}
