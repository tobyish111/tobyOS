/* user_cron -- crontab, native (2026-09-04, native-parity batch).
 *
 *   crontab [-u user] file | -      install (validated like Vixie cron)
 *   crontab [-u user] -l | -r | -e  list / remove / edit ($EDITOR)
 *
 * tobyOS has no crond daemon; like the native `at`, the table's owner gets a
 * detached RUNNER process the first time a table is installed. The runner
 * wakes every minute, re-reads /var/spool/cron/crontabs/<user> (so edits and
 * -r take effect without a restart; a missing table ends the runner), and
 * runs matching entries with /bin/tsh -c under the user's environment. Job
 * output goes to /var/spool/cron/<user>.log -- there is no mail. @reboot
 * entries run once when the runner starts. Day-of-month and day-of-week
 * combine the Vixie way: when both are restricted, either one matching is a
 * match. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define SPOOL_DIR "/var/spool/cron"
#define TABS_DIR  SPOOL_DIR "/crontabs"
#define CRON_SHELL "/bin/tsh"

/* ------------------------------------------------------------ passwd */
static int lookup_user(int uid, char *name, size_t cap, char *home, size_t hcap) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line; char *fld[7]; int k = 0;
        fld[k++] = s;
        for (char *p = s; *p && k < 7; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 6) continue;
        if (atoi(fld[2]) != uid) continue;
        snprintf(name, cap, "%s", fld[0]);
        char *h = fld[5]; size_t hl = strcspn(h, ":\n"); h[hl] = '\0';
        if (home) snprintf(home, hcap, "%s", h);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static int lookup_name(const char *want, int *uid, int *gid, char *home, size_t hcap) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fld[7]; int k = 0; fld[k++] = line;
        for (char *p = line; *p && k < 7; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 6 || strcmp(fld[0], want)) continue;
        *uid = atoi(fld[2]); *gid = atoi(fld[3]);
        char *h = fld[5]; size_t hl = strcspn(h, ":\n"); h[hl] = '\0';
        if (home) snprintf(home, hcap, "%s", h);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

/* ------------------------------------------------------------ parsing */
struct entry {
    uint64_t min;      /* bits 0..59 */
    uint32_t hour;     /* 0..23 */
    uint32_t dom;      /* 1..31 */
    uint16_t mon;      /* 1..12 */
    uint8_t  dow;      /* 0..6 */
    uint8_t  dom_star, dow_star, reboot;
    char     cmd[512];
};

static const char *g_month_names[] = { "jan","feb","mar","apr","may","jun","jul","aug","sep","oct","nov","dec" };
static const char *g_dow_names[]   = { "sun","mon","tue","wed","thu","fri","sat" };

static int name_value(const char *s, const char **names, int n, int base) {
    for (int i = 0; i < n; i++) if (!strncasecmp(s, names[i], 3)) return i + base;
    return -1;
}

/* Parse one field into a bitmask; returns 0 or -1. *star set when the field is
 * a bare "*". */
