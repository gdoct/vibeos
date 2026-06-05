/*
 * pkg — VibeOS package tool, v1 (docs/pkgman.md).
 *
 * A package is a POSIX ustar archive with a `.pkg` extension carrying a
 * `package_info.yml` manifest at its root plus a staged file tree. This tool
 * lists, extracts, and creates such archives. It does NOT resolve dependencies,
 * track installed packages, uninstall, build ports, or run maintainer scripts —
 * those belong to the "future" layer described in docs/pkgman.md.
 *
 *   pkg list    <archive.pkg>
 *   pkg extract <archive.pkg> <destdir>
 *   pkg create  <pkgdir> [--output <archive.pkg>] [--include <path>]...
 *
 * Built as a static x86_64-linux-musl binary, so it runs unmodified both on the
 * host (for image-time `pkg create`) and on VibeOS over the serial shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

#define BLK 512

/* POSIX ustar header (one 512-byte block). */
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

/* ---- small helpers ------------------------------------------------------ */

static unsigned long oct(const char *s, int n) {
    unsigned long v = 0;
    for (int i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (s[i] - '0');
    return v;
}

/* Write `val` as a NUL-terminated octal field of width `n` (right-justified,
   zero-padded) — the classic ustar numeric encoding. */
static void putoct(char *dst, int n, unsigned long val) {
    dst[n - 1] = '\0';
    for (int i = n - 2; i >= 0; i--) { dst[i] = '0' + (val & 7); val >>= 3; }
}

/* Read exactly `n` bytes (handling short reads); returns n, 0 at EOF, <0 err. */
static ssize_t read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) return -1;
        if (r == 0) break;
        got += r;
    }
    return got;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w <= 0) return -1;
        put += w;
    }
    return 0;
}

/* Reject paths that could escape the destination: absolute paths and any ".."
   component. Leading "./" and embedded "." components are harmless. Returns 1 if
   safe, 0 otherwise. */
static int path_is_safe(const char *p) {
    if (p[0] == '/') return 0;
    const char *s = p;
    while (*s) {
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - s);
        if (len == 2 && s[0] == '.' && s[1] == '.') return 0;
        s = (*e == '/') ? e + 1 : e;
    }
    return 1;
}

static int path_is_current_dir(const char *p) {
    return !strcmp(p, ".") || !strcmp(p, "./");
}

static int path_join_checked(char *out, size_t cap, const char *base, const char *rel) {
    if (!rel[0] || path_is_current_dir(rel)) {
        size_t blen = strlen(base);
        if (blen + 1 > cap) {
            fprintf(stderr, "pkg: path too long\n");
            return -1;
        }
        memcpy(out, base, blen + 1);
        return 0;
    }
    if (!path_is_safe(rel)) {
        fprintf(stderr, "pkg: refusing unsafe relative path %s\n", rel);
        return -1;
    }
    size_t blen = strlen(base);
    size_t rlen = strlen(rel);
    if (blen + 1 + rlen + 1 > cap) {
        fprintf(stderr, "pkg: path too long\n");
        return -1;
    }
    memcpy(out, base, blen);
    out[blen] = '/';
    memcpy(out + blen + 1, rel, rlen + 1);
    return 0;
}

static int path_join3_checked(char *out, size_t cap, const char *a, const char *b, const char *c) {
    char tmp[1024];
    if (path_join_checked(tmp, sizeof tmp, a, b) < 0) return -1;
    return path_join_checked(out, cap, tmp, c);
}

/* mkdir -p for every parent directory of `path` (path itself is left alone). */
static void mkparents(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(buf, 0755); *p = '/'; }
    }
}

/* Join dest + "/" + name, normalizing the slash. */
static void joinp(char *out, size_t cap, const char *dest, const char *name) {
    while (name[0] == '/') name++;
    if (!dest[0] || (dest[0] == '/' && !dest[1]))
        snprintf(out, cap, "/%s", name);
    else
        snprintf(out, cap, "%s/%s", dest, name);
}

