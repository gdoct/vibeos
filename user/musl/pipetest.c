/* §4 pipe smoke test: fork a child, send bytes through a pipe, read them back,
 * and check blocking + EOF. Also exercises dup2-style fd plumbing implicitly
 * via the standard pipe()/fork() pattern. Static musl. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* child: write three messages, then close to signal EOF */
        close(fds[0]);
        const char *msgs[] = { "one\n", "two\n", "three\n" };
        for (int i = 0; i < 3; i++)
            write(fds[1], msgs[i], strlen(msgs[i]));
        close(fds[1]);
        _exit(0);
    }

    /* parent: read until EOF, print what arrived */
    close(fds[1]);
    char buf[128];
    long total = 0, n;
    printf("parent reading pipe:\n");
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        write(1, buf, n);
        total += n;
    }
    close(fds[0]);
    int status = 0;
    wait(&status);
    printf("pipe test done: %ld bytes, child status=%d\n", total, status);
    return 0;
}