static int parse_field(const char *s, int lo, int hi, const char **names, int nnames,
                       uint64_t *mask, int *star) {
    *mask = 0; *star = 0;
    if (!strcmp(s, "*")) { *star = 1; for (int i = lo; i <= hi; i++) *mask |= 1ull << i; return 0; }
    const char *p = s;
    while (*p) {
        int a, b, step = 1;
        if (*p == '*') { a = lo; b = hi; p++; }
        else {
            if (isalpha((unsigned char)*p)) { a = names ? name_value(p, names, nnames, lo == 0 && names == g_dow_names ? 0 : 1) : -1; if (a < 0) return -1; p += 3; }
            else if (isdigit((unsigned char)*p)) a = (int)strtol(p, (char **)&p, 10);
            else return -1;
            b = a;
            if (*p == '-') {
                p++;
                if (isalpha((unsigned char)*p)) { b = names ? name_value(p, names, nnames, lo == 0 && names == g_dow_names ? 0 : 1) : -1; if (b < 0) return -1; p += 3; }
                else if (isdigit((unsigned char)*p)) b = (int)strtol(p, (char **)&p, 10);
                else return -1;
            }
        }
        if (*p == '/') { p++; if (!isdigit((unsigned char)*p)) return -1; step = (int)strtol(p, (char **)&p, 10); if (step <= 0) return -1; }
        if (names == g_dow_names) { if (a == 7) a = 0; if (b == 7) b = 0; if (a == 0 && b == 0 && strchr(s, '7')) { /* 0-7 handled below */ } }
        if (a < lo || b > hi || a > b) {
            if (names == g_dow_names && b == 0 && a > 0) { b = 6; *mask |= 1; } /* e.g. 5-7 */
            else return -1;
        }
        for (int i = a; i <= b; i += step) *mask |= 1ull << i;
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return 0;
}

/* Parse a crontab line into e. Returns 1 entry, 0 skip (blank/comment/env),
 * -1 error with *what naming the bad field. */
static int parse_line(const char *line, struct entry *e, const char **what) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n' || *p == '#') return 0;
    /* NAME=value */
    {
        const char *q = p;
        while (isalnum((unsigned char)*q) || *q == '_') q++;
        const char *r = q; while (*r == ' ' || *r == '\t') r++;
        if (q > p && *r == '=') return 0;
    }
    memset(e, 0, sizeof *e);
    char f[5][64];
    if (*p == '@') {
        char name[16]; size_t k = 0;
        while (*p && !isspace((unsigned char)*p) && k < sizeof name - 1) name[k++] = *p++;
        name[k] = '\0';
        const char *spec = NULL;
        if (!strcmp(name, "@reboot")) { e->reboot = 1; }
        else if (!strcmp(name, "@yearly") || !strcmp(name, "@annually")) spec = "0 0 1 1 *";
        else if (!strcmp(name, "@monthly")) spec = "0 0 1 * *";
        else if (!strcmp(name, "@weekly"))  spec = "0 0 * * 0";
        else if (!strcmp(name, "@daily") || !strcmp(name, "@midnight")) spec = "0 0 * * *";
        else if (!strcmp(name, "@hourly")) spec = "0 * * * *";
        else { *what = "bad time specifier"; return -1; }
        if (spec) sscanf(spec, "%63s %63s %63s %63s %63s", f[0], f[1], f[2], f[3], f[4]);
        else { strcpy(f[0], "*"); strcpy(f[1], "*"); strcpy(f[2], "*"); strcpy(f[3], "*"); strcpy(f[4], "*"); }
    } else {
        for (int i = 0; i < 5; i++) {
            while (*p == ' ' || *p == '\t') p++;
            size_t k = 0;
            while (*p && !isspace((unsigned char)*p) && k < sizeof f[i] - 1) f[i][k++] = *p++;
            f[i][k] = '\0';
            if (!k) { *what = i == 0 ? "bad minute" : i == 1 ? "bad hour" : i == 2 ? "bad day-of-month" : i == 3 ? "bad month" : "bad day-of-week"; return -1; }
        }
    }
    uint64_t m; int star;
    if (parse_field(f[0], 0, 59, NULL, 0, &m, &star)) { *what = "bad minute"; return -1; } e->min = m;
    if (parse_field(f[1], 0, 23, NULL, 0, &m, &star)) { *what = "bad hour"; return -1; } e->hour = (uint32_t)m;
    if (parse_field(f[2], 1, 31, NULL, 0, &m, &star)) { *what = "bad day-of-month"; return -1; } e->dom = (uint32_t)m; e->dom_star = (uint8_t)star;
    if (parse_field(f[3], 1, 12, g_month_names, 12, &m, &star)) { *what = "bad month"; return -1; } e->mon = (uint16_t)m;
    if (parse_field(f[4], 0, 7, g_dow_names, 7, &m, &star)) { *what = "bad day-of-week"; return -1; }
    if (m & (1ull << 7)) m |= 1; e->dow = (uint8_t)(m & 0x7F); e->dow_star = (uint8_t)star;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '\n') { *what = "bad command"; return -1; }
    snprintf(e->cmd, sizeof e->cmd, "%s", p);
    size_t l = strlen(e->cmd);
    while (l && (e->cmd[l - 1] == '\n' || e->cmd[l - 1] == '\r')) e->cmd[--l] = '\0';
    return 1;
}

