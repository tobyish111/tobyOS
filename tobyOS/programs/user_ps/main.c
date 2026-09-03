/* ps -- report a snapshot of the current processes, procps style.
 *
 * Reads /proc/<pid>/{stat,status,cmdline}, /proc/uptime, /proc/stat and
 * /proc/meminfo -- the same files procps and busybox read on Linux -- so a
 * script that parses `ps -o pid= -C name` or `ps aux | grep x` on Linux
 * parses the same bytes here.
 *
 * HISTORY. Until 2026-09-03 /bin/ps listed SERVICES (ABI_SYS_SVC_LIST)
 * while the kernel shell's `ps` builtin listed processes: two different
 * things under one name, and the one every Linux user types answered the
 * wrong question. The service listing lives where it belongs, `svcctl
 * list`; the process table this reads was built by the Linux slices and
 * had been serving busybox `ps` all along. The one kernel gap was that
 * /proc/<pid>/stat reported pgrp, session and starttime as 0, so ELAPSED,
 * START and %CPU could not exist; that is fixed in the same commit.
 *
 * Supported: ps, ps ax, ps aux, ps -e/-A, ps -ef, ps -el, -o/--format with
 * every common column name and the `name=HEADER` form, -p/--pid, -C,
 * -u/-U/--user, --ppid, -L/-T (threads), -h/--no-headers, -w, --sort by
 * a numeric or string column (a leading - reverses), --version, --help.
 *
 * Without a selection option procps shows "the invoker's processes on the
 * invoker's terminal". There is no controlling-tty record here, so the
 * terminal is approximated by the login session (/proc/<pid>/stat field 6),
 * which is what a tty stands for.
 *
 * Widths are classic procps (pid_max 32768): scripts do not depend on them,
 * eyes do. Exit status is procps's: 0 if at least one process was listed,
 * 1 if the selection was empty. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

#define HZ 100

struct pinfo {
    int  pid, ppid, pgrp, session, tty, tgid, nthreads;
    int  uid;
    char state;
    long utime, stime;          /* ticks */
    long starttime;             /* ticks since boot */
    long vsize;                 /* bytes */
    long rss_kb;
    int  prio, nice;
    char comm[64];
    char args[512];
    int  selected;
};

static struct pinfo *g_procs;
static int g_nprocs, g_cap;

/* No floating point anywhere: native programs are built without SSE, so
 * every percentage is carried as integer tenths. */
static long   g_uptime_cs;      /* centiseconds since boot */
static long   g_btime;          /* epoch seconds of boot */
static long   g_memtotal_kb = 1;
static int    g_self_pid, g_self_uid, g_self_session;

/* ---- small readers ------------------------------------------------- */

