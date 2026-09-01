/* at / batch / atq / atrm -- deferred command execution.
 *
 *     at [-f file] [-m] [-q Q] TIMESPEC     read the job from stdin or -f
 *     at -t CCYYMMDDhhmm [-f file]
 *     at -l                                 == atq
 *     at -r JOB ...                         == atrm
 *     batch [-f file]                       run as soon as possible
 *     atq [-q Q]
 *     atrm JOB ...
 *
 * THERE IS NO atd HERE. A daemon that polls a spool directory is the way
 * this works on a distro because a distro's spool survives a reboot; this
 * initrd's /var is RAM, so a job could not outlive the machine anyway.
 * Instead each `at` leaves a detached runner of its own, which sleeps
 * until the time, RE-READS the spool file, and runs it only if it is
 * still there -- which is what makes `atrm` genuinely cancel a job rather
 * than only hide it from `atq`.
 *
 * Consequences, said plainly rather than discovered later:
 *   * jobs do not survive a reboot,
 *   * output is written next to the job as JOB.out instead of mailed,
 *   * `atq` shows jobs from this boot only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* Both are overridable so the tool can be exercised somewhere other than
 * the one path it ships with. */
#ifndef AT_SPOOL
#define AT_SPOOL "/var/spool/at"
#endif
#ifndef AT_SHELL
#define AT_SHELL "/bin/tsh"
#endif

static const char *prog = "at";
static int mail_flag, queue = 'a';

/* Creates the spool and every parent it needs, rather than naming three
 * fixed directories -- the path is a build-time knob now. */
static void ensure_spool(void) {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "%s", AT_SPOOL);
    for (char *q = tmp + 1; *q; q++) {
        if (*q != '/') continue;
        *q = 0;
        mkdir(tmp, 0755);
        *q = '/';
    }
    mkdir(tmp, 0755);
}

/* ------------------------------------------------------- the time spec */

/* Accepts the shapes people actually type:
 *   now
 *   now + 5 minutes | 2 hours | 1 day | 3 weeks
 *   HH:MM            (today, or tomorrow if it has passed)
 *   H:MMam / H:MMpm
 *   midnight | noon | teatime
 * and, via -t, CCYYMMDDhhmm. Anything else is REFUSED rather than
 * silently rounded to "now", which would run a job the user meant to
 * schedule for the night. */
static int parse_units(const char *u, long *mult) {
    if (!strncmp(u, "min", 3))  { *mult = 60;     return 1; }
    if (!strncmp(u, "hour", 4)) { *mult = 3600;   return 1; }
    if (!strncmp(u, "day", 3))  { *mult = 86400;  return 1; }
    if (!strncmp(u, "week", 4)) { *mult = 604800; return 1; }
    if (!strncmp(u, "sec", 3))  { *mult = 1;      return 1; }
    return 0;
}

static int hhmm_today(int h, int m, time_t now, time_t *out) {
    struct tm tmv;
    if (!localtime_r(&now, &tmv)) return 0;
    tmv.tm_hour = h;
    tmv.tm_min  = m;
    tmv.tm_sec  = 0;
    time_t t = mktime(&tmv);
    if (t <= now) t += 86400;      /* a time that has passed means tomorrow */
    *out = t;
    return 1;
}

