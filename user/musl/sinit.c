/*
 * sinit — VibeOS service-managed init (ROADMAP: service-managed init).
 *
 * PID 1. Reads one small YAML file per service from /config/services/, starts
 * them in dependency order, and supervises them with restart back-off. NOT SysV
 * (no runlevels / rc scripts) and NOT systemd (no unit sections / dbus) —
 * services are just discoverable files you can `ls` and `cat`.
 *
 * Service file (/config/services/<name>.yaml), a flat key: value subset:
 *
 *     description: interactive serial shell
 *     exec:        /bin/sh
 *     respawn:     yes              # restart when it exits (default no)
 *     oneshot:     no               # run once to completion, don't supervise
 *     enabled:     yes              # start it at all (default yes)
 *     after:       welcome net      # start only after these services
 *     log:         /config/logs/x.log   # redirect stdout/stderr here (else console)
 *
 * Supervision: a respawn service that exits is restarted; if it keeps exiting
 * within FAIL_WINDOW seconds, the restart is delayed (back-off) and after
 * MAX_FAILS rapid failures init gives up on it. Live state is written to
 * /config/services.state (see `sysconf services`).
 *
 * Built static with the x86_64-vibeos-musl cross compiler, installed as /bin/init.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#define SVC_DIR     "/config/services"
#define STATE_FILE  "/config/services.state"
#define MAX_SVC     32
#define MAX_DEPS    8
#define MAX_ARGV    16
#define FAIL_WINDOW 3        /* exit within N s of start counts as a rapid failure */
#define MAX_FAILS   5        /* give up after this many consecutive rapid failures */

typedef struct {
    char name[32];
    char desc[64];
    char exec[160];
    char logpath[128];
    char deps[MAX_DEPS][32];
    int  ndeps;
    int  respawn, oneshot, enabled;
    pid_t pid;          /* >0 while running */
    int  started;       /* has been started at least once */
    int  done;          /* oneshot finished / non-respawn exited */
    int  failed;        /* gave up after too many rapid failures */
    int  restarts;      /* total (re)starts beyond the first */
    int  fast_fails;    /* consecutive rapid failures */
    long last_start;    /* monotonic seconds of the last start */
    long next_start;    /* earliest monotonic second to (re)start (back-off) */
} svc_t;

static svc_t g_svc[MAX_SVC];
static int   g_nsvc;

static int copy_cstr(char *dst, size_t dst_size, const char *src) {
    size_t len;
    if (!dst_size) return -1;
    len = strlen(src);
    if (len >= dst_size) return -1;
    memcpy(dst, src, len + 1);
    return 0;
}

static long mono(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long)ts.tv_sec; }

static void logmsg(const char *msg, const char *arg) {
    char line[256];
    int n = snprintf(line, sizeof line, "[init +%lds] %s%s%s\n",
                     mono(), msg, arg ? " " : "", arg ? arg : "");
    write(1, line, n);
}

/* --- tiny YAML-subset parse (key: value) --- */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
    *e = 0;
    if (e - s >= 2 && ((s[0] == '"' && e[-1] == '"') || (s[0] == '\'' && e[-1] == '\''))) { s++; e[-1] = 0; }
    return s;
}
static int truthy(const char *v) {
    return !strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "1");
}

static void load_one(const char *dir, const char *fname) {
    if (g_nsvc >= MAX_SVC) return;
    svc_t *s = &g_svc[g_nsvc];
    memset(s, 0, sizeof *s);
    s->enabled = 1;
    if (copy_cstr(s->name, sizeof s->name, fname) != 0) return;
    char *dot = strrchr(s->name, '.');
    if (dot && (!strcmp(dot, ".yaml") || !strcmp(dot, ".yml") || !strcmp(dot, ".conf"))) *dot = 0;

    char path[300];
    int path_len = snprintf(path, sizeof path, "%s/%s", dir, fname);
    if (path_len < 0 || path_len >= (int)sizeof path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *l = trim(line);
        if (!l[0] || l[0] == '#') continue;
        char *colon = strchr(l, ':');
        if (!colon) continue;
        *colon = 0;
        char *k = trim(l), *v = trim(colon + 1);
        if (!strcmp(k, "exec"))             snprintf(s->exec, sizeof s->exec, "%s", v);
        else if (!strcmp(k, "description")) snprintf(s->desc, sizeof s->desc, "%s", v);
        else if (!strcmp(k, "log"))         snprintf(s->logpath, sizeof s->logpath, "%s", v);
        else if (!strcmp(k, "respawn"))     s->respawn = truthy(v);
        else if (!strcmp(k, "oneshot"))     s->oneshot = truthy(v);
        else if (!strcmp(k, "enabled"))     s->enabled = truthy(v);
        else if (!strcmp(k, "after")) {
            char *tok = strtok(v, " ,\t");
            while (tok && s->ndeps < MAX_DEPS) { snprintf(s->deps[s->ndeps++], 32, "%s", tok); tok = strtok(NULL, " ,\t"); }
        }
    }
    fclose(f);
    if (s->exec[0]) g_nsvc++;
}

static int load_services(void) {
    DIR *d = opendir(SVC_DIR);
    if (!d) { logmsg("no", SVC_DIR " — nothing to supervise"); return 0; }
    char names[MAX_SVC][64]; int nn = 0;
    struct dirent *de;
    while ((de = readdir(d)) && nn < MAX_SVC) {
        if (de->d_name[0] == '.') continue;
        if (copy_cstr(names[nn], sizeof names[nn], de->d_name) == 0) nn++;
    }
    closedir(d);
    for (int i = 0; i < nn; i++)
        for (int j = i + 1; j < nn; j++)
            if (strcmp(names[j], names[i]) < 0) { char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t); }
    for (int i = 0; i < nn; i++) load_one(SVC_DIR, names[i]);
    return g_nsvc;
}