static int slurp(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

static void read_system(void) {
    char buf[4096];
    g_uptime_cs = 0;
    if (slurp("/proc/uptime", buf, sizeof buf) > 0) {
        /* "1234.56 1234.56": whole seconds, then up to two decimals */
        char *e;
        long s = strtol(buf, &e, 10), cs = 0;
        if (*e == '.') {
            e++;
            if (*e >= '0' && *e <= '9') cs = (*e++ - '0') * 10;
            if (*e >= '0' && *e <= '9') cs += *e - '0';
        }
        g_uptime_cs = s * 100 + cs;
    }
    if (slurp("/proc/stat", buf, sizeof buf) > 0) {
        char *b = strstr(buf, "btime ");
        if (b) g_btime = atol(b + 6);
    }
    if (slurp("/proc/meminfo", buf, sizeof buf) > 0) {
        char *m = strstr(buf, "MemTotal:");
        if (m) { long v = atol(m + 9); if (v > 0) g_memtotal_kb = v; }
    }
}

static int read_stat(int pid, struct pinfo *pi) {
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (slurp(path, buf, sizeof buf) <= 0) return -1;
    /* comm is parenthesised and may contain spaces: split on the LAST ')' */
    char *lp = strchr(buf, '('), *rp = strrchr(buf, ')');
    if (!lp || !rp || rp < lp) return -1;
    size_t cl = (size_t)(rp - lp - 1);
    if (cl >= sizeof pi->comm) cl = sizeof pi->comm - 1;
    memcpy(pi->comm, lp + 1, cl);
    pi->comm[cl] = '\0';
    pi->pid = atoi(buf);
    /* fields from 3 on */
    char *p = rp + 2;
    char st = *p ? *p : '?';
    pi->state = st;
    long f[52];
    int n = 0;
    for (n = 0; n < 52; n++) f[n] = 0;
    /* skip the state token */
    while (*p && *p != ' ') p++;
    n = 4;                                  /* next token is field 4 */
    while (*p && n <= 52) {
        while (*p == ' ') p++;
        if (!*p) break;
        char *e;
        long v = strtol(p, &e, 10);
        if (e == p) break;
        f[n - 1] = v;
        p = e;
        n++;
    }
    pi->ppid      = (int)f[3];
    pi->pgrp      = (int)f[4];
    pi->session   = (int)f[5];
    pi->tty       = (int)f[6];
    pi->utime     = f[13];
    pi->stime     = f[14];
    pi->prio      = (int)f[17];
    pi->nice      = (int)f[18];
    pi->nthreads  = (int)f[19];
    pi->starttime = f[21];
    pi->vsize     = f[22];
    pi->rss_kb    = f[23] * 4;
    return 0;
}

static int read_status(int pid, struct pinfo *pi) {
    char path[64], buf[2048];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (slurp(path, buf, sizeof buf) <= 0) return -1;
    pi->tgid = pi->pid;
    pi->uid = 0;
    char *t = strstr(buf, "Tgid:");
    if (t) pi->tgid = atoi(t + 5);
    char *u = strstr(buf, "Uid:");
    if (u) {
        /* real, EFFECTIVE, saved, fs -- procps's USER is the effective one */
        char *q = u + 4;
        long r = strtol(q, &q, 10);
        long e = strtol(q, &q, 10);
        (void)r;
        pi->uid = (int)e;
    }
    char *th = strstr(buf, "Threads:");
    if (th) pi->nthreads = atoi(th + 8);
    return 0;
}

static void read_cmdline(int pid, struct pinfo *pi) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    int n = slurp(path, buf, sizeof buf);
    if (n <= 0) {
        snprintf(pi->args, sizeof pi->args, "[%s]", pi->comm);
        return;
    }
    /* NUL-separated argv -> space-separated; drop the trailing NUL(s) */
    while (n > 0 && buf[n - 1] == '\0') n--;
    for (int i = 0; i < n; i++) if (buf[i] == '\0') buf[i] = ' ';
    buf[n] = '\0';
    if (n == 0) snprintf(pi->args, sizeof pi->args, "[%s]", pi->comm);
    else        snprintf(pi->args, sizeof pi->args, "%s", buf);
}

static void read_procs(void) {
    DIR *d = opendir("/proc");
    if (!d) { fprintf(stderr, "ps: cannot open /proc\n"); exit(1); }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] < '0' || nm[0] > '9') continue;
        int pid = atoi(nm);
        if (g_nprocs == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 64;
            g_procs = realloc(g_procs, (size_t)g_cap * sizeof *g_procs);
            if (!g_procs) { fprintf(stderr, "ps: out of memory\n"); exit(1); }
        }
        struct pinfo *pi = &g_procs[g_nprocs];
        memset(pi, 0, sizeof *pi);
        if (read_stat(pid, pi) != 0) continue;   /* it exited under us */
        read_status(pid, pi);
        read_cmdline(pid, pi);
        g_nprocs++;
    }
    closedir(d);
}

/* ---- users --------------------------------------------------------- */

static const char *user_name(int uid) {
    static char cache[64][40];
    static int  cache_uid[64], ncache;
    for (int i = 0; i < ncache; i++) if (cache_uid[i] == uid) return cache[i];
    char *slot = ncache < 64 ? cache[ncache] : cache[63];
    snprintf(slot, 40, "%d", uid);
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *c1 = strchr(line, ':'); if (!c1) continue;
            char *c2 = strchr(c1 + 1, ':'); if (!c2) continue;
            if (atoi(c2 + 1) == uid) {
                *c1 = '\0';
                snprintf(slot, 40, "%s", line);
                break;
            }
        }
        fclose(f);
    }
    if (ncache < 64) cache_uid[ncache++] = uid;
    return slot;
}

static int user_lookup(const char *name) {
    char *e;
    long v = strtol(name, &e, 10);
    if (*name && !*e) return (int)v;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[256];
    int uid = -1;
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':'); if (!c1) continue;
        *c1 = '\0';
        if (strcmp(line, name) == 0) {
            char *c2 = strchr(c1 + 1, ':');
            if (c2) uid = atoi(c2 + 1);
            break;
        }
    }
    fclose(f);
    return uid;
}

/* ---- formatting ---------------------------------------------------- */