static int ensure_parent_dirs(const char *path, char *why, size_t why_cap) {
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        struct stat st;
        if (lstat(buf, &st) == 0) {
            if (!S_ISDIR(st.st_mode)) {
                snprintf(why, why_cap, "parent is not a directory");
                return -1;
            }
        } else if (errno == ENOENT) {
            if (mkdir(buf, 0755) < 0) {
                snprintf(why, why_cap, "%s", strerror(errno));
                return -1;
            }
        } else {
            snprintf(why, why_cap, "%s", strerror(errno));
            return -1;
        }
        *p = '/';
    }
    return 0;
}

static int make_tempdir(char *out, size_t cap) {
    const char *base = ".pkgwork";
    for (int i = 0; i < 1000; i++) {
        snprintf(out, cap, "%s-%ld-%d", base, (long)getpid(), i);
        if (mkdir(out, 0700) == 0) return 0;
        if (errno != EEXIST) break;
    }
    fprintf(stderr, "pkg: cannot create temporary directory: %s\n", strerror(errno));
    return -1;
}

static int copy_file(const char *src, const char *dst, mode_t mode) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777 ? mode & 0777 : 0644);
    if (out < 0) { close(in); return -1; }
    char buf[4096];
    int rc = 0;
    for (;;) {
        ssize_t got = read(in, buf, sizeof buf);
        if (got < 0) { rc = -1; break; }
        if (got == 0) break;
        if (write_full(out, buf, (size_t)got) < 0) { rc = -1; break; }
    }
    close(in);
    close(out);
    return rc;
}

static int copy_tree(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) < 0) return -1;

    if (S_ISLNK(st.st_mode)) {
        char tgt[512];
        ssize_t n = readlink(src, tgt, sizeof tgt - 1);
        if (n < 0) return -1;
        tgt[n] = 0;
        return symlink(tgt, dst);
    }
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, st.st_mode & 0777 ? st.st_mode & 0777 : 0755) < 0 && errno != EEXIST)
            return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        int rc = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char csrc[1024], cdst[1024];
            snprintf(csrc, sizeof csrc, "%s/%s", src, de->d_name);
            snprintf(cdst, sizeof cdst, "%s/%s", dst, de->d_name);
            if (copy_tree(csrc, cdst) < 0) { rc = -1; break; }
        }
        closedir(d);
        return rc;
    }
    if (S_ISREG(st.st_mode)) return copy_file(src, dst, st.st_mode);
    errno = EINVAL;
    return -1;
}

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        int rc = 0;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char child[1024];
            snprintf(child, sizeof child, "%s/%s", path, de->d_name);
            if (remove_tree(child) < 0) rc = -1;
        }
        closedir(d);
        if (rmdir(path) < 0) rc = -1;
        return rc;
    }
    return unlink(path);
}

/* ---- list / extract ----------------------------------------------------- */

/* Decode the archive name field (prefix + name) into `out`. */
static void hdr_name(const struct tar_hdr *h, char *out, size_t cap) {
    if (h->prefix[0]) snprintf(out, cap, "%.155s/%.100s", h->prefix, h->name);
    else              snprintf(out, cap, "%.100s", h->name);
}

static int cmd_list(const char *archive) {
    int fd = open(archive, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "pkg: cannot open %s: %s\n", archive, strerror(errno)); return 1; }

    struct tar_hdr h;
    int zeros = 0;
    for (;;) {
        ssize_t r = read_full(fd, &h, BLK);
        if (r == 0) break;
        if (r != BLK) { fprintf(stderr, "pkg: truncated archive\n"); close(fd); return 1; }
        if (h.name[0] == '\0') { if (++zeros >= 2) break; continue; }
        zeros = 0;

        char name[300];
        hdr_name(&h, name, sizeof name);
        printf("%s\n", name);

        /* Skip the data blocks (only regular files carry any). */
        if (h.typeflag == '0' || h.typeflag == '\0') {
            unsigned long size = oct(h.size, 12);
            unsigned long blocks = (size + BLK - 1) / BLK;
            char skip[BLK];
            for (unsigned long b = 0; b < blocks; b++)
                if (read_full(fd, skip, BLK) != BLK) { fprintf(stderr, "pkg: truncated archive\n"); close(fd); return 1; }
        }
    }
    close(fd);
    return 0;
}

