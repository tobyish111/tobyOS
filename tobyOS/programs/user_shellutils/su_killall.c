/* su_killall.c -- killall, as psmisc 23.7's (logs/utilgate/killall.sh):
 * killall [OPTION]... [--] NAME..., killall -l, killall -V.
 *
 * Sends a signal (TERM unless told) to every process called NAME. What
 * was measured, every case in a private pid namespace of its own:
 *
 * THE NAME. A process's name is the one in /proc/PID/stat, which the
 * kernel cuts to 15 characters; for a 15-character one the command line's
 * first word, or its second (a script run by its interpreter), stands for
 * the whole name -- its last path component -- when it starts with those
 * 15; else the whole name is unknown, the process
 * matches on its first 15 characters, and -e skips it ("killall: skipping
 * partial match NAME(PID)" under -v, whatever the NAMEs were). A NAME
 * with a '/' in it is a FILE: it is stat()ed before anything is sent
 * ("NAME: No such file or directory", status 1, nothing killed) and
 * matches the processes running that very file (/proc/PID/exe, by device
 * and inode -- a hard link or symbolic link to it matches, a copy does
 * not). -I ignores case; -r makes each NAME an extended regex ("killall:
 * Bad regular expression: NAME"), matched against the name. A process
 * matched by one NAME is not matched again by a later one. killall never
 * signals itself.
 *
 * NARROWING: -u USER (a NAME in /etc/passwd only -- "Cannot find user
 * X"; the last -u wins), -o/-y TIME older/younger than TIME (a number > 0
 * and a unit straight after it: s m h d w M y; what follows the unit is
 * ignored; else "Invalid time format" and the usage), -n PID in the same
 * pid namespace as PID (a PID that cannot be read narrows nothing; not a
 * number: "Invalid namespace PID" and the usage), -Z REGEX by security
 * context (/proc/PID/attr/current; none readable, no match; a bad REGEX
 * is "Bad regular expression: X"). With -u or -Z, NAMEs may be left out:
 * then everything that passes is signalled.
 *
 * SIGNALS: -s SIG, --signal SIG, or -SIG as an argument of its own: a
 * number (as atoi reads one, so "9x" is 9 and "0x9" is 0 -- the kernel
 * refuses what is out of range: "NAME(PID): Invalid argument"), or one of
 * the names -l lists, case and all, with or without "SIG" ("SIGKILL";
 * "kill", "IOT", "CLD" and "SIG9" are "X: unknown signal; killall -l
 * lists signals."). psmisc's reading of its arguments is kept, oddities
 * and all: an argument starting with '-' and an upper-case letter or a
 * digit is a signal -- except exactly "-I" and "-V", and -Z with its
 * REGEX -- so "-Iv" is the signal "Iv"; an -I or -V that ends a cluster
 * ("-qI KXA") takes the NEXT argument, less its first character, as the
 * signal ("XA"); other capitals in a cluster ("-vL") are an unknown
 * option. Options take effect in order: -l and -V answer at once, and the
 * first error ends it.
 *
 * WHAT IS SAID (stderr): -v "Killed NAME(PID) with signal N", with -g
 * "Killed NAME(pgid N) with signal N"; a refusal "NAME(PID): strerror"
 * (even under -q); after the walk, "NAME: no process found" for each NAME
 * that killed nothing (not under -q). -i asks on stdout, "Kill NAME(PID)
 * ? (y/N) " -- "Signal ..." for any signal but TERM -- and reads a line:
 * y or Y first is yes; n, N, an empty line or the end of input is no;
 * anything else asks again. -g signals each process group once (a group
 * this kernel shows as 0 is skipped). -w waits until everything signalled
 * is gone. The usage goes to stderr with status 1 -- for an unknown
 * option, a missing argument, no NAME, and --help alike; -V's version
 * line goes to stderr too. Status 0 when every NAME killed something
 * (with no NAMEs, when anything was), else 1.
 *
 * The old killall knew five signal names, matched /proc/PID/status's name
 * whole, and took every option as nothing. */

#define KA_MAXN 4096