static void fmt_hms(long secs, int bsd, char *out, size_t cap) {
    long d = secs / 86400, h = secs / 3600 % 24, m = secs / 60 % 60, s = secs % 60;
    if (bsd) {
        /* procps bsdtime: MMM:SS, minutes not bounded by the hour */
        snprintf(out, cap, "%ld:%02ld", secs / 60, s);
    } else if (d > 0) {
        snprintf(out, cap, "%ld-%02ld:%02ld:%02ld", d, h, m, s);
    } else {
        snprintf(out, cap, "%02ld:%02ld:%02ld", h, m, s);
    }
}

static void fmt_etime(long secs, char *out, size_t cap) {
    long d = secs / 86400, h = secs / 3600 % 24, m = secs / 60 % 60, s = secs % 60;
    if (d > 0)      snprintf(out, cap, "%ld-%02ld:%02ld:%02ld", d, h, m, s);
    else if (h > 0) snprintf(out, cap, "%02ld:%02ld:%02ld", h, m, s);
    else            snprintf(out, cap, "%02ld:%02ld", m, s);
}

static long start_epoch(const struct pinfo *pi) {
    return g_btime + pi->starttime / HZ;
}

static long elapsed_secs(const struct pinfo *pi) {
    long e = g_uptime_cs / 100 - pi->starttime / HZ;
    return e < 0 ? 0 : e;
}

/* procps START: HH:MM if started today, else "MmmDD", else the year if it
 * was a different year. STIME (-f) is the same rendering. */
static void fmt_start(const struct pinfo *pi, char *out, size_t cap) {
    time_t st = (time_t)start_epoch(pi), now = time(0);
    struct tm a, b;
    gmtime_r(&st, &a);
    gmtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        strftime(out, cap, "%H:%M", &a);
    else if (a.tm_year == b.tm_year)
        strftime(out, cap, "%b%d", &a);
    else
        strftime(out, cap, "%Y", &a);
}

static void fmt_stat(const struct pinfo *pi, char *out, size_t cap) {
    int n = 0;
    out[n++] = pi->state;
    if (pi->nice < 0) out[n++] = '<';
    else if (pi->nice > 0) out[n++] = 'N';
    if (pi->session == pi->pid) out[n++] = 's';
    if (pi->nthreads > 1) out[n++] = 'l';
    out[n] = '\0';
    (void)cap;
}

/* percent * 10: cpu ticks are centiseconds (HZ = 100), so ticks * 1000 /
 * elapsed centiseconds is tenths of a percent. procps caps at 99.9. */
static long pcpu_tenths(const struct pinfo *pi) {
    long e_cs = g_uptime_cs - pi->starttime;      /* both in ticks/cs */
    if (e_cs < 100) e_cs = 100;
    long v = (pi->utime + pi->stime) * 1000 / e_cs;
    if (v > 999) v = 999;
    return v;
}

static long pmem_tenths(const struct pinfo *pi) {
    long v = pi->rss_kb * 1000 / g_memtotal_kb;
    if (v > 999) v = 999;
    return v;
}

/* One column: procps's names, header, width and alignment. */
struct col {
    const char *names;      /* comma-separated aliases */
    const char *header;
    int width;              /* 0 = the rest of the line */
    int right;
    int id;
};

enum {
    C_PID, C_PPID, C_PGID, C_SID, C_USER, C_UID, C_RUSER, C_PCPU, C_PMEM, C_VSZ,
    C_RSS, C_TTY, C_STAT, C_S, C_START, C_LSTART, C_TIME, C_BSDTIME, C_ETIME,
    C_ETIMES, C_COMM, C_ARGS, C_CMD, C_NLWP, C_LWP, C_TGID, C_PRI, C_NI, C_C,
    C_F, C_SZ, C_WCHAN, C_ADDR, C_CLS, C_RTPRIO, C_PSR, C_GID, C_GROUP
};

