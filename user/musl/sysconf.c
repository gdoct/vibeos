/*
 * sysconf — query and edit the VibeOS /config service (ROADMAP: config service).
 *
 *   sysconf                 list all live settings
 *   sysconf get <key>       print one value
 *   sysconf reload          re-read /config/system.conf into the kernel
 *   sysconf set <key> <val> edit /config/system.conf and reload
 *
 * Reads live values through the sysconfig(1000) syscall; `set` rewrites the
 * file and then reloads, so the change takes effect immediately (e.g. uname's
 * hostname). Built with the x86_64-vibeos-musl cross compiler.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#define CONFIG_PATH "/config/system.conf"
enum { CFG_RELOAD = 0, CFG_GET = 1, CFG_COUNT = 2, CFG_ENTRY = 3 };

static long sysconfig(int op, unsigned long a, void *buf, unsigned long len) {
    return syscall(1000, (long)op, (long)a, buf, (long)len);
}

/* Rewrite CONFIG_PATH so `key`'s value becomes `val` (replacing the existing
   line, or appending one). Returns 0 on success. */
static int set_key(const char *key, const char *val) {
    static char in[8192], out[8192];
    int fd = open(CONFIG_PATH, O_RDONLY);
    int n = 0;
    if (fd >= 0) { n = read(fd, in, sizeof in - 1); if (n < 0) n = 0; close(fd); }
    in[n] = '\0';

    size_t klen = strlen(key);
    int o = 0, replaced = 0;
    char *line = in;
    while (*line || line < in + n) {
        char *nl = strchr(line, '\n');
        int linelen = nl ? (int)(nl - line) : (int)strlen(line);
        /* does this line define `key`? (optional leading spaces, then key, then ':') */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        int match = (strncmp(p, key, klen) == 0);
        if (match) { const char *q = p + klen; while (*q == ' ' || *q == '\t') q++; match = (*q == ':'); }
        if (match && !replaced) {
            o += snprintf(out + o, sizeof out - o, "%s: %s\n", key, val);
            replaced = 1;
        } else if (linelen > 0 || nl) {
            memcpy(out + o, line, linelen); o += linelen;
            out[o++] = '\n';
        }
        if (!nl) break;
        line = nl + 1;
    }
    if (!replaced) o += snprintf(out + o, sizeof out - o, "%s: %s\n", key, val);

    fd = open(CONFIG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { fprintf(stderr, "sysconf: cannot write %s\n", CONFIG_PATH); return 1; }
    write(fd, out, o);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    char buf[256];
    if (argc < 2 || strcmp(argv[1], "list") == 0) {
        int count = (int)sysconfig(CFG_COUNT, 0, 0, 0);
        for (int i = 0; i < count; i++) {
            long m = sysconfig(CFG_ENTRY, (unsigned long)i, buf, sizeof buf - 1);
            if (m >= 0) { buf[m] = 0; printf("%s\n", buf); }
        }
        return 0;
    }
    if (strcmp(argv[1], "get") == 0 && argc >= 3) {
        long m = sysconfig(CFG_GET, (unsigned long)argv[2], buf, sizeof buf - 1);
        if (m < 0) { printf("sysconf: no such key: %s\n", argv[2]); return 1; }
        buf[m] = 0; printf("%s\n", buf);
        return 0;
    }
    if (strcmp(argv[1], "reload") == 0) {
        long n = sysconfig(CFG_RELOAD, 0, 0, 0);
        printf("sysconf: reloaded %ld setting(s)\n", n);
        return 0;
    }
    if (strcmp(argv[1], "set") == 0 && argc >= 4) {
        if (set_key(argv[2], argv[3]) != 0) return 1;
        long n = sysconfig(CFG_RELOAD, 0, 0, 0);
        printf("sysconf: set %s=%s, reloaded %ld setting(s)\n", argv[2], argv[3], n);
        return 0;
    }
    fprintf(stderr, "usage: sysconf [list | get <key> | reload | set <key> <val>]\n");
    return 2;
}