static int parse_when(int argc, char **argv, int start, time_t *out) {
    time_t now = time(NULL);
    if (start >= argc) return 0;

    const char *w = argv[start];
    time_t base = now;
    int i = start;

    if (!strcmp(w, "now")) { i++; }
    else if (!strcmp(w, "midnight")) { if (!hhmm_today(0, 0, now, &base)) return 0; i++; }
    else if (!strcmp(w, "noon"))     { if (!hhmm_today(12, 0, now, &base)) return 0; i++; }
    else if (!strcmp(w, "teatime"))  { if (!hhmm_today(16, 0, now, &base)) return 0; i++; }
    else if (strchr(w, ':')) {
        int h = 0, m = 0;
        const char *p = w;
        while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
        if (*p != ':') return 0;
        p++;
        while (*p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0');
        if (!strncmp(p, "pm", 2) || !strncmp(p, "PM", 2)) { if (h < 12) h += 12; p += 2; }
        else if (!strncmp(p, "am", 2) || !strncmp(p, "AM", 2)) { if (h == 12) h = 0; p += 2; }
        if (*p) return 0;
        if (h > 23 || m > 59) return 0;
        if (!hhmm_today(h, m, now, &base)) return 0;
        i++;
    } else if (w[0] == '+') {
        /* handled by the offset loop below */
    } else {
        return 0;
    }

    /* an optional "+ N units", possibly written "+5min" */
    while (i < argc) {
        const char *a = argv[i];
        long n = 0, mult = 0;
        const char *u = NULL;
        if (!strcmp(a, "+")) {
            if (i + 1 >= argc) return 0;
            const char *num = argv[++i];
            while (*num >= '0' && *num <= '9') n = n * 10 + (*num++ - '0');
            if (*num) u = num;
            else if (i + 1 < argc) u = argv[++i];
            else return 0;
        } else if (a[0] == '+') {
            const char *num = a + 1;
            while (*num >= '0' && *num <= '9') n = n * 10 + (*num++ - '0');
            if (*num) u = num;
            else if (i + 1 < argc) u = argv[++i];
            else return 0;
        } else {
            return 0;
        }
        if (!parse_units(u, &mult)) return 0;
        base += n * mult;
        i++;
    }

    *out = base;
    return 1;
}

static int parse_t(const char *s, time_t *out) {
    /* CCYYMMDDhhmm[.ss] */
    size_t n = strlen(s);
    if (n < 12) return 0;
    int v[6];
    static const int w[6] = { 4, 2, 2, 2, 2 };
    size_t p = 0;
    for (int k = 0; k < 5; k++) {
        v[k] = 0;
        for (int j = 0; j < w[k]; j++) {
            if (s[p] < '0' || s[p] > '9') return 0;
            v[k] = v[k] * 10 + (s[p++] - '0');
        }
    }
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = v[0] - 1900;
    tmv.tm_mon  = v[1] - 1;
    tmv.tm_mday = v[2];
    tmv.tm_hour = v[3];
    tmv.tm_min  = v[4];
    time_t t = mktime(&tmv);
    if (t == (time_t)-1) return 0;
    *out = t;
    return 1;
}

/* ------------------------------------------------------------ the spool */

static int next_job_id(void) {
    DIR *d = opendir(AT_SPOOL);
    int max = 0;
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "job.", 4)) continue;
            int id = atoi(e->d_name + 4);
            if (id > max) max = id;
        }
        closedir(d);
    }
    return max + 1;
}

static void job_path(int id, char *out, size_t cap) {
    snprintf(out, cap, "%s/job.%d", AT_SPOOL, id);
}

/* The runner: a detached child that waits, re-checks, then runs.
 * Re-checking is the whole point -- it is what atrm cancels. */
static void run_later(int id, time_t when) {
    pid_t pid = fork();
    if (pid != 0) return;                    /* parent (and any error) returns */

    char path[256], outp[280];
    job_path(id, path, sizeof path);
    snprintf(outp, sizeof outp, "%s.out", path);

    for (;;) {
        time_t now = time(NULL);
        if (now >= when) break;
        struct stat st;
        if (stat(path, &st) != 0) _exit(0);  /* atrm removed it */
        long left = (long)(when - now);
        sleep(left > 5 ? 5 : (unsigned)left);
    }
    struct stat st;
    if (stat(path, &st) != 0) _exit(0);

    int fd = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    char *av[4];
    av[0] = (char *)"tsh";
    av[1] = path;
    av[2] = NULL;
    execv(AT_SHELL, av);
    _exit(127);
}