/* Fail an extraction with a message naming the offending entry. */
static int extract_fail(int fd, const char *name, const char *why) {
    fprintf(stderr, "pkg: failed to extract %s: %s\n", name, why);
    if (fd >= 0) close(fd);
    return 1;
}

static int cmd_extract(const char *archive, const char *dest) {
    int fd = open(archive, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "pkg: cannot open %s: %s\n", archive, strerror(errno)); return 1; }

    char base[256];
    const char *slash = strrchr(archive, '/');
    snprintf(base, sizeof base, "%s", slash ? slash + 1 : archive);
    printf("Extracting %s...\n", base);

    struct tar_hdr h;
    int zeros = 0;
    for (;;) {
        ssize_t r = read_full(fd, &h, BLK);
        if (r == 0) break;
        if (r != BLK) { fprintf(stderr, "pkg: truncated archive\n"); close(fd); return 1; }
        if (h.name[0] == '\0') { if (++zeros >= 2) break; continue; }
        zeros = 0;

        char name[300];
        hdr_name(&h, name, sizeof name);
        if (!path_is_safe(name)) return extract_fail(fd, name, "unsafe path");

        char full[1024];
        joinp(full, sizeof full, dest, name);
        unsigned long size = oct(h.size, 12);
        unsigned long mode = oct(h.mode, 8) & 0777;
        char why[256];
        if (ensure_parent_dirs(full, why, sizeof why) < 0)
            return extract_fail(fd, name, why);

        struct stat st;
        int exists = (lstat(full, &st) == 0);

        if (h.typeflag == '5') {                         /* directory */
            if (exists && !S_ISDIR(st.st_mode)) return extract_fail(fd, name, "exists, not a directory");
            if (!exists && mkdir(full, mode ? (mode_t)mode : 0755) < 0 && errno != EEXIST)
                return extract_fail(fd, name, strerror(errno));
            if (mode) chmod(full, (mode_t)mode);
            printf("Created %s\n", full);
        } else if (h.typeflag == '2') {                  /* symlink */
            char tgt[101]; snprintf(tgt, sizeof tgt, "%.100s", h.linkname);
            if (exists) {
                if (!S_ISLNK(st.st_mode)) return extract_fail(fd, name, "exists, not a symlink");
                unlink(full);
            }
            if (symlink(tgt, full) < 0) return extract_fail(fd, name, strerror(errno));
            printf("Created %s -> %s\n", full, tgt);
        } else if (h.typeflag == '0' || h.typeflag == '\0') { /* regular file */
            if (exists && S_ISDIR(st.st_mode)) return extract_fail(fd, name, "exists as a directory");
            if (exists && S_ISLNK(st.st_mode)) return extract_fail(fd, name, "exists as a symlink");
            int out = open(full, O_WRONLY | O_CREAT | O_TRUNC, mode ? (mode_t)mode : 0644);
            if (out < 0) return extract_fail(fd, name, strerror(errno));
            unsigned long remaining = size;
            char buf[BLK];
            while (remaining > 0) {
                if (read_full(fd, buf, BLK) != BLK) { close(out); return extract_fail(fd, name, "truncated data"); }
                unsigned long w = remaining < BLK ? remaining : BLK;
                if (write_full(out, buf, w) < 0) { close(out); return extract_fail(fd, name, "write failed"); }
                remaining -= w;
            }
            close(out);
            if (mode) chmod(full, (mode_t)mode);
            printf("Created %s\n", full);
            continue;                                    /* data already consumed */
        } else {
            return extract_fail(fd, name, "unsupported entry type");
        }

        /* Skip any trailing data blocks for non-regular entries (normally 0). */
        unsigned long blocks = (size + BLK - 1) / BLK;
        char skip[BLK];
        for (unsigned long b = 0; b < blocks; b++)
            if (read_full(fd, skip, BLK) != BLK) break;
    }
    close(fd);
    printf("Package extracted successfully.\n");
    return 0;
}

/* ---- manifest parsing --------------------------------------------------- */

#define MAXLIST 64
#define STRMAX  256