static const char *const ka_signames[] = {
    0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL",
    "USR1", "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT",
    "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF",
    "WINCH", "POLL", "PWR", "SYS",
};
#define KA_NSIG ((int)(sizeof ka_signames / sizeof ka_signames[0]))

static void ka_usage(void) __attribute__((noreturn));
static void ka_usage(void) {
    fflush(stdout);
    fprintf(stderr,
            "Usage: killall [OPTION]... [--] NAME...\n"
            "       killall -l, --list\n"
            "       killall -V, --version\n"
            "\n"
            "  -e,--exact          skip a process whose long name is cut short\n"
            "  -I,--ignore-case    match names in any case\n"
            "  -g,--process-group  signal the process group, not the process\n"
            "  -y,--younger-than   only processes younger than TIME\n"
            "  -o,--older-than     only processes older than TIME\n"
            "  -i,--interactive    ask before each one\n"
            "  -l,--list           list the signal names\n"
            "  -q,--quiet          say nothing when no process is found\n"
            "  -r,--regexp         take each NAME as an extended regular expression\n"
            "  -s,--signal SIGNAL  send SIGNAL (TERM if not given)\n"
            "  -u,--user USER      only processes running as USER\n"
            "  -v,--verbose        say what was signalled\n"
            "  -V,--version        show the version\n"
            "  -w,--wait           wait until the processes are gone\n"
            "  -n,--ns PID         only processes in PID's pid namespace\n"
            "  -Z,--context REGEX  only processes whose security context matches\n"
            "\n");
    exit(1);
}

/* SPEC (an argument less its '-') as a signal, or exit 1 */
static int ka_signal(const char *spec) {
    if (isdigit((unsigned char)*spec)) return (int)strtol(spec, 0, 10);
    const char *s = strncmp(spec, "SIG", 3) == 0 ? spec + 3 : spec;
    for (int i = 1; i < KA_NSIG; i++)
        if (!strcmp(s, ka_signames[i])) return i;
    fflush(stdout);
    fprintf(stderr, "%s: unknown signal; killall -l lists signals.\n", s);
    exit(1);
}

/* TIME for -o/-y, in seconds; or "Invalid time format" and the usage */
static unsigned long long ka_time(const char *s) {
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    unsigned long long m = 0;
    if (e != s && errno != ERANGE && v > 0) {
        switch (*e) {
        case 's': m = 1; break;
        case 'm': m = 60; break;
        case 'h': m = 3600; break;
        case 'd': m = 86400; break;
        case 'w': m = 604800; break;
        case 'M': m = 2592000; break;       /* 30 days: not measurable here */
        case 'y': m = 31536000; break;      /* 365 days: nor this */
        }
    }
    if (!m) {
        fflush(stdout);
        fprintf(stderr, "Invalid time format\n");
        ka_usage();
    }
    unsigned long long r = (unsigned long long)v * m;
    return r / m == (unsigned long long)v ? r : ~0ULL;
}

static char *ka_slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    size_t cap = 4096, n = 0;
    char *b = (char *)malloc(cap + 1);
    while (b) {
        ssize_t r = read(fd, b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n == cap) {
            cap *= 2;
            char *nb = (char *)realloc(b, cap + 1);
            if (!nb) { free(b); b = 0; break; }
            b = nb;
        }
    }
    close(fd);
    if (!b) return 0;
    b[n] = 0;
    if (len) *len = n;
    return b;
}

/* /proc/uptime in hundredths of a second (no floating point here) */
static unsigned long long ka_uptime(void) {
    char *u = ka_slurp("/proc/uptime", 0);
    unsigned long long v = 0;
    if (u) {
        const char *p = u;
        while (isdigit((unsigned char)*p)) v = v * 10 + (unsigned)(*p++ - '0');
        v *= 100;
        if (*p == '.') {
            p++;
            if (isdigit((unsigned char)*p)) { v += (unsigned)(*p++ - '0') * 10; }
            if (isdigit((unsigned char)*p)) { v += (unsigned)(*p - '0'); }
        }
        free(u);
    }
    return v;
}

struct ka_proc {
    int pid, pgrp;
    unsigned long long start;   /* clock ticks since boot */
    long euid;
    char comm[64];
    char full[256];             /* the whole name, when known */
    int partial;                /* a 15-character name, the whole unknown */
};

