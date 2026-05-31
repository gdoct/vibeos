#include "kernel.h"
#include "config.h"
#include "fs.h"

/*
 * System configuration service. See config.h.
 *
 * A fixed-size key/value store (like the task/file pools): exhaustion just drops
 * extra keys with a log line. The parser is line-oriented and forgiving — it is
 * meant for hand-written /config files, not adversarial input.
 */

#define CONFIG_MAX      64
#define CONFIG_KEYLEN   64
#define CONFIG_VALLEN   192
#define CONFIG_FILEMAX  8192

typedef struct { char key[CONFIG_KEYLEN]; char val[CONFIG_VALLEN]; } centry_t;
static centry_t g_cfg[CONFIG_MAX];
static int      g_cfg_n;

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Trim leading/trailing whitespace and one layer of matching quotes, in place
   via start/end pointers. Returns the trimmed start; *end_out is one past end. */
static char *trim(char *s, char **end_out) {
    while (*s && is_space(*s)) s++;
    char *e = s + kstrlen(s);
    while (e > s && (is_space(e[-1]) || e[-1] == '\n')) e--;
    if (e - s >= 2 && ((s[0] == '"' && e[-1] == '"') || (s[0] == '\'' && e[-1] == '\''))) {
        s++; e--;
    }
    *end_out = e;
    return s;
}

static void store(const char *key, const char *val) {
    for (int i = 0; i < g_cfg_n; i++)            /* update an existing key */
        if (streq(g_cfg[i].key, key)) {
            unsigned j = 0; for (; val[j] && j < CONFIG_VALLEN - 1; j++) g_cfg[i].val[j] = val[j];
            g_cfg[i].val[j] = '\0';
            return;
        }
    if (g_cfg_n >= CONFIG_MAX) { kprintf("[config] too many keys; dropping '%s'\n", key); return; }
    centry_t *e = &g_cfg[g_cfg_n++];
    unsigned j = 0; for (; key[j] && j < CONFIG_KEYLEN - 1; j++) e->key[j] = key[j]; e->key[j] = '\0';
    j = 0; for (; val[j] && j < CONFIG_VALLEN - 1; j++) e->val[j] = val[j]; e->val[j] = '\0';
}

/* Parse a NUL-terminated config buffer into the store (clears it first). */
static void parse(char *buf) {
    g_cfg_n = 0;
    char *p = buf;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        char saved = *p;
        *p = '\0';

        char *lend;
        char *l = trim(line, &lend);
        *lend = '\0';
        if (l[0] && l[0] != '#') {                /* skip blanks + comments */
            char *colon = l;
            while (*colon && *colon != ':') colon++;
            if (*colon == ':') {
                *colon = '\0';
                char *kend, *vend;
                char *k = trim(l, &kend); *kend = '\0';
                char *v = trim(colon + 1, &vend); *vend = '\0';
                if (k[0]) store(k, v);
            }
        }

        if (saved == '\0') break;
        p++;                                      /* past the newline */
    }
}

int config_reload(void) {
    int ino = fs_resolve(CONFIG_PATH);
    if (ino < 0) { kprintf("[config] %s not found; using defaults\n", CONFIG_PATH); g_cfg_n = 0; return -1; }
    fs_stat_t st;
    if (fs_istat((uint32_t)ino, &st) != FS_OK) { g_cfg_n = 0; return -1; }
    uint32_t n = st.size < CONFIG_FILEMAX - 1 ? (uint32_t)st.size : CONFIG_FILEMAX - 1;
    static char buf[CONFIG_FILEMAX];
    int r = fs_pread((uint32_t)ino, 0, buf, n);
    if (r < 0) { g_cfg_n = 0; return -1; }
    buf[r] = '\0';
    parse(buf);
    return g_cfg_n;
}

void config_init(void) {
    int n = config_reload();
    if (n >= 0)
        kprintf("[config] loaded %s: %d setting(s) (hostname=%s)\n",
                CONFIG_PATH, n, config_get_def("hostname", "vibeos"));
}

const char *config_get(const char *key) {
    for (int i = 0; i < g_cfg_n; i++)
        if (streq(g_cfg[i].key, key)) return g_cfg[i].val;
    return nullptr;
}
const char *config_get_def(const char *key, const char *def) {
    const char *v = config_get(key);
    return v ? v : def;
}
int config_get_int(const char *key, int def) {
    const char *v = config_get(key);
    if (!v || !*v) return def;
    int sign = 1; if (*v == '-') { sign = -1; v++; }
    int n = 0; int any = 0;
    for (; *v >= '0' && *v <= '9'; v++) { n = n * 10 + (*v - '0'); any = 1; }
    return any ? sign * n : def;
}
int config_get_bool(const char *key, int def) {
    const char *v = config_get(key);
    if (!v) return def;
    if (streq(v, "true") || streq(v, "yes") || streq(v, "on") || streq(v, "1")) return 1;
    if (streq(v, "false") || streq(v, "no") || streq(v, "off") || streq(v, "0")) return 0;
    return def;
}

int config_count(void) { return g_cfg_n; }
int config_entry(int i, const char **key, const char **val) {
    if (i < 0 || i >= g_cfg_n) return 0;
    if (key) *key = g_cfg[i].key;
    if (val) *val = g_cfg[i].val;
    return 1;
}