static int cmd_at(int argc, char **argv, int is_batch) {
    ensure_spool();

    const char *fromfile = NULL;
    time_t when = 0;
    int have_when = 0, first_spec = argc;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-f") && i + 1 < argc) { fromfile = argv[++i]; continue; }
        if (!strcmp(a, "-q") && i + 1 < argc) { queue = argv[++i][0]; continue; }
        if (!strcmp(a, "-t") && i + 1 < argc) {
            if (!parse_t(argv[++i], &when)) {
                fprintf(stderr, "at: -t wants CCYYMMDDhhmm\n");
                return 1;
            }
            have_when = 1;
            continue;
        }
        if (!strcmp(a, "-m")) { mail_flag = 1; continue; }
        if (!strcmp(a, "-M")) { mail_flag = 0; continue; }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("at (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: at [-f file] [-q Q] TIMESPEC | at -t CCYYMMDDhhmm | at -l | at -r JOB\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "at: unknown option %s\n", a); return 1; }
        if (first_spec == argc) first_spec = i;
    }

    if (is_batch) {
        /* batch means "as soon as the machine is willing". There is no load
         * average to consult here, so it means one minute from now -- late
         * enough to be a queue, soon enough to be useful. */
        if (!have_when) { when = time(NULL) + 60; have_when = 1; }
    } else if (!have_when) {
        if (!parse_when(argc, argv, first_spec, &when)) {
            fprintf(stderr, "at: cannot understand that time\n");
            fprintf(stderr, "at: try `now', `now + 5 minutes', `HH:MM', "
                            "`midnight', `noon', `teatime' or -t CCYYMMDDhhmm\n");
            return 1;
        }
    }

    int id = next_job_id();
    char path[256];
    job_path(id, path, sizeof path);

    FILE *out = fopen(path, "w");
    if (!out) { fprintf(stderr, "at: cannot write %s: %s\n", path, strerror(errno)); return 1; }
    fprintf(out, "#!%s\n", AT_SHELL);
    fprintf(out, "# atjob id=%d at=%ld queue=%c\n", id, (long)when, queue);

    FILE *in = stdin;
    if (fromfile) {
        in = fopen(fromfile, "r");
        if (!in) {
            fprintf(stderr, "at: cannot read %s: %s\n", fromfile, strerror(errno));
            fclose(out);
            unlink(path);
            return 1;
        }
    }
    char line[1024];
    int wrote = 0;
    while (fgets(line, sizeof line, in)) { fputs(line, out); wrote = 1; }
    if (fromfile) fclose(in);
    fclose(out);

    if (!wrote) {
        fprintf(stderr, "at: no commands given -- job not queued\n");
        unlink(path);
        return 1;
    }
    chmod(path, 0700);

    char stamp[64];
    struct tm tmv;
    if (localtime_r(&when, &tmv)) strftime(stamp, sizeof stamp, "%a %b %e %H:%M:%S %Y", &tmv);
    else snprintf(stamp, sizeof stamp, "%ld", (long)when);
    fprintf(stderr, "job %d at %s\n", id, stamp);

    run_later(id, when);
    return 0;
}

struct jobinfo { int id; time_t when; char queue; };

static int read_job(const char *name, struct jobinfo *ji) {
    char path[280];
    snprintf(path, sizeof path, "%s/%s", AT_SPOOL, name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int ok = 0;
    /* the header is the second line; nothing else is trusted to be there */
    if (fgets(line, sizeof line, f) && fgets(line, sizeof line, f)) {
        long w = 0;
        char q = 'a';
        int id = 0;
        if (sscanf(line, "# atjob id=%d at=%ld queue=%c", &id, &w, &q) == 3) {
            ji->id = id; ji->when = (time_t)w; ji->queue = q;
            ok = 1;
        }
    }
    fclose(f);
    return ok;
}

static int cmd_atq(int argc, char **argv) {
    ensure_spool();
    char want = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-q") && i + 1 < argc) want = argv[++i][0];

    DIR *d = opendir(AT_SPOOL);
    if (!d) return 0;
    struct dirent *e;
    struct jobinfo jobs[256];
    int n = 0;
    while ((e = readdir(d)) && n < 256) {
        if (strncmp(e->d_name, "job.", 4)) continue;
        if (strstr(e->d_name, ".out")) continue;
        if (read_job(e->d_name, &jobs[n])) {
            if (!want || jobs[n].queue == want) n++;
        }
    }
    closedir(d);

    /* oldest first, the way atq lists */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (jobs[j].when < jobs[i].when) {
                struct jobinfo t = jobs[i]; jobs[i] = jobs[j]; jobs[j] = t;
            }

    for (int i = 0; i < n; i++) {
        char stamp[64];
        struct tm tmv;
        if (localtime_r(&jobs[i].when, &tmv))
            strftime(stamp, sizeof stamp, "%a %b %e %H:%M:%S %Y", &tmv);
        else snprintf(stamp, sizeof stamp, "%ld", (long)jobs[i].when);
        printf("%d\t%s %c root\n", jobs[i].id, stamp, jobs[i].queue);
    }
    return 0;
}

static int cmd_atrm(int argc, char **argv) {
    ensure_spool();
    int rc = 0, removed = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        int id = atoi(argv[i]);
        char path[256];
        job_path(id, path, sizeof path);
        if (unlink(path) != 0) {
            fprintf(stderr, "atrm: cannot find job %d\n", id);
            rc = 1;
        } else {
            removed++;
        }
    }
    if (!removed && rc == 0) { fprintf(stderr, "usage: atrm JOB ...\n"); return 1; }
    return rc;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "at";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    (void)prog;

    if (!strcmp(base, "atq"))   return cmd_atq(argc, argv);
    if (!strcmp(base, "atrm"))  return cmd_atrm(argc, argv);
    if (!strcmp(base, "batch")) return cmd_at(argc, argv, 1);

    /* at -l and at -r are the same tools under at's own flags */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) return cmd_atq(argc - i, argv + i);
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-d"))
            return cmd_atrm(argc - i, argv + i);
    }
    return cmd_at(argc, argv, 0);
}