/* /proc/PID into P; 0, or -1 when it is gone */
static int ka_read(int pid, struct ka_proc *p) {
    char path[64];
    memset(p, 0, sizeof *p);
    p->pid = pid;
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char *st = ka_slurp(path, 0);
    if (!st) return -1;
    char *lp = strchr(st, '('), *rp = strrchr(st, ')');
    if (!lp || !rp || rp < lp) { free(st); return -1; }
    snprintf(p->comm, sizeof p->comm, "%.*s", (int)(rp - lp - 1), lp + 1);
    /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt
     * majflt cmajflt utime stime cutime cstime priority nice threads
     * itrealvalue starttime */
    char *f[20];
    int nf = 0;
    for (char *t = rp + 1; nf < 20; ) {
        while (*t == ' ') t++;
        if (!*t) break;
        f[nf++] = t;
        while (*t && *t != ' ') t++;
    }
    if (nf >= 3) p->pgrp = atoi(f[2]);
    if (nf >= 20) p->start = strtoull(f[19], 0, 10);
    free(st);
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    char *ss = ka_slurp(path, 0);
    p->euid = -1;
    if (ss) {
        for (char *l = ss; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
            if (!strncmp(l, "Uid:", 4)) {
                char *e;
                strtol(l + 4, &e, 10);
                p->euid = strtol(e, 0, 10);
                break;
            }
        }
        free(ss);
    }
    snprintf(p->full, sizeof p->full, "%s", p->comm);
    if (strlen(p->comm) == 15) {
        /* the kernel's cut: the command line's first word -- or, for a
         * script ("/bin/sh ./name"), its second -- may hold the rest, as
         * the last component of a path */
        snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
        size_t len = 0;
        char *c = ka_slurp(path, &len);
        p->partial = 1;
        for (size_t off = 0, k = 0; c && off < len && k < 2; k++) {
            const char *arg = c + off, *base = strrchr(arg, '/');
            base = base ? base + 1 : arg;
            if (strncmp(base, p->comm, 15) == 0) {
                snprintf(p->full, sizeof p->full, "%s", base);
                p->partial = 0;
                break;
            }
            off += strlen(arg) + 1;
        }
        free(c);
    }
    return 0;
}

/* the pid namespace PID is in, or "" when it cannot be read */
static void ka_pidns(int pid, char *out, size_t cap) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/ns/pid", pid);
    ssize_t n = readlink(path, out, cap - 1);
    out[n > 0 ? n : 0] = 0;
}

static int ka_cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

/* is PID (or, for -g, group -PID) still there? A zombie is gone. */
static int ka_alive(int pid) {
    if (kill(pid, 0) != 0 && errno != EPERM) return 0;
    if (pid < 0) return 1;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char *st = ka_slurp(path, 0);
    if (!st) return 0;
    char *rp = strrchr(st, ')');
    int zombie = rp && rp[1] == ' ' && rp[2] == 'Z';
    free(st);
    return !zombie;
}

/* -i: ask, and read one line's answer */
static int ka_ask(const char *verb, const char *name, const char *what, int id) {
    for (;;) {
        printf("%s %s(%s%d) ? (y/N) ", verb, name, what, id);
        fflush(stdout);
        int c = getchar(), first = c;
        while (c != EOF && c != '\n') c = getchar();
        if (first == 'y' || first == 'Y') return 1;
        if (first == 'n' || first == 'N' || first == '\n' || first == EOF) return 0;
    }
}