static const struct col g_cols[] = {
    { "pid,tgid",              "PID",     5, 1, C_PID },
    { "ppid",                  "PPID",    5, 1, C_PPID },
    { "pgid,pgrp",             "PGID",    5, 1, C_PGID },
    { "sid,sess,session",      "SID",     5, 1, C_SID },
    { "user,uname,euser",      "USER",    8, 0, C_USER },
    { "uid,euid",              "UID",     5, 1, C_UID },
    { "ruser",                 "RUSER",   8, 0, C_RUSER },
    { "%cpu,pcpu,cpu",         "%CPU",    4, 1, C_PCPU },
    { "%mem,pmem",             "%MEM",    4, 1, C_PMEM },
    { "vsz,vsize",             "VSZ",     6, 1, C_VSZ },
    { "rss,rsz,rssize",        "RSS",     5, 1, C_RSS },
    { "tty,tt,tname",          "TT",      8, 0, C_TTY },
    { "stat",                  "STAT",    4, 0, C_STAT },
    { "state,s",               "S",       1, 0, C_S },
    { "start,start_time,bsdstart", "START", 5, 0, C_START },
    { "stime",                "STIME",   5, 0, C_START },
    { "lstart",                "STARTED", 24, 0, C_LSTART },
    { "time,cputime",          "TIME",    8, 1, C_TIME },
    { "bsdtime",               "TIME",    6, 1, C_BSDTIME },
    { "etime",                 "ELAPSED", 11, 1, C_ETIME },
    { "etimes",                "ELAPSED", 7, 1, C_ETIMES },
    { "comm,ucomm,ucmd",       "COMMAND", 15, 0, C_COMM },
    { "args,command",          "COMMAND", 0, 0, C_ARGS },
    { "cmd",                   "CMD",     0, 0, C_CMD },
    { "nlwp,thcount",          "NLWP",    4, 1, C_NLWP },
    { "lwp,tid,spid",          "LWP",     5, 1, C_LWP },
    { "pri",                   "PRI",     3, 1, C_PRI },
    { "ni,nice",               "NI",      3, 1, C_NI },
    { "c",                     "C",       2, 1, C_C },
    { "f,flags,flag",          "F",       1, 1, C_F },
    { "sz",                    "SZ",      5, 1, C_SZ },
    { "wchan",                 "WCHAN",   6, 0, C_WCHAN },
    { "addr",                  "ADDR",    4, 0, C_ADDR },
    { "cls,class,policy",      "CLS",     3, 0, C_CLS },
    { "rtprio",                "RTPRIO",  6, 1, C_RTPRIO },
    { "psr",                   "PSR",     3, 1, C_PSR },
    { "gid,egid",              "GID",     5, 1, C_GID },
    { "group,egroup",          "GROUP",   8, 0, C_GROUP },
    { 0, 0, 0, 0, 0 }
};

static const struct col *find_col(const char *name, size_t len) {
    for (const struct col *c = g_cols; c->names; c++) {
        const char *p = c->names;
        while (*p) {
            const char *q = strchr(p, ',');
            size_t l = q ? (size_t)(q - p) : strlen(p);
            if (l == len && strncmp(p, name, len) == 0) return c;
            if (!q) break;
            p = q + 1;
        }
    }
    return 0;
}

struct outcol {
    const struct col *c;
    char header[32];
    int  width;
    int  fixed_header;      /* name=HEADER given */
};
static struct outcol g_out[48];
static int g_nout;

static void add_col(const char *spec, size_t len) {
    const char *eq = memchr(spec, '=', len);
    size_t nlen = eq ? (size_t)(eq - spec) : len;
    const struct col *c = find_col(spec, nlen);
    if (!c) {
        fprintf(stderr, "ps: error: unknown user-defined format specifier \"%.*s\"\n", (int)nlen, spec);
        exit(1);
    }
    if (g_nout >= 48) return;
    struct outcol *o = &g_out[g_nout++];
    o->c = c;
    o->width = c->width;
    if (eq) {
        size_t hl = len - nlen - 1;
        if (hl >= sizeof o->header) hl = sizeof o->header - 1;
        memcpy(o->header, eq + 1, hl);
        o->header[hl] = '\0';
        o->fixed_header = 1;
    } else {
        snprintf(o->header, sizeof o->header, "%s", c->header);
    }
    if (o->width && (int)strlen(o->header) > o->width) o->width = (int)strlen(o->header);
}

/* -o "pid,comm" or -o "pid comm" or -o pid=P,comm=C. A header that contains
 * a comma cannot be written in this syntax, as in procps. */
static void add_format(const char *fmt) {
    const char *p = fmt;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        add_col(p, (size_t)(e - p));
        p = e;
    }
}

