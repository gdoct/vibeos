/*
 * pkg — a minimal VibeOS package tool (ROADMAP §4).
 *
 * A "package" is a plain POSIX ustar tar archive (so it can be produced by any
 * host `tar`); the metadata VibeFS understands — path, entry type, symlink
 * target, file size — is carried in the tar header and applied on extract.
 *
 *   pkg x <archive.tar> [destdir]   extract into destdir (default "/")
 *   pkg t <archive.tar>             list contents
 *
 * Built against musl (uses libc: open/read/write/mkdir/symlink).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define BLK 512

/* ustar header field offsets. */
struct tar_hdr {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static unsigned long oct(const char *s, int n) {
    unsigned long v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0');
    return v;
}

/* Join destdir + "/" + name into out. */
static void joinp(char *out, size_t cap, const char *dest, const char *name) {
    if (name[0] == '/') name++;
    snprintf(out, cap, "%s/%s", dest, name);
}

/* mkdir -p for the parent directories of `path`. */
static void mkparents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(path, 0755); *p = '/'; }
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || (argv[1][0] != 'x' && argv[1][0] != 't')) {
        fprintf(stderr, "usage: pkg x|t <archive.tar> [destdir]\n");
        return 2;
    }
    int list = (argv[1][0] == 't');
    const char *dest = (argc > 3) ? argv[3] : "";

    int fd = open(argv[2], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "pkg: cannot open %s\n", argv[2]); return 1; }

    struct tar_hdr h;
    int zeros = 0, count = 0;
    for (;;) {
        ssize_t r = read(fd, &h, BLK);
        if (r <= 0) break;
        if (r < BLK) { fprintf(stderr, "pkg: short read\n"); break; }
        if (h.name[0] == '\0') { if (++zeros >= 2) break; continue; }
        zeros = 0;

        unsigned long size = oct(h.size, 12);
        char full[512];
        char name[256];
        if (h.prefix[0]) snprintf(name, sizeof name, "%.155s/%.100s", h.prefix, h.name);
        else             snprintf(name, sizeof name, "%.100s", h.name);
        joinp(full, sizeof full, dest, name);

        if (list) {
            printf("%c %8lu  %s\n", h.typeflag ? h.typeflag : '0', size, name);
        } else if (h.typeflag == '5') {                 /* directory */
            mkparents(full);
            mkdir(full, 0755);
        } else if (h.typeflag == '2') {                 /* symlink */
            char tgt[101]; snprintf(tgt, sizeof tgt, "%.100s", h.linkname);
            mkparents(full);
            symlink(tgt, full);
        } else {                                        /* regular file ('0'/0) */
            mkparents(full);
            int out = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            unsigned long remaining = size;
            char buf[BLK];
            while (remaining > 0) {
                if (read(fd, buf, BLK) != BLK) { remaining = 0; break; }
                unsigned long w = remaining < BLK ? remaining : BLK;
                if (out >= 0) write(out, buf, w);
                remaining -= w;
            }
            if (out >= 0) close(out);
            count++;
            continue;                                   /* data already consumed */
        }

        /* Reached only for list mode or non-regular entries (extract of a
           regular file consumes its data and `continue`s above). Skip the data
           blocks so the next read lands on the following header. */
        unsigned long blocks = (size + BLK - 1) / BLK;
        for (unsigned long b = 0; b < blocks; b++) { char skip[BLK]; if (read(fd, skip, BLK) != BLK) break; }
    }
    close(fd);
    if (!list) printf("pkg: extracted %d file(s)\n", count);
    return 0;
}
