/* su_pgrep.c -- pgrep and pkill; included by main.c. */


static int name_of_pid(const char *pid, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%s/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int got = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Name:", 5)) continue;
        const char *q = line + 5;
        while (*q == ' ' || *q == 9) q++;
        size_t n = 0;
        while (q[n] && q[n] != 10 && q[n] != 13 && n + 1 < cap) { out[n] = q[n]; n++; }
        out[n] = 0;
        got = 0;
        break;
    }
    fclose(f);
    return got;
}

static int sig_from_name(const char *s) {
    if (*s == '-') s++;
    if (*s >= '0' && *s <= '9') return atoi(s);
    if (!strncmp(s, "SIG", 3)) s += 3;
    if (!strcmp(s, "HUP"))  return SIGHUP;
    if (!strcmp(s, "INT"))  return SIGINT;
    if (!strcmp(s, "QUIT")) return SIGQUIT;
    if (!strcmp(s, "KILL")) return SIGKILL;
    if (!strcmp(s, "TERM")) return SIGTERM;
    if (!strcmp(s, "STOP")) return SIGSTOP;
    if (!strcmp(s, "CONT")) return SIGCONT;
    if (!strcmp(s, "USR1")) return SIGUSR1;
    if (!strcmp(s, "USR2")) return SIGUSR2;
    return -1;
}

static int do_pgrep(int argc, char **argv, int do_kill) {
    int sig = SIGTERM;
    const char *pat = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (do_kill) {
                int s = sig_from_name(a);
                if (s > 0) { sig = s; continue; }
            }
            continue;                 /* -f, -l and friends: accepted */
        }
        if (!pat) pat = a;
    }
    if (!pat) {
        fprintf(stderr, "%s: no pattern given\n", do_kill ? "pkill" : "pgrep");
        return 2;
    }
    DIR *d = opendir("/proc");
    if (!d) { fprintf(stderr, "%s: cannot open /proc\n", prog); return 2; }
    int hits = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        int pid = atoi(e->d_name);
        if (pid <= 1 || pid == (int)getpid()) continue;
        char nm[64];
        if (name_of_pid(e->d_name, nm, sizeof nm) != 0) continue;
        if (!strstr(nm, pat)) continue;      /* pgrep matches a substring */
        hits++;
        if (do_kill) kill(pid, sig);
        else         printf("%d\n", pid);
    }
    closedir(d);
    return hits ? 0 : 1;
}