static void render(const struct pinfo *pi, int id, char *out, size_t cap) {
    switch (id) {
    case C_PID:     snprintf(out, cap, "%d", pi->tgid ? pi->tgid : pi->pid); break;
    case C_LWP:     snprintf(out, cap, "%d", pi->pid); break;
    case C_TGID:    snprintf(out, cap, "%d", pi->tgid); break;
    case C_PPID:    snprintf(out, cap, "%d", pi->ppid); break;
    case C_PGID:    snprintf(out, cap, "%d", pi->pgrp); break;
    case C_SID:     snprintf(out, cap, "%d", pi->session); break;
    case C_USER: case C_RUSER: {
        const char *u = user_name(pi->uid);
        /* procps truncates a long name to 7 characters plus '+' */
        if (strlen(u) > 8) snprintf(out, cap, "%.7s+", u);
        else snprintf(out, cap, "%s", u);
        break;
    }
    case C_UID:     snprintf(out, cap, "%d", pi->uid); break;
    case C_GID:     snprintf(out, cap, "%d", 0); break;
    case C_GROUP:   snprintf(out, cap, "%s", "root"); break;
    case C_PCPU: { long v = pcpu_tenths(pi); snprintf(out, cap, "%ld.%ld", v / 10, v % 10); break; }
    case C_PMEM: { long v = pmem_tenths(pi); snprintf(out, cap, "%ld.%ld", v / 10, v % 10); break; }
    case C_VSZ:     snprintf(out, cap, "%ld", pi->vsize / 1024); break;
    case C_SZ:      snprintf(out, cap, "%ld", pi->vsize / 4096); break;
    case C_RSS:     snprintf(out, cap, "%ld", pi->rss_kb); break;
    case C_TTY:     snprintf(out, cap, "%s", pi->tty ? "tty" : "?"); break;
    case C_STAT:    fmt_stat(pi, out, cap); break;
    case C_S:       snprintf(out, cap, "%c", pi->state); break;
    case C_START:   fmt_start(pi, out, cap); break;
    case C_LSTART: {
        time_t st = (time_t)start_epoch(pi);
        struct tm a; gmtime_r(&st, &a);
        strftime(out, cap, "%a %b %e %H:%M:%S %Y", &a);
        break;
    }
    case C_TIME:    fmt_hms((pi->utime + pi->stime) / HZ, 0, out, cap); break;
    case C_BSDTIME: fmt_hms((pi->utime + pi->stime) / HZ, 1, out, cap); break;
    case C_ETIME:   fmt_etime(elapsed_secs(pi), out, cap); break;
    case C_ETIMES:  snprintf(out, cap, "%ld", elapsed_secs(pi)); break;
    case C_COMM:    snprintf(out, cap, "%s", pi->comm); break;
    case C_ARGS: case C_CMD: snprintf(out, cap, "%s", pi->args); break;
    case C_NLWP:    snprintf(out, cap, "%d", pi->nthreads > 0 ? pi->nthreads : 1); break;
    case C_PRI:     snprintf(out, cap, "%d", 80 - pi->prio); break;
    case C_NI:      snprintf(out, cap, "%d", pi->nice); break;
    case C_C:       snprintf(out, cap, "%ld", pcpu_tenths(pi) / 10); break;
    case C_F:       snprintf(out, cap, "%d", pi->pid == 0 ? 4 : 0); break;
    case C_WCHAN:   snprintf(out, cap, "%s", pi->state == 'S' ? "-" : "-"); break;
    case C_ADDR:    snprintf(out, cap, "-"); break;
    case C_CLS:     snprintf(out, cap, "TS"); break;
    case C_RTPRIO:  snprintf(out, cap, "-"); break;
    case C_PSR:     snprintf(out, cap, "0"); break;
    default:        snprintf(out, cap, "?"); break;
    }
}

/* ---- selection ----------------------------------------------------- */

#define LIST_MAX 64
static int   g_pids[LIST_MAX], g_npids;
static int   g_ppids[LIST_MAX], g_nppids;
static int   g_uids[LIST_MAX], g_nuids;
static char  g_cmds[LIST_MAX][64]; static int g_ncmds;
static int   g_all, g_show_threads, g_no_header, g_any_selector;
static int   g_sort_col = -1, g_sort_desc;

static void add_int_list(const char *s, int *arr, int *n) {
    g_any_selector = 1;
    while (*s) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        char *e;
        long v = strtol(s, &e, 10);
        if (e == s) { fprintf(stderr, "ps: error: process ID list syntax error\n"); exit(1); }
        if (*n < LIST_MAX) arr[(*n)++] = (int)v;
        s = e;
    }
}

static void add_user_list(const char *s) {
    g_any_selector = 1;
    while (*s) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != ',' && *e != ' ') e++;
        char name[64];
        size_t l = (size_t)(e - s); if (l >= sizeof name) l = sizeof name - 1;
        memcpy(name, s, l); name[l] = '\0';
        int uid = user_lookup(name);
        if (uid < 0) { fprintf(stderr, "ps: error: user name does not exist\n"); exit(1); }
        if (g_nuids < LIST_MAX) g_uids[g_nuids++] = uid;
        s = e;
    }
}

static void add_cmd_list(const char *s) {
    g_any_selector = 1;
    while (*s) {
        while (*s == ',' || *s == ' ') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != ',' && *e != ' ') e++;
        if (g_ncmds < LIST_MAX) {
            size_t l = (size_t)(e - s); if (l >= 64) l = 63;
            memcpy(g_cmds[g_ncmds], s, l); g_cmds[g_ncmds][l] = '\0';
            g_ncmds++;
        }
        s = e;
    }
}