struct manifest {
    char name[STRMAX];
    char version[STRMAX];
    char build_cmd[512];
    char build_workdir[STRMAX];
    char stage_from[STRMAX];
    char include[MAXLIST][STRMAX];
    int  n_include;
    char extras[MAXLIST][STRMAX];
    int  n_extras;
};

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* Parse the tiny YAML subset used by package_info.yml. Returns 0 on success. */
static int parse_manifest(const char *path, struct manifest *m) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "pkg: cannot open manifest %s: %s\n", path, strerror(errno)); return -1; }

    memset(m, 0, sizeof *m);
    char line[1024];
    /* section: "", "build", "stage"; sublist: 0 none, 1 include, 2 extras */
    char section[32] = "";
    int sublist = 0;

    while (fgets(line, sizeof line, f)) {
        /* strip comments */
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        int indent = 0;
        while (line[indent] == ' ') indent++;
        char *body = trim(line);
        if (!*body) continue;

        if (body[0] == '-') {                            /* list item */
            char *item = trim(body + 1);
            if (!*item) continue;
            if (sublist == 1 && m->n_include < MAXLIST)
                snprintf(m->include[m->n_include++], STRMAX, "%s", item);
            else if (sublist == 2 && m->n_extras < MAXLIST)
                snprintf(m->extras[m->n_extras++], STRMAX, "%s", item);
            continue;
        }

        char *colon = strchr(body, ':');
        if (!colon) continue;
        *colon = 0;
        char *key = trim(body);
        char *val = trim(colon + 1);

        if (indent == 0) {                               /* top-level key */
            sublist = 0;
            if (!strcmp(key, "name"))         snprintf(m->name, STRMAX, "%s", val);
            else if (!strcmp(key, "version")) snprintf(m->version, STRMAX, "%s", val);
            else if (!strcmp(key, "extras"))  { section[0] = 0; sublist = 2; }
            else                              snprintf(section, sizeof section, "%s", key);
        } else {                                         /* nested key */
            if (!strcmp(section, "build")) {
                if (!strcmp(key, "command"))      snprintf(m->build_cmd, sizeof m->build_cmd, "%s", val);
                else if (!strcmp(key, "workdir")) snprintf(m->build_workdir, STRMAX, "%s", val);
            } else if (!strcmp(section, "stage")) {
                if (!strcmp(key, "from"))         snprintf(m->stage_from, STRMAX, "%s", val);
                else if (!strcmp(key, "include")) sublist = 1;
            }
        }
    }
    fclose(f);

    if (!m->name[0])    { fprintf(stderr, "pkg: manifest missing required key: name\n"); return -1; }
    if (!m->version[0]) { fprintf(stderr, "pkg: manifest missing required key: version\n"); return -1; }
    return 0;
}

/* ---- create ------------------------------------------------------------- */

/* Run the manifest build command inside `cwd`. Tokenizes on whitespace and
   execs directly (the VibeOS shell has no `-c`). A bare program name is looked
   up in /bin. Returns 0 on success (or if no command), -1 on failure. */
static int run_build(const char *cmd, const char *cwd) {
    if (!cmd || !*cmd) return 0;

    char buf[512];
    snprintf(buf, sizeof buf, "%s", cmd);
    char *argv[32];
    int argc = 0;
    char *p = buf;
    while (*p && argc < 31) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    argv[argc] = 0;
    if (argc == 0) return 0;

    char prog[STRMAX];
    if (strchr(argv[0], '/')) snprintf(prog, sizeof prog, "%s", argv[0]);
    else                      snprintf(prog, sizeof prog, "/bin/%s", argv[0]);

    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "pkg: fork failed: %s\n", strerror(errno)); return -1; }
    if (pid == 0) {
        if (cwd && *cwd && chdir(cwd) < 0) _exit(126);
        execve(prog, argv, environ);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "pkg: build command failed (status %d)\n", WEXITSTATUS(status));
        return -1;
    }
    return 0;
}