static svc_t *find(const char *name) {
    for (int i = 0; i < g_nsvc; i++) if (!strcmp(g_svc[i].name, name)) return &g_svc[i];
    return NULL;
}

/* Write a human-readable snapshot of live service state (discoverable / read by
   `sysconf services`). Best-effort. */
static void write_state(void) {
    int fd = open(STATE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char buf[2048]; int o = 0;
    o += snprintf(buf + o, sizeof buf - o, "# VibeOS live service state (written by init)\n");
    for (int i = 0; i < g_nsvc && o < (int)sizeof buf - 128; i++) {
        svc_t *s = &g_svc[i];
        const char *st = s->failed ? "failed"
                       : s->pid    ? "running"
                       : s->oneshot && s->done ? "done"
                       : s->done   ? "stopped"
                       : s->started ? "restarting" : "pending";
        o += snprintf(buf + o, sizeof buf - o, "%-12s %-10s pid=%-5d restarts=%d%s%s\n",
                      s->name, st, s->pid, s->restarts,
                      s->logpath[0] ? " log=" : "", s->logpath[0] ? s->logpath : "");
    }
    write(fd, buf, o);
    close(fd);
}

static void start(svc_t *s) {
    char buf[160]; snprintf(buf, sizeof buf, "%s", s->exec);
    char *argv[MAX_ARGV]; int argc = 0;
    char *tok = strtok(buf, " \t");
    while (tok && argc < MAX_ARGV - 1) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
    argv[argc] = NULL;
    if (argc == 0) return;

    char *envp[] = {
        (char *)"PATH=/bin", (char *)"HOME=/", (char *)"TERM=vibeos",
        (char *)"ENV=/etc/mkshrc",               /* mksh sources this when interactive */
        NULL
    };
    pid_t pid = fork();
    if (pid == 0) {
        if (s->logpath[0]) {                   /* redirect stdout+stderr to the log */
            int lf = open(s->logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (lf >= 0) { dup2(lf, 1); dup2(lf, 2); if (lf > 2) close(lf); }
        }
        execve(argv[0], argv, envp);
        _exit(127);
    }
    if (pid < 0) { logmsg("fork failed for", s->name); return; }
    s->pid = pid; s->last_start = mono();
    if (s->started) s->restarts++;
    s->started = 1;
    logmsg(s->oneshot ? "running oneshot" : "started", s->name);

    if (s->oneshot) {
        int st; waitpid(pid, &st, 0);
        s->pid = 0; s->done = 1;
        logmsg("oneshot complete:", s->name);
    }
}

static int deps_ready(svc_t *s) {
    for (int i = 0; i < s->ndeps; i++) {
        svc_t *d = find(s->deps[i]);
        if (!d || !d->enabled) continue;
        if (d->oneshot ? !d->done : !d->started) return 0;
    }
    return 1;
}

static void start_all(void) {
    for (int pass = 0; pass < g_nsvc + 1; pass++) {
        int progress = 0, pending = 0;
        for (int i = 0; i < g_nsvc; i++) {
            svc_t *s = &g_svc[i];
            if (!s->enabled || s->started) continue;
            if (deps_ready(s)) { start(s); progress = 1; }
            else pending = 1;
        }
        if (!pending) return;
        if (!progress) {
            for (int i = 0; i < g_nsvc; i++)
                if (g_svc[i].enabled && !g_svc[i].started) { logmsg("unmet deps, starting anyway:", g_svc[i].name); start(&g_svc[i]); }
            return;
        }
    }
}

int main(void) {
    logmsg("VibeOS service-managed init (PID 1)", NULL);
    int n = load_services();
    char cnt[16]; snprintf(cnt, sizeof cnt, "%d", n);
    logmsg("loaded service(s):", cnt);
    start_all();
    write_state();

    /* Supervisor poll loop: reap exits without blocking, apply restart back-off,
       and (re)start due respawn services. */
    for (;;) {
        int st; pid_t pid;
        int changed = 0;
        while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
            svc_t *s = NULL;
            for (int i = 0; i < g_nsvc; i++) if (g_svc[i].pid == pid) { s = &g_svc[i]; break; }
            if (!s) continue;                  /* reparented orphan */
            s->pid = 0; changed = 1;
            long now = mono();
            if (!s->respawn || !s->enabled) { s->done = 1; logmsg("exited:", s->name); continue; }
            if (now - s->last_start < FAIL_WINDOW) s->fast_fails++; else s->fast_fails = 0;
            if (s->fast_fails >= MAX_FAILS) {
                s->failed = 1;
                logmsg("FAILED (too many rapid restarts), giving up:", s->name);
                continue;
            }
            int backoff = s->fast_fails;       /* 0,1,2,3,4 s as failures accrue */
            s->next_start = now + backoff;
            if (backoff) { char m[64]; snprintf(m, sizeof m, "%s in %ds", s->name, backoff); logmsg("exited, respawning", m); }
            else logmsg("exited, respawning:", s->name);
        }

        long now = mono();
        for (int i = 0; i < g_nsvc; i++) {
            svc_t *s = &g_svc[i];
            if (s->enabled && s->respawn && !s->failed && s->pid == 0 && s->started && now >= s->next_start) {
                start(s); changed = 1;
            }
        }
        if (changed) write_state();
        usleep(200000);                        /* 200 ms */
    }
    return 0;
}
