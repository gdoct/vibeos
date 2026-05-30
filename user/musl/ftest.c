/* §4 rung 2 smoke test: a static musl program that does real file I/O.
 * Writes a file, reads it back, stats it, and lists a directory — exercising
 * openat/read/write/lseek/fstat/getdents64 through the VibeOS fd table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp_ftest.txt";

    /* write */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open(write)"); return 1; }
    const char *msg = "vibeos file io works\n";
    if (write(fd, msg, strlen(msg)) != (long)strlen(msg)) { perror("write"); return 1; }
    close(fd);

    /* stat */
    struct stat st;
    if (stat(path, &st) == 0)
        printf("stat %s: size=%lld mode=%o\n", path, (long long)st.st_size, st.st_mode & 07777);
    else
        perror("stat");

    /* read back */
    fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open(read)"); return 1; }
    char buf[128]; long n = read(fd, buf, sizeof buf - 1);
    if (n < 0) { perror("read"); return 1; }
    buf[n] = 0;
    printf("read back (%ld bytes): %s", n, buf);
    close(fd);

    /* list a directory via opendir/readdir (getdents64) */
    const char *dir = "/bin";
    DIR *d = opendir(dir);
    if (!d) { perror("opendir"); return 1; }
    printf("ls %s:\n", dir);
    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL) {
        printf("  %s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
        count++;
    }
    closedir(d);
    printf("(%d entries)\n", count);
    return 0;
}