static int in_list(int v, const int *arr, int n) {
    for (int i = 0; i < n; i++) if (arr[i] == v) return 1;
    return 0;
}

static int selected(const struct pinfo *pi) {
    if (!g_show_threads && pi->tgid && pi->tgid != pi->pid) return 0;
    if (g_all) return 1;
    if (g_any_selector) {
        /* procps: the selection lists are OR'd together */
        if (g_npids  && in_list(pi->pid,  g_pids,  g_npids))  return 1;
        if (g_nppids && in_list(pi->ppid, g_ppids, g_nppids)) return 1;
        if (g_nuids  && in_list(pi->uid,  g_uids,  g_nuids))  return 1;
        for (int i = 0; i < g_ncmds; i++)
            if (strcmp(g_cmds[i], pi->comm) == 0) return 1;
        return 0;
    }
    /* the default: my processes, on my "terminal" */
    return pi->uid == g_self_uid && pi->session == g_self_session;
}

/* ---- sorting ------------------------------------------------------- */

static int cmp_procs(const void *a, const void *b) {
    const struct pinfo *x = a, *y = b;
    if (g_sort_col < 0) return x->pid - y->pid;
    char sx[64], sy[64];
    render(x, g_sort_col, sx, sizeof sx);
    render(y, g_sort_col, sy, sizeof sy);
    /* numeric when both cells are numbers (one decimal allowed: %cpu), else
     * lexical -- the same rule procps applies per column type */
    int r;
    char *ex, *ey;
    long dx = strtol(sx, &ex, 10) * 10, dy = strtol(sy, &ey, 10) * 10;
    if (*ex == '.' && ex[1] >= '0' && ex[1] <= '9') { dx += ex[1] - '0'; ex += 2; }
    if (*ey == '.' && ey[1] >= '0' && ey[1] <= '9') { dy += ey[1] - '0'; ey += 2; }
    if (*ex == '\0' && *ey == '\0' && ex != sx && ey != sy) r = dx < dy ? -1 : dx > dy ? 1 : 0;
    else r = strcmp(sx, sy);
    if (r == 0) r = x->pid - y->pid;
    return g_sort_desc ? -r : r;
}

/* ---- output -------------------------------------------------------- */

static void print_row(const struct pinfo *pi) {
    for (int i = 0; i < g_nout; i++) {
        struct outcol *o = &g_out[i];
        char cell[600];
        if (pi) render(pi, o->c->id, cell, sizeof cell);
        else snprintf(cell, sizeof cell, "%s", o->header);
        if (i) putchar(' ');
        if (o->width == 0 || i == g_nout - 1) {
            /* the last column is never padded on the right */
            if (o->c->right && o->width) printf("%*s", o->width, cell);
            else fputs(cell, stdout);
        } else if (o->c->right) {
            printf("%*s", o->width, cell);
        } else {
            printf("%-*s", o->width, cell);
        }
    }
    putchar('\n');
}

static void usage(FILE *f) {
    fputs("Usage:\n"
          " ps [options]\n\n"
          " Basic options:\n"
          "  -A, -e               all processes\n"
          "  -a                   all with tty, except session leaders\n"
          "   a                   all with tty, including other users\n"
          "  -d                   all except session leaders\n"
          "   x                   processes without controlling ttys\n\n"
          " Selection by list:\n"
          "  -C <command>         command name\n"
          "  -p, p, --pid <PID>   process id\n"
          "        --ppid <PID>   parent process id\n"
          "  -u, U, --user <UID>  effective user id or name\n\n"
          " Output formats:\n"
          "  -f                   full-format, including command lines\n"
          "  -l                   long format\n"
          "   u                   user-oriented format\n"
          "  -o, o, --format <format>\n"
          "                       user-defined format\n"
          "  -L, -T, H            show threads\n"
          "  -w, w                unlimited output width\n"
          "  -h, --no-headers     do not print header\n"
          "      --sort <spec>    specify sorting order\n\n"
          " Misc:\n"
          "  --help, --version\n", f);
}

