/*
 * heartbeat — a tiny daemon to exercise the service-managed init (ROADMAP):
 * a long-running, supervised, non-shell service that logs to a file. Its
 * service file sets `log: /config/logs/heartbeat.log`, so init redirects this
 * output there; `cat` that file to see it accumulate. Built with the
 * x86_64-vibeos-musl cross compiler.
 */
#include <stdio.h>
#include <unistd.h>
#include <time.h>

int main(void) {
    for (int i = 1; ; i++) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        printf("heartbeat %d at +%lds\n", i, (long)ts.tv_sec);
        fflush(stdout);
        sleep(3);
    }
    return 0;
}