static int do_killall(int argc, char **argv) {
    enum { KA_E = 1, KA_I, KA_G, KA_Y, KA_O, KA_INT, KA_L, KA_Q, KA_R, KA_S,
           KA_U, KA_V, KA_VER, KA_W, KA_N, KA_Z };
    static const struct { const char *name; int arg, val; } longs[] = {
        { "exact", 0, KA_E }, { "ignore-case", 0, KA_I },
        { "process-group", 0, KA_G }, { "younger-than", 1, KA_Y },
        { "older-than", 1, KA_O }, { "interactive", 0, KA_INT },
        { "list", 0, KA_L }, { "quiet", 0, KA_Q }, { "regexp", 0, KA_R },
        { "signal", 1, KA_S }, { "user", 1, KA_U }, { "verbose", 0, KA_V },
        { "version", 0, KA_VER }, { "wait", 0, KA_W }, { "ns", 1, KA_N },
        { "context", 1, KA_Z },
    };
    const int nlongs = (int)(sizeof longs / sizeof longs[0]);
    int sig = SIGTERM, exact = 0, icase = 0, group = 0, ask = 0, quiet = 0;
    int regex = 0, verbose = 0, waitfor = 0, have_user = 0, ns_pid = 0, have_ns = 0;
    long uid = -1;
    unsigned long long younger = 0, older = 0;
    const char *context = 0;
    char **names = (char **)malloc((size_t)argc * sizeof *names);
    int nn = 0;
    if (!names) su_error(1, ENOMEM, "memory exhausted");

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) {
            while (++i < argc) names[nn++] = argv[i];
            break;
        }
        if (a[0] != '-' || !a[1]) { names[nn++] = a; continue; }
        int opt = 0;
        const char *val = 0;
        if (a[1] == '-') {
            /* a long option: a unique prefix, or the whole name */
            const char *nm = a + 2, *eq = strchr(nm, '=');
            size_t len = eq ? (size_t)(eq - nm) : strlen(nm);
            int found = -1, hits = 0;
            for (int k = 0; k < nlongs; k++) {
                if (strncmp(longs[k].name, nm, len)) continue;
                if (strlen(longs[k].name) == len) { found = k; hits = 1; break; }
                if (found < 0) found = k;
                hits++;
            }
            if (found < 0 || hits > 1) ka_usage();
            if (longs[found].arg) {
                if (eq) val = eq + 1;
                else if (i + 1 < argc) val = argv[++i];
                else ka_usage();
            } else if (eq) ka_usage();
            opt = longs[found].val;
        } else if (isupper((unsigned char)a[1]) || isdigit((unsigned char)a[1])) {
            if (!strcmp(a, "-I")) opt = KA_I;
            else if (!strcmp(a, "-V")) opt = KA_VER;
            else if (a[1] == 'Z') {
                if (a[2]) val = a + 2;
                else if (i + 1 < argc) val = argv[++i];
                else ka_signal("Z");
                opt = KA_Z;
            } else {
                sig = ka_signal(a + 1);
                continue;
            }
        }
        if (!opt) {
            /* a cluster of short options */
            for (int j = 1; a[j]; j++) {
                int c = (unsigned char)a[j], o = 0;
                switch (c) {
                case 'e': o = KA_E; break;
                case 'g': o = KA_G; break;
                case 'i': o = KA_INT; break;
                case 'l': o = KA_L; break;
                case 'q': o = KA_Q; break;
                case 'r': o = KA_R; break;
                case 'v': o = KA_V; break;
                case 'w': o = KA_W; break;
                case 's': o = KA_S; break;
                case 'u': o = KA_U; break;
                case 'o': o = KA_O; break;
                case 'y': o = KA_Y; break;
                case 'n': o = KA_N; break;
                case 'Z': o = KA_Z; break;
                case 'I': case 'V':
                    /* psmisc takes the signal from the argument after an
                     * -I or -V that ends a cluster, from this one else */
                    if (a[j + 1]) sig = ka_signal(a + 1);
                    else if (i + 1 < argc) sig = ka_signal(argv[i + 1] + 1);
                    else ka_usage();
                    goto next_arg;
                default: ka_usage();
                }
                if (o == KA_S || o == KA_U || o == KA_O || o == KA_Y || o == KA_N || o == KA_Z) {
                    if (a[j + 1]) val = a + j + 1;
                    else if (i + 1 < argc) val = argv[++i];
                    else ka_usage();
                    opt = o;
                    break;
                }
                if (o == KA_L) { opt = o; break; }
                switch (o) {
                case KA_E: exact = 1; break;
                case KA_G: group = 1; break;
                case KA_INT: ask = 1; break;
                case KA_Q: quiet = 1; break;
                case KA_R: regex = 1; break;
                case KA_V: verbose = 1; break;
                case KA_W: waitfor = 1; break;
                }
            }
            if (!opt) continue;
        }
        switch (opt) {
        case KA_E: exact = 1; break;
        case KA_I: icase = 1; break;
        case KA_G: group = 1; break;
        case KA_INT: ask = 1; break;
        case KA_Q: quiet = 1; break;
        case KA_R: regex = 1; break;
        case KA_V: verbose = 1; break;
        case KA_W: waitfor = 1; break;
        case KA_L:
            for (int k = 1; k < KA_NSIG; k++)
                printf("%s%c", ka_signames[k], k == 16 || k == KA_NSIG - 1 ? '\n' : ' ');
            return 0;
        case KA_VER:
            fflush(stdout);
            fprintf(stderr, "killall (tobyOS) 1.0\n");
            return 0;
        case KA_S: sig = ka_signal(val); break;
        case KA_U:
            uid = cg_db_id("/etc/passwd", val);
            if (uid < 0) {
                fflush(stdout);
                fprintf(stderr, "Cannot find user %s\n", val);
                return 1;
            }
            have_user = 1;
            break;
        case KA_O: older = ka_time(val); break;
        case KA_Y: younger = ka_time(val); break;
        case KA_N: {
            char *e;
            long v = strtol(val, &e, 10);
            if (e == val) {
                fflush(stdout);
                fprintf(stderr, "Invalid namespace PID\n");
                ka_usage();
            }
            ns_pid = (int)v;
            have_ns = 1;
            break;
        }
        case KA_Z: context = val; break;
        }
