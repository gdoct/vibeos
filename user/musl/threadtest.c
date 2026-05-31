/*
 * threadtest — exercises kernel threads (ROADMAP): clone, futex, gettid/tgid,
 * CHILD_CLEARTID. Spawns musl pthreads that hammer a mutex-protected counter,
 * then joins them — pthread_create => clone(CLONE_VM|CLONE_FILES|CLONE_THREAD|
 * CLONE_SETTLS|...), pthread_mutex => futex, pthread_join => futex on the
 * CHILD_CLEARTID word. Built with the x86_64-vibeos-musl cross compiler.
 */
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#define NTHREADS 4
#define ITERS    20000

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

static void *worker(void *arg) {
    long id = (long)arg;
    for (int i = 0; i < ITERS; i++) {
        pthread_mutex_lock(&mtx);
        counter++;
        pthread_mutex_unlock(&mtx);
    }
    printf("  thread %ld (tid via gettid differs from pid) done\n", id);
    return (void *)(id * 100 + 1);
}

int main(void) {
    printf("threadtest: pid=%d, spawning %d threads x %d iters\n",
           getpid(), NTHREADS, ITERS);
    pthread_t t[NTHREADS];
    for (long i = 0; i < NTHREADS; i++) {
        if (pthread_create(&t[i], NULL, worker, (void *)i) != 0) {
            printf("  pthread_create %ld FAILED\n", i);
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) {
        void *r = NULL;
        pthread_join(t[i], &r);
        printf("  joined thread %d, ret=%ld\n", i, (long)r);
    }
    printf("threadtest: counter=%ld (expected %d) -> %s\n",
           counter, NTHREADS * ITERS,
           counter == (long)NTHREADS * ITERS ? "OK" : "MISMATCH");
    return 0;
}