static int validate(const char *path, const char *display) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "crontab: %s: No such file or directory\n", path); return -1; }
    char line[1024]; int ln = 0, errs = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        struct entry e; const char *what = "";
        if (parse_line(line, &e, &what) < 0) { fprintf(stderr, "\"%s\":%d: %s\n", display, ln, what); errs++; }
    }
    fclose(f);
    if (errs) { fprintf(stderr, "errors in crontab file, can't install.\n"); return -1; }
    return 0;
}

/* ------------------------------------------------------------ runner */
static int runner_alive(const char *user) {
    char pidp[256]; snprintf(pidp, sizeof pidp, SPOOL_DIR "/%s.pid", user);
    FILE *f = fopen(pidp, "r");
    if (!f) return 0;
    int pid = 0;
    char pl[32];
    if (fgets(pl, sizeof pl, f)) pid = atoi(pl);
    fclose(f);
    if (pid <= 0) return 0;
    char procp[64]; snprintf(procp, sizeof procp, "/proc/%d/stat", pid);
    struct stat st;
    return stat(procp, &st) == 0;
}

static void run_job(const struct entry *e, const char *user, const char *home, const char *logp,
                    char **envv) {
    pid_t pid = fork();
    if (pid != 0) return;
    int in = open("/dev/null", O_RDONLY);
    if (in >= 0) { dup2(in, 0); if (in > 0) close(in); }
    int out = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (out >= 0) { dup2(out, 1); dup2(out, 2); if (out > 2) close(out); }
    if (chdir(home) != 0) chdir("/");
    (void)user;
    char *av[4] = { (char *)"tsh", (char *)"-c", (char *)e->cmd, NULL };
    execve(CRON_SHELL, av, envv);
    _exit(127);
}