int main(int argc, char **argv) {
    int fmt_f = 0, fmt_l = 0, fmt_u = 0, fmt_x = 0, bsd_a = 0, unix_a = 0;
    int custom = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--help")) { usage(stdout); return 0; }
        if (!strcmp(a, "--version") || !strcmp(a, "-V") || !strcmp(a, "V")) {
            printf("ps from procps-ng (tobyOS) 1.0\n"); return 0;
        }
        if (!strcmp(a, "--no-headers") || !strcmp(a, "--no-heading") ||
            !strcmp(a, "--noheader") || !strcmp(a, "--no-header")) { g_no_header = 1; continue; }
        if (!strcmp(a, "--format") || !strcmp(a, "-o") || !strcmp(a, "o") || !strcmp(a, "--pid") ||
            !strcmp(a, "-p") || !strcmp(a, "p") || !strcmp(a, "--ppid") || !strcmp(a, "-C") ||
            !strcmp(a, "-u") || !strcmp(a, "U") || !strcmp(a, "-U") || !strcmp(a, "--user") ||
            !strcmp(a, "--sort") || !strcmp(a, "-q") || !strcmp(a, "--quick-pid")) {
            if (i + 1 >= argc) { fprintf(stderr, "ps: error: option requires an argument -- '%s'\n", a); usage(stderr); return 1; }
            const char *v = argv[++i];
            if (!strcmp(a, "--format") || !strcmp(a, "-o") || !strcmp(a, "o")) { add_format(v); custom = 1; }
            else if (!strcmp(a, "--pid") || !strcmp(a, "-p") || !strcmp(a, "p") || !strcmp(a, "-q") || !strcmp(a, "--quick-pid")) add_int_list(v, g_pids, &g_npids);
            else if (!strcmp(a, "--ppid")) add_int_list(v, g_ppids, &g_nppids);
            else if (!strcmp(a, "-C")) add_cmd_list(v);
            else if (!strcmp(a, "--sort")) {
                const char *s = v;
                if (*s == '-') { g_sort_desc = 1; s++; } else if (*s == '+') s++;
                const struct col *c = find_col(s, strlen(s));
                if (!c) { fprintf(stderr, "ps: error: unknown sort specifier \"%s\"\n", s); return 1; }
                g_sort_col = c->id;
            }
            else add_user_list(v);
            continue;
        }
        if (!strncmp(a, "--format=", 9)) { add_format(a + 9); custom = 1; continue; }
        if (!strncmp(a, "--pid=", 6))    { add_int_list(a + 6, g_pids, &g_npids); continue; }
        if (!strncmp(a, "--ppid=", 7))   { add_int_list(a + 7, g_ppids, &g_nppids); continue; }
        if (!strncmp(a, "--user=", 7))   { add_user_list(a + 7); continue; }
        if (!strncmp(a, "--sort=", 7))   { argv[i] = (char *)"--sort"; /* handled above */
            const char *s = a + 7;
            if (*s == '-') { g_sort_desc = 1; s++; } else if (*s == '+') s++;
            const struct col *c = find_col(s, strlen(s));
            if (!c) { fprintf(stderr, "ps: error: unknown sort specifier \"%s\"\n", s); return 1; }
            g_sort_col = c->id;
            continue;
        }
        if (a[0] == '-' && a[1] == '-') {
            if (!strcmp(a, "--forest") || !strcmp(a, "--cols") || !strcmp(a, "--width") ||
                !strcmp(a, "--lines") || !strcmp(a, "--rows") || !strcmp(a, "--headers")) continue;
            fprintf(stderr, "ps: error: unsupported option '%s'\n", a);
            usage(stderr);
            return 1;
        }
        /* -o pid,comm glued: -opid; and BSD/Unix letter clusters */
        const char *p = a;
        int unix_style = 0;
        if (*p == '-') { unix_style = 1; p++; }
        if (!*p) { fprintf(stderr, "ps: error: garbage option\n"); usage(stderr); return 1; }
        for (; *p; p++) {
            switch (*p) {
            case 'e': if (unix_style) g_all = 1; /* BSD e = environment: ignored */ break;
            case 'A': g_all = 1; break;
            case 'a': if (unix_style) unix_a = 1; else bsd_a = 1; break;
            case 'd': unix_a = 1; break;
            case 'x': fmt_x = 1; break;
            case 'u': if (unix_style) {
                          if (p[1]) { add_user_list(p + 1); p += strlen(p) - 1; }
                          else if (i + 1 < argc) add_user_list(argv[++i]);
                      } else fmt_u = 1;
                      break;
            case 'U': if (p[1]) { add_user_list(p + 1); p += strlen(p) - 1; }
                      else if (i + 1 < argc) add_user_list(argv[++i]);
                      break;
            case 'f': if (unix_style) fmt_f = 1; /* BSD f = forest: ignored */ break;
            case 'l': fmt_l = 1; break;
            case 'j': fmt_l = 2; break;
            case 'w': break;
            case 'h': g_no_header = 1; break;
            case 'H': if (unix_style) { /* forest */ } else g_show_threads = 1; break;
            case 'L': case 'T': case 'm': g_show_threads = 1; break;
            case 'r': break;
            case 'c': break;
            case 'N': break;
            case 'o': if (p[1]) { add_format(p + 1); custom = 1; p += strlen(p) - 1; }
                      else if (i + 1 < argc) { add_format(argv[++i]); custom = 1; }
                      break;
            case 'p': if (p[1]) { add_int_list(p + 1, g_pids, &g_npids); p += strlen(p) - 1; }
                      else if (i + 1 < argc) add_int_list(argv[++i], g_pids, &g_npids);
                      break;
            case 'C': if (p[1]) { add_cmd_list(p + 1); p += strlen(p) - 1; }
                      else if (i + 1 < argc) add_cmd_list(argv[++i]);
                      break;
            case 'q': if (p[1]) { add_int_list(p + 1, g_pids, &g_npids); p += strlen(p) - 1; }
                      else if (i + 1 < argc) add_int_list(argv[++i], g_pids, &g_npids);
                      break;
            case 't': case 's': case 'g': case 'G':
                      /* tty / session / group selectors: no data to select on */
                      if (!p[1] && i + 1 < argc) i++;
                      p += strlen(p) - 1;
                      break;
            default:
                if (*p >= '0' && *p <= '9' && !unix_style) {
                    /* BSD: a bare number is a pid */
                    add_int_list(p, g_pids, &g_npids);
                    p += strlen(p) - 1;
                    break;
                }
                fprintf(stderr, "ps: error: unsupported option (BSD syntax)\n");
                usage(stderr);
                return 1;
            }
        }
    }

    /* BSD a/x: 'a' lifts the "my uid" filter, 'x' lifts the "my tty" filter;
     * together they are everything. Unix -a/-d: everything with a tty
     * (except session leaders) -- there is no tty record, so everything. */
    if ((bsd_a && fmt_x) || unix_a) g_all = 1;
    else if (bsd_a || fmt_x) {
        /* one of them: widen the default to the whole session / the whole
         * user, which is the honest reading of each */
        g_all = 1;
    }

    if (!custom) {
        if (fmt_u) {
            add_format("user,pid,%cpu,%mem,vsz,rss,tty,stat,start,bsdtime,args");
        } else if (fmt_l == 1) {
            add_format("f,s,uid,pid,ppid,c,pri,ni,addr,sz,wchan,tty,time,cmd");
        } else if (fmt_l == 2) {
            add_format("pid,pgid,sid,tty,time,cmd");
        } else if (fmt_f) {
            add_format("user=UID,pid,ppid,c,stime,tty,time,cmd");
        } else if (bsd_a || fmt_x) {
            add_format("pid,tty,stat,bsdtime,args");
        } else {
            add_format("pid,tty,time,cmd");
        }
        /* the tty column is TTY, not TT, in these classic layouts */
        for (int i = 0; i < g_nout; i++)
            if (g_out[i].c->id == C_TTY && !g_out[i].fixed_header)
                snprintf(g_out[i].header, sizeof g_out[i].header, "TTY");
    }
    if (g_show_threads && custom == 0) {
        /* -L: insert LWP after PID, as procps does */
        for (int i = 0; i < g_nout; i++) {
            if (g_out[i].c->id == C_PID && g_nout < 48) {
                memmove(&g_out[i + 2], &g_out[i + 1], (size_t)(g_nout - i - 1) * sizeof g_out[0]);
                g_nout++;
                g_out[i + 1].c = find_col("lwp", 3);
                g_out[i + 1].width = g_out[i + 1].c->width;
                g_out[i + 1].fixed_header = 0;
                snprintf(g_out[i + 1].header, sizeof g_out[i + 1].header, "LWP");
                break;
            }
        }
    }

    read_system();
    g_self_pid = getpid();
    g_self_uid = (int)geteuid();
    {
        struct pinfo me;
        memset(&me, 0, sizeof me);
        if (read_stat(g_self_pid, &me) == 0) g_self_session = me.session;
    }
    read_procs();
    qsort(g_procs, (size_t)g_nprocs, sizeof g_procs[0], cmp_procs);

    /* a header is printed unless every column was given an empty one */
    int any_header = 0;
    for (int i = 0; i < g_nout; i++) if (g_out[i].header[0]) any_header = 1;
    if (any_header && !g_no_header) print_row(0);

    int shown = 0;
    for (int i = 0; i < g_nprocs; i++) {
        if (!selected(&g_procs[i])) continue;
        print_row(&g_procs[i]);
        shown++;
    }
    return shown ? 0 : 1;
}