static int split_tar_name(const char *arcname, char *prefix, size_t prefix_cap,
                          char *name, size_t name_cap) {
    size_t len = strlen(arcname);
    if (len < name_cap) {
        snprintf(name, name_cap, "%s", arcname);
        prefix[0] = 0;
        return 0;
    }

    const char *slash = arcname + len;
    while (slash > arcname) {
        while (slash > arcname && slash[-1] != '/') slash--;
        if (slash == arcname) break;
        size_t plen = (size_t)((slash - 1) - arcname);
        size_t nlen = len - (size_t)(slash - arcname);
        if (plen < prefix_cap && nlen < name_cap) {
            memcpy(prefix, arcname, plen);
            prefix[plen] = 0;
            snprintf(name, name_cap, "%s", slash);
            return 0;
        }
        slash--;
    }

    fprintf(stderr, "pkg: archive path too long for ustar: %s\n", arcname);
    return -1;
}

/* Emit one ustar header block. */
static int write_header(int out, const char *arcname, char type,
                        unsigned long mode, unsigned long size, const char *linkname) {
    struct tar_hdr h;
    memset(&h, 0, sizeof h);
    if (split_tar_name(arcname, h.prefix, sizeof h.prefix, h.name, sizeof h.name) < 0)
        return -1;
    putoct(h.mode, 8, mode & 07777);
    putoct(h.uid, 8, 0);
    putoct(h.gid, 8, 0);
    putoct(h.size, 12, type == '0' ? size : 0);
    putoct(h.mtime, 12, 0);
    h.typeflag = type;
    if (linkname) snprintf(h.linkname, sizeof h.linkname, "%s", linkname);
    memcpy(h.magic, "ustar", 5);
    h.version[0] = '0'; h.version[1] = '0';
    snprintf(h.uname, sizeof h.uname, "root");
    snprintf(h.gname, sizeof h.gname, "root");

    /* checksum: sum of all bytes with the checksum field taken as spaces */
    memset(h.chksum, ' ', sizeof h.chksum);
    unsigned long sum = 0;
    const unsigned char *raw = (const unsigned char *)&h;
    for (size_t i = 0; i < sizeof h; i++) sum += raw[i];
    putoct(h.chksum, 7, sum);
    h.chksum[7] = ' ';

    return write_full(out, &h, BLK);
}

/* Append a regular file's data, zero-padded to a block boundary. */
static int write_file_data(int out, const char *src, unsigned long size) {
    int in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "pkg: cannot read %s: %s\n", src, strerror(errno)); return -1; }
    unsigned long remaining = size;
    char buf[BLK];
    int rc = 0;
    while (remaining > 0) {
        size_t want = remaining < BLK ? remaining : BLK;
        ssize_t got = read_full(in, buf, want);
        if (got != (ssize_t)want) { rc = -1; break; }
        if (want < BLK) memset(buf + want, 0, BLK - want);
        if (write_full(out, buf, BLK) < 0) { rc = -1; break; }
        remaining -= want;
    }
    close(in);
    return rc;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* A small set of archive paths, used to archive each path at most once.
   Returns 1 if `p` was already present (caller should skip), 0 if newly added. */
struct strset { char items[3 * MAXLIST + 1][STRMAX]; int n; };
static int strset_add(struct strset *s, const char *p) {
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->items[i], p)) return 1;
    if (s->n < (int)(sizeof s->items / sizeof s->items[0]))
        snprintf(s->items[s->n++], STRMAX, "%s", p);
    return 0;
}

/* Recursively archive `src` under archive-relative path `arcname`.
   `staging` controls the per-file log verb ("Staging" vs "Adding"). */