static void runner_main(const char *user, const char *home) {
    char tabp[256], pidp[256], logp[256];
    snprintf(tabp, sizeof tabp, TABS_DIR "/%s", user);
    snprintf(pidp, sizeof pidp, SPOOL_DIR "/%s.pid", user);
    snprintf(logp, sizeof logp, SPOOL_DIR "/%s.log", user);
    FILE *pf = fopen(pidp, "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }
    int in = open("/dev/null", O_RDONLY);
    if (in >= 0) { dup2(in, 0); dup2(in, 1); dup2(in, 2); if (in > 2) close(in); }
    int first = 1;
    long last_minute = -1;
    for (;;) {
        time_t now = time(NULL);
        if (!first) {
            unsigned wait = (unsigned)(60 - (now % 60));
            sleep(wait);
            now = time(NULL);
        }
        struct stat st;
        if (stat(tabp, &st) != 0) { unlink(pidp); _exit(0); }
        long minute = (long)(now / 60);
        if (!first && minute == last_minute) continue;
        last_minute = minute;
        struct tm *tm = localtime(&now);
        FILE *f = fopen(tabp, "r");
        if (!f) { unlink(pidp); _exit(0); }
        /* environment: defaults plus the file's NAME=value lines */
        char *envv[64]; int ne = 0;
        static char eb[8][600];
        snprintf(eb[0], sizeof eb[0], "HOME=%s", home);   envv[ne++] = eb[0];
        snprintf(eb[1], sizeof eb[1], "LOGNAME=%s", user); envv[ne++] = eb[1];
        snprintf(eb[2], sizeof eb[2], "USER=%s", user);    envv[ne++] = eb[2];
        snprintf(eb[3], sizeof eb[3], "SHELL=%s", CRON_SHELL); envv[ne++] = eb[3];
        snprintf(eb[4], sizeof eb[4], "PATH=/usr/bin:/bin"); envv[ne++] = eb[4];
        char line[1024];
        static char envl[32][600]; int nl = 0;
        while (fgets(line, sizeof line, f)) {
            struct entry e; const char *what;
            const char *p = line; while (*p == ' ' || *p == '\t') p++;
            {   /* NAME=value */
                const char *q = p; while (isalnum((unsigned char)*q) || *q == '_') q++;
                const char *r = q; while (*r == ' ' || *r == '\t') r++;
                if (q > p && *r == '=' && nl < 32 && ne < 63) {
                    const char *v = r + 1; while (*v == ' ' || *v == '\t') v++;
                    size_t vl = strcspn(v, "\n");
                    if (vl >= 2 && ((v[0] == '"' && v[vl - 1] == '"') || (v[0] == '\'' && v[vl - 1] == '\''))) { v++; vl -= 2; }
                    snprintf(envl[nl], sizeof envl[nl], "%.*s=%.*s", (int)(q - p), p, (int)vl, v);
                    /* override an earlier same-name entry */
                    int repl = 0;
                    for (int i = 0; i < ne; i++) if (!strncmp(envv[i], envl[nl], (size_t)(q - p) + 1)) { envv[i] = envl[nl]; repl = 1; break; }
                    if (!repl) envv[ne++] = envl[nl];
                    nl++;
                    continue;
                }
            }
            if (parse_line(line, &e, &what) != 1) continue;
            envv[ne] = NULL;
            if (e.reboot) { if (first) run_job(&e, user, home, logp, envv); continue; }
            if (first) continue;          /* regular entries wait for a minute boundary */
            int dom_ok = (e.dom >> tm->tm_mday) & 1;
            int dow_ok = (e.dow >> tm->tm_wday) & 1;
            int day_ok = (!e.dom_star && !e.dow_star) ? (dom_ok || dow_ok) : (dom_ok && dow_ok);
            if (((e.min >> tm->tm_min) & 1) && ((e.hour >> tm->tm_hour) & 1) &&
                ((e.mon >> (tm->tm_mon + 1)) & 1) && day_ok)
                run_job(&e, user, home, logp, envv);
        }
        fclose(f);
        first = 0;
        /* reap finished jobs without blocking */
        int st2; while (waitpid(-1, &st2, WNOHANG) > 0) {}
    }
}

static void ensure_runner(const char *user, const char *home) {
    if (runner_alive(user)) return;
    pid_t pid = fork();
    if (pid != 0) return;
    runner_main(user, home);
    _exit(0);
}

/* ------------------------------------------------------------ crontab */
static int copy_file(const char *src, int from_stdin, const char *dst) {
    FILE *in = from_stdin ? stdin : fopen(src, "r");
    if (!in) return -1;
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { if (!from_stdin) fclose(in); return -1; }
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        const char *p = buf; while (n) { ssize_t w = write(fd, p, n); if (w <= 0) break; p += w; n -= (size_t)w; }
    }
    close(fd);
    if (!from_stdin) fclose(in);
    return 0;
}

static void mkdirs(void) {
    mkdir("/var", 0755); mkdir("/var/spool", 0755); mkdir(SPOOL_DIR, 0755); mkdir(TABS_DIR, 0755);
}

