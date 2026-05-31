/*
 * sinit — VibeOS service-managed init (ROADMAP: service-managed init).
 *
 * PID 1. Reads one small YAML file per service from /config/services/, starts
 * them in dependency order, and supervises them: respawn services are restarted
 * when they exit, oneshot services run once. Deliberately NOT SysV (no
 * runlevels / rc scripts) and NOT systemd (no unit sections / dbus) — services
 * are just discoverable files you can `ls` and `cat`.
 *
 * Service file (/config/services/<name>.yaml), a flat key: value subset:
 *
 *     description: interactive serial shell
 *     exec:        /bin/sh
 *     respawn:     yes          # restart when it exits (default no)
 *     oneshot:     no           # run once to completion, don't supervise
 *     enabled:     yes          # start it at all (default yes)
 *     after:       welcome net  # start only after these services
 *
 * Built static with the x86_64-vibeos-musl cross compiler and installed as
 * /bin/init.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#define SVC_DIR    "/config/services"
#define MAX_SVC    32
#define MAX_DEPS   8
#define MAX_ARGV   16

typedef struct {
    char name[32];
    char desc[64];
    char exec[160];
    char deps[MAX_DEPS][32];
    int  ndeps;
    int  respawn, oneshot, enabled;
    pid_t pid;          /* >0 while running */
    int  started;       /* has been started at least once */
    int  done;          /* oneshot finished */
} svc_t;

static svc_t g_svc[MAX_SVC];
static int   g_nsvc;

/* --- logging --- */
static void logmsg(const char *msg, const char *arg) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    char line[256];
    int n = snprintf(line, sizeof line, "[init +%lds] %s%s%s\n",
                     (long)ts.tv_sec, msg, arg ? " " : "", arg ? arg : "");
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
    /* name = fname without a trailing ".yaml"/".yml"/".conf" */
    if (g_nsvc >= MAX_SVC) return;
    svc_t *s = &g_svc[g_nsvc];
    memset(s, 0, sizeof *s);
    s->enabled = 1;
    snprintf(s->name, sizeof s->name, "%s", fname);
    char *dot = strrchr(s->name, '.');
    if (dot && (!strcmp(dot, ".yaml") || !strcmp(dot, ".yml") || !strcmp(dot, ".conf"))) *dot = 0;

    char path[256];
    snprintf(path, sizeof path, "%s/%s", dir, fname);
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
        else if (!strcmp(k, "respawn"))     s->respawn = truthy(v);
        else if (!strcmp(k, "oneshot"))     s->oneshot = truthy(v);
        else if (!strcmp(k, "enabled"))     s->enabled = truthy(v);
        else if (!strcmp(k, "after")) {
            char *tok = strtok(v, " ,\t");
            while (tok && s->ndeps < MAX_DEPS) { snprintf(s->deps[s->ndeps++], 32, "%s", tok); tok = strtok(NULL, " ,\t"); }
        }
    }
    fclose(f);
    if (s->exec[0]) g_nsvc++;          /* a service must have an exec */
}

static int load_services(void) {
    DIR *d = opendir(SVC_DIR);
    if (!d) { logmsg("no", SVC_DIR " — nothing to supervise"); return 0; }
    /* collect + sort filenames for a stable default order */
    char names[MAX_SVC][64]; int nn = 0;
    struct dirent *de;
    while ((de = readdir(d)) && nn < MAX_SVC) {
        if (de->d_name[0] == '.') continue;
        snprintf(names[nn++], 64, "%s", de->d_name);
    }
    closedir(d);
    for (int i = 0; i < nn; i++)              /* simple insertion sort */
        for (int j = i + 1; j < nn; j++)
            if (strcmp(names[j], names[i]) < 0) { char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t); }
    for (int i = 0; i < nn; i++) load_one(SVC_DIR, names[i]);
    return g_nsvc;
}

static svc_t *find(const char *name) {
    for (int i = 0; i < g_nsvc; i++) if (!strcmp(g_svc[i].name, name)) return &g_svc[i];
    return NULL;
}

/* A dependency is satisfied once it has started (respawn) or completed (oneshot). */
static int deps_ready(svc_t *s) {
    for (int i = 0; i < s->ndeps; i++) {
        svc_t *d = find(s->deps[i]);
        if (!d || !d->enabled) continue;       /* unknown/disabled dep: ignore */
        if (d->oneshot ? !d->done : !d->started) return 0;
    }
    return 1;
}

static void start(svc_t *s) {
    /* split exec into argv */
    char buf[160]; snprintf(buf, sizeof buf, "%s", s->exec);
    char *argv[MAX_ARGV]; int argc = 0;
    char *tok = strtok(buf, " \t");
    while (tok && argc < MAX_ARGV - 1) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
    argv[argc] = NULL;
    if (argc == 0) return;

    char *envp[] = { (char *)"PATH=/bin", (char *)"HOME=/", (char *)"TERM=vibeos", NULL };
    pid_t pid = fork();
    if (pid == 0) { execve(argv[0], argv, envp); _exit(127); }
    if (pid < 0) { logmsg("fork failed for", s->name); return; }
    s->pid = pid; s->started = 1;
    logmsg(s->oneshot ? "running oneshot" : "started", s->name);

    if (s->oneshot) {                          /* run to completion before dependents */
        int st; waitpid(pid, &st, 0);
        s->pid = 0; s->done = 1;
        logmsg("oneshot complete:", s->name);
    }
}

/* Start every enabled service in dependency order. */
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
        if (!progress) {                       /* dependency cycle / unmet: start the rest */
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

    /* Supervise: reap children; respawn the respawn services. */
    for (;;) {
        int st;
        pid_t pid = wait(&st);
        if (pid < 0) { sleep(1); continue; }   /* no children right now */
        svc_t *s = NULL;
        for (int i = 0; i < g_nsvc; i++) if (g_svc[i].pid == pid) { s = &g_svc[i]; break; }
        if (!s) continue;                      /* a reparented orphan */
        s->pid = 0;
        if (s->respawn && s->enabled) {
            logmsg("exited, respawning:", s->name);
            start(s);
        } else {
            logmsg("exited:", s->name);
            s->done = 1;
        }
    }
    return 0;
}