next_arg:;
    }
    if (!nn && !have_user && !context) ka_usage();

    regex_t zre;
    if (context && regcomp(&zre, context, REG_EXTENDED | REG_NOSUB) != 0) {
        fflush(stdout);
        fprintf(stderr, "Bad regular expression: %s\n", context);
        return 1;
    }
    regex_t *res = 0;
    struct stat *files = 0;
    char *isfile = (char *)calloc((size_t)nn + 1, 1);
    if (regex) {
        res = (regex_t *)malloc(((size_t)nn + 1) * sizeof *res);
        for (int k = 0; k < nn; k++) {
            if (regcomp(&res[k], names[k], REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0)) != 0) {
                fflush(stdout);
                fprintf(stderr, "killall: Bad regular expression: %s\n", names[k]);
                return 1;
            }
        }
    } else {
        files = (struct stat *)malloc(((size_t)nn + 1) * sizeof *files);
        for (int k = 0; k < nn; k++) {
            if (!strchr(names[k], '/')) continue;
            if (stat(names[k], &files[k]) != 0) {
                int e = errno;
                fflush(stdout);
                fprintf(stderr, "%s: %s\n", names[k], strerror(e));
                return 1;
            }
            isfile[k] = 1;
        }
    }
    char nsref[128] = "";
    if (have_ns) ka_pidns(ns_pid, nsref, sizeof nsref);
    unsigned long long now = (older || younger) ? ka_uptime() : 0;

    /* the pids /proc lists, in order */
    int *pids = 0, npids = 0, pcap = 0;
    DIR *dp = opendir("/proc");
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            char *e;
            long v = strtol(de->d_name, &e, 10);
            if (!*de->d_name || *e || v <= 0) continue;
            if (npids == pcap) {
                pcap = pcap ? pcap * 2 : 256;
                pids = (int *)realloc(pids, (size_t)pcap * sizeof *pids);
                if (!pids) su_error(1, ENOMEM, "memory exhausted");
            }
            pids[npids++] = (int)v;
        }
        closedir(dp);
    }
    if (npids) qsort(pids, (size_t)npids, sizeof *pids, ka_cmp_int);

    int me = getpid(), killed_any = 0;
    int *found = (int *)calloc((size_t)nn + 1, sizeof *found);
    int *done = (int *)malloc(((size_t)npids + 1) * sizeof *done), ndone = 0;
    int *pgdone = (int *)malloc(((size_t)npids + 1) * sizeof *pgdone), npg = 0;
    for (int i = 0; i < npids; i++) {
        if (pids[i] == me) continue;
        struct ka_proc p;
        if (ka_read(pids[i], &p) != 0) continue;
        if (have_user && p.euid != uid) continue;
        if (older || younger) {
            unsigned long long age = now > p.start ? (now - p.start) / 100 : 0;
            if (older && age < older) continue;
            if (younger && age >= younger) continue;
        }
        if (context) {
            char path[64];
            snprintf(path, sizeof path, "/proc/%d/attr/current", p.pid);
            char *cx = ka_slurp(path, 0);
            int ok = cx && regexec(&zre, cx, 0, 0, 0) == 0;
            free(cx);
            if (!ok) continue;
        }
        if (have_ns && nsref[0]) {
            char mine[128];
            ka_pidns(p.pid, mine, sizeof mine);
            if (strcmp(mine, nsref)) continue;
        }
        if (exact && p.partial) {
            if (verbose) {
                fflush(stdout);
                fprintf(stderr, "killall: skipping partial match %s(%d)\n", p.comm, p.pid);
            }
            continue;
        }
        const char *shown = p.partial ? p.comm : p.full;
        int which = -1;
        if (!nn) which = KA_MAXN;
        struct stat ex;
        int have_ex = -1;
        for (int k = 0; k < nn && which < 0; k++) {
            if (regex) {
                if (regexec(&res[k], shown, 0, 0, 0) == 0) which = k;
            } else if (isfile[k]) {
                if (have_ex < 0) {
                    char path[64];
                    snprintf(path, sizeof path, "/proc/%d/exe", p.pid);
                    /* Linux's stat follows the link to the running file; a
                     * /proc whose stat does not is asked by readlink */
                    have_ex = stat(path, &ex) == 0 && !S_ISLNK(ex.st_mode);
                    if (!have_ex) {
                        char lk[4096];
                        ssize_t ln = readlink(path, lk, sizeof lk - 1);
                        if (ln > 0) {
                            lk[ln] = 0;
                            have_ex = stat(lk, &ex) == 0;
                        }
                    }
                }
                if (have_ex && ex.st_dev == files[k].st_dev && ex.st_ino == files[k].st_ino)
                    which = k;
            } else if (p.partial) {
                if ((icase ? strncasecmp(p.comm, names[k], 15) : strncmp(p.comm, names[k], 15)) == 0)
                    which = k;
            } else if ((icase ? strcasecmp(p.full, names[k]) : strcmp(p.full, names[k])) == 0) {
                which = k;
            }
        }
        if (which < 0) continue;
        int target = p.pid;
        if (group) {
            if (p.pgrp <= 0) continue;
            int seen = 0;
            for (int k = 0; k < npg; k++) if (pgdone[k] == p.pgrp) seen = 1;
            if (seen) {
                if (which != KA_MAXN) found[which] = 1;
                continue;
            }
            target = -p.pgrp;
        }
        if (ask && !ka_ask(sig == SIGTERM ? "Kill" : "Signal", shown, group ? "pgid " : "",
                           group ? p.pgrp : p.pid))
            continue;
        if (kill(target, sig) == 0) {
            if (verbose) {
                fflush(stdout);
                fprintf(stderr, "Killed %s(%s%d) with signal %d\n", shown,
                        group ? "pgid " : "", group ? p.pgrp : p.pid, sig);
            }
            if (which != KA_MAXN) found[which] = 1;
            killed_any = 1;
            if (group) pgdone[npg++] = p.pgrp;
            done[ndone++] = target;
        } else {
            int e = errno;
            fflush(stdout);
            fprintf(stderr, "%s(%d): %s\n", shown, p.pid, strerror(e));
        }
    }
    int rc = 0;
    if (!nn) rc = killed_any ? 0 : 1;
    for (int k = 0; k < nn; k++) {
        if (found[k]) continue;
        rc = 1;
        if (!quiet) {
            fflush(stdout);
            fprintf(stderr, "%s: no process found\n", names[k]);
        }
    }
    if (waitfor) {
        for (;;) {
            int left = 0;
            for (int k = 0; k < ndone; k++) {
                if (!done[k]) continue;
                if (ka_alive(done[k])) left = 1;
                else done[k] = 0;
            }
            if (!left) break;
            usleep(50000);
        }
    }
    return rc;
}