int main(int argc, char **argv) {
    const char *user_opt = NULL, *file = NULL;
    int do_list = 0, do_remove = 0, do_edit = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--version")) { printf("crontab (tobyOS cron) 1.0\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage: crontab [-u user] file\n       crontab [-u user] { -e | -l | -r }\n"); return 0; }
        if (!strcmp(a, "-u") && i + 1 < argc) { user_opt = argv[++i]; continue; }
        if (!strcmp(a, "-l")) { do_list = 1; continue; }
        if (!strcmp(a, "-r")) { do_remove = 1; continue; }
        if (!strcmp(a, "-e")) { do_edit = 1; continue; }
        if (!strcmp(a, "-i")) continue;
        if (!strcmp(a, "-")) { file = "-"; continue; }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "crontab: invalid option -- '%c'\n", a[1]); fprintf(stderr, "usage:\tcrontab [-u user] file\n\tcrontab [-u user] [ -e | -l | -r ]\n"); return 1; }
        file = a;
    }
    int uid = (int)getuid(), gid = 0;
    char user[64] = "", home[256] = "/";
    if (user_opt) {
        if (uid != 0) { fprintf(stderr, "must be privileged to use -u\n"); return 1; }
        if (lookup_name(user_opt, &uid, &gid, home, sizeof home)) { fprintf(stderr, "crontab: user `%s' unknown\n", user_opt); return 1; }
        snprintf(user, sizeof user, "%s", user_opt);
    } else if (lookup_user(uid, user, sizeof user, home, sizeof home)) {
        snprintf(user, sizeof user, "uid%d", uid);
    }
    if (do_list + do_remove + do_edit > 1 || ((do_list || do_remove || do_edit) && file)) {
        fprintf(stderr, "usage:\tcrontab [-u user] file\n\tcrontab [-u user] [ -e | -l | -r ]\n");
        return 1;
    }
    mkdirs();
    char tabp[256]; snprintf(tabp, sizeof tabp, TABS_DIR "/%s", user);
    if (do_list) {
        FILE *f = fopen(tabp, "r");
        if (!f) { fprintf(stderr, "no crontab for %s\n", user); return 1; }
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) fwrite(buf, 1, n, stdout);
        fclose(f);
        return 0;
    }
    if (do_remove) {
        if (unlink(tabp) != 0) { fprintf(stderr, "no crontab for %s\n", user); return 1; }
        return 0;
    }
    if (do_edit) {
        char tmp[256]; snprintf(tmp, sizeof tmp, "/tmp/crontab.%d", (int)getpid());
        struct stat st;
        if (stat(tabp, &st) == 0) copy_file(tabp, 0, tmp);
        else { int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); if (fd >= 0) close(fd); }
        const char *ed = getenv("VISUAL"); if (!ed || !*ed) ed = getenv("EDITOR"); if (!ed || !*ed) ed = "vi";
        char cmd[512]; snprintf(cmd, sizeof cmd, "%s %s", ed, tmp);
        int rc = system(cmd);
        if (rc != 0) { fprintf(stderr, "crontab: edits left in %s\n", tmp); return 1; }
        if (validate(tmp, tmp)) { fprintf(stderr, "crontab: edits left in %s\n", tmp); return 1; }
        copy_file(tmp, 0, tabp);
        unlink(tmp);
        fprintf(stderr, "crontab: installing new crontab\n");
        ensure_runner(user, home);
        return 0;
    }
    if (!file) {
        fprintf(stderr, "usage:\tcrontab [-u user] file\n\tcrontab [-u user] [ -e | -l | -r ]\n");
        return 1;
    }
    int from_stdin = !strcmp(file, "-");
    if (from_stdin) {
        /* stdin can only be read once: spool it first, validate the copy */
        char tmp[256]; snprintf(tmp, sizeof tmp, "/tmp/crontab.%d", (int)getpid());
        if (copy_file(NULL, 1, tmp)) { fprintf(stderr, "crontab: cannot read standard input\n"); return 1; }
        if (validate(tmp, "-")) { unlink(tmp); return 1; }
        copy_file(tmp, 0, tabp);
        unlink(tmp);
    } else {
        if (validate(file, file)) return 1;
        if (copy_file(file, 0, tabp)) { fprintf(stderr, "crontab: %s: cannot install\n", file); return 1; }
    }
    ensure_runner(user, home);
    return 0;
}