static int add_path(int out, const char *src, const char *arcname, int staging) {
    if (!path_is_safe(arcname)) {
        fprintf(stderr, "pkg: refusing unsafe archive path %s\n", arcname);
        return -1;
    }
    struct stat st;
    if (lstat(src, &st) < 0) {
        fprintf(stderr, "pkg: %s: %s\n", src, strerror(errno));
        return -1;
    }

    if (S_ISLNK(st.st_mode)) {
        char tgt[256];
        ssize_t n = readlink(src, tgt, sizeof tgt - 1);
        if (n < 0) { fprintf(stderr, "pkg: readlink %s: %s\n", src, strerror(errno)); return -1; }
        tgt[n] = 0;
        printf("%s %s\n", staging ? "Staging" : "Adding", arcname);
        return write_header(out, arcname, '2', (unsigned long)(st.st_mode & 0777), 0, tgt);
    }

    if (S_ISDIR(st.st_mode)) {
        char dirname[STRMAX + 2];
        snprintf(dirname, sizeof dirname, "%s/", arcname);
        if (write_header(out, dirname, '5', (unsigned long)(st.st_mode & 0777), 0, NULL) < 0) return -1;

        DIR *d = opendir(src);
        if (!d) { fprintf(stderr, "pkg: opendir %s: %s\n", src, strerror(errno)); return -1; }
        static char names[512][STRMAX];   /* file-scope-ish; create is single-threaded */
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d)) && n < (int)(sizeof names / sizeof names[0])) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(names[n++], STRMAX, "%s", de->d_name);
        }
        closedir(d);
        char *ptrs[512];
        for (int i = 0; i < n; i++) ptrs[i] = names[i];
        qsort(ptrs, n, sizeof ptrs[0], cmp_str);

        for (int i = 0; i < n; i++) {
            char csrc[1024], carc[1024];
            snprintf(csrc, sizeof csrc, "%s/%s", src, ptrs[i]);
            snprintf(carc, sizeof carc, "%s/%s", arcname, ptrs[i]);
            if (add_path(out, csrc, carc, staging) < 0) return -1;
        }
        return 0;
    }

    if (S_ISREG(st.st_mode)) {
        printf("%s %s\n", staging ? "Staging" : "Adding", arcname);
        if (write_header(out, arcname, '0', (unsigned long)(st.st_mode & 0777), (unsigned long)st.st_size, NULL) < 0) return -1;
        return write_file_data(out, src, (unsigned long)st.st_size);
    }

    fprintf(stderr, "pkg: %s: unsupported file type\n", src);
    return -1;
}

static int cmd_create(const char *pkgdir, const char *out_opt,
                      char cli_inc[][STRMAX], int n_cli_inc) {
    struct manifest m;
    char manifest_path[1024];
    snprintf(manifest_path, sizeof manifest_path, "%s/package_info.yml", pkgdir);
    if (parse_manifest(manifest_path, &m) < 0) return 1;

    if (m.build_workdir[0] && !path_is_current_dir(m.build_workdir) && !path_is_safe(m.build_workdir)) {
        fprintf(stderr, "pkg: manifest build.workdir is unsafe\n");
        return 1;
    }
    if (m.stage_from[0] && !path_is_safe(m.stage_from)) {
        fprintf(stderr, "pkg: manifest stage.from is unsafe\n");
        return 1;
    }
    for (int i = 0; i < m.n_include; i++) {
        if (!path_is_safe(m.include[i])) {
            fprintf(stderr, "pkg: manifest stage.include path is unsafe: %s\n", m.include[i]);
            return 1;
        }
    }
    for (int i = 0; i < m.n_extras; i++) {
        if (!path_is_safe(m.extras[i])) {
            fprintf(stderr, "pkg: manifest extras path is unsafe: %s\n", m.extras[i]);
            return 1;
        }
    }
    for (int i = 0; i < n_cli_inc; i++) {
        if (!path_is_safe(cli_inc[i])) {
            fprintf(stderr, "pkg: include path is unsafe: %s\n", cli_inc[i]);
            return 1;
        }
    }

    char output[1024];
    if (out_opt) snprintf(output, sizeof output, "%s", out_opt);
    else         snprintf(output, sizeof output, "%s-%s.pkg", m.name, m.version);

    printf("Building %s-%s...\n", m.name, m.version);

    char tempdir[1024];
    char workroot[1024];
    if (make_tempdir(tempdir, sizeof tempdir) < 0) return 1;
    if (path_join_checked(workroot, sizeof workroot, tempdir, "src") < 0) {
        remove_tree(tempdir);
        return 1;
    }
    if (copy_tree(pkgdir, workroot) < 0) {
        fprintf(stderr, "pkg: failed to copy package source tree: %s\n", strerror(errno));
        remove_tree(tempdir);
        return 1;
    }

    if (path_join_checked(manifest_path, sizeof manifest_path, workroot, "package_info.yml") < 0) {
        remove_tree(tempdir);
        return 1;
    }

    /* Run the build command inside the copied work tree. */
    char workdir[1024];
    if (path_join_checked(workdir, sizeof workdir, workroot,
                          (m.build_workdir[0] && !path_is_current_dir(m.build_workdir)) ? m.build_workdir : "") < 0) {
        remove_tree(tempdir);
        return 1;
    }
    if (run_build(m.build_cmd, workdir) < 0) {
        remove_tree(tempdir);
        return 1;
    }

    int out = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        fprintf(stderr, "pkg: cannot write %s: %s\n", output, strerror(errno));
        remove_tree(tempdir);
        return 1;
    }

    /* Track archive paths already written so the same path requested via both
       the manifest `extras` and a CLI `--include` is archived only once. */
    static struct strset seen;
    seen.n = 0;

    /* package_info.yml is always archived first, at the root. */
    strset_add(&seen, "package_info.yml");
    if (add_path(out, manifest_path, "package_info.yml", 0) < 0) goto fail;

    /* Staged build outputs, resolved against <pkgdir>/<stage.from>. */
    for (int i = 0; i < m.n_include; i++) {
        if (strset_add(&seen, m.include[i])) continue;
        char src[1024];
        if (m.stage_from[0]) {
            if (path_join3_checked(src, sizeof src, workroot, m.stage_from, m.include[i]) < 0) goto fail;
        } else {
            if (path_join_checked(src, sizeof src, workroot, m.include[i]) < 0) goto fail;
        }
        if (add_path(out, src, m.include[i], 1) < 0) goto fail;
    }

    /* Extra files: manifest `extras` plus CLI `--include`, taken straight from
       the package source directory and kept at their given relative path. */
    for (int i = 0; i < m.n_extras; i++) {
        if (strset_add(&seen, m.extras[i])) continue;
        char src[1024];
        if (path_join_checked(src, sizeof src, workroot, m.extras[i]) < 0) goto fail;
        if (add_path(out, src, m.extras[i], 0) < 0) goto fail;
    }
    for (int i = 0; i < n_cli_inc; i++) {
        if (strset_add(&seen, cli_inc[i])) continue;
        char src[1024];
        if (path_join_checked(src, sizeof src, workroot, cli_inc[i]) < 0) goto fail;
        if (add_path(out, src, cli_inc[i], 0) < 0) goto fail;
    }

    /* Two zero blocks terminate a ustar archive. */
    {
        char zero[BLK];
        memset(zero, 0, sizeof zero);
        if (write_full(out, zero, BLK) < 0 || write_full(out, zero, BLK) < 0) goto fail;
    }
    close(out);
    remove_tree(tempdir);
    printf("Wrote %s\n", output);
    return 0;

fail:
    close(out);
    unlink(output);
    remove_tree(tempdir);
    return 1;
}

/* ---- entry point -------------------------------------------------------- */

static int usage(void) {
    fprintf(stderr,
        "usage:\n"
        "  pkg list    <archive.pkg>\n"
        "  pkg extract <archive.pkg> <destdir>\n"
        "  pkg create  <pkgdir> [--output <archive.pkg>] [--include <path>]...\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 3) return usage();
    const char *cmd = argv[1];

    if (!strcmp(cmd, "list")) {
        if (argc != 3) return usage();
        return cmd_list(argv[2]);
    }
    if (!strcmp(cmd, "extract")) {
        if (argc != 4) return usage();
        return cmd_extract(argv[2], argv[3]);
    }
    if (!strcmp(cmd, "create")) {
        const char *pkgdir = argv[2];
        const char *out_opt = NULL;
        static char cli_inc[MAXLIST][STRMAX];
        int n_cli_inc = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--output") && i + 1 < argc) {
                out_opt = argv[++i];
            } else if (!strcmp(argv[i], "--include") && i + 1 < argc) {
                if (n_cli_inc < MAXLIST) snprintf(cli_inc[n_cli_inc++], STRMAX, "%s", argv[++i]);
            } else {
                fprintf(stderr, "pkg: unknown argument: %s\n", argv[i]);
                return usage();
            }
        }
        return cmd_create(pkgdir, out_opt, cli_inc, n_cli_inc);
    }

    return usage();
}
