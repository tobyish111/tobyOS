/* su_pgrep.c -- pgrep and pkill, as procps-ng 4.0.4's (logs/utilgate/pgrep.sh).
 *
 *   pgrep [options] <pattern>
 *   pkill [options] <pattern>
 *
 *   - The pattern is an extended regex (-i: any case; -x: the whole name)
 *     matched against each process's name, or with -f its whole command
 *     line; -v selects the rest. pgrep and pkill never select themselves.
 *   - Narrowed (all must hold) by -P parents, -g process groups and -s
 *     sessions (0: one's own), -u effective and -U real users, -G real
 *     groups (names or numbers, comma lists), -t terminals, -r states, -O
 *     at least SECS old, -F a pid file's pid (-L: its lock), -A not an
 *     ancestor; -n only the newest, -o only the oldest.
 *   - pgrep prints pids, one per line or joined by -d DELIM, with -l the
 *     name and -a the command line; -c the count; -w each thread.
 *   - pkill sends -SIGNAL (-9, -KILL, -SIGKILL, -usr1 ...) or --signal SIG
 *     -- TERM by default; -e says "NAME killed (pid N)".
 *   - Status 0 for a match, 1 for none, 2 for a usage error; procps's
 *     messages ("no matching criteria specified", "only one pattern can be
 *     provided", "invalid user name: X", "regex error: ...", "Unknown
 *     signal \"X\".") and usage on stderr.
 *
 * Processes come from /proc: stat (state, parent, group, session, terminal,
 * start time), status (the user and group ids), cmdline, task/ for -w. They
 * are listed by pid, as Linux's /proc lists them.
 *
 * It matched a substring of the name only, took every option as nothing,
 * and pkill understood a few signal names. */

struct pg_proc {
    int pid, ppid, pgrp, session, tty;
    char state;
    unsigned long long start;
    long ruid, euid, rgid;
    char comm[64];
    char *cmd;              /* the command line, spaces for NULs; or NULL */
};

struct pg_list { long *v; int n, cap; };

static void pg_push(struct pg_list *l, long x) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = (long *)realloc(l->v, (size_t)l->cap * sizeof *l->v);
        if (!l->v) su_error(3, ENOMEM, "memory exhausted");
    }
    l->v[l->n++] = x;
}

static int pg_in(const struct pg_list *l, long x) {
    for (int i = 0; i < l->n; i++) if (l->v[i] == x) return 1;
    return 0;
}

static int pg_kill_mode;

static void pg_usage(FILE *out) {
    fprintf(out, "\nUsage:\n %s [options] <pattern>\n\nOptions:\n", su_short);
    if (pg_kill_mode)
        fprintf(out,
                " -<sig>, --signal <sig>    the signal to send (TERM unless given)\n"
                " -e, --echo                say what is killed\n");
    else
        fprintf(out,
                " -d, --delimiter <string>  put STRING between the pids (newline)\n"
                " -l, --list-name           the name after each pid\n"
                " -a, --list-full           the command line after each pid\n"
                " -v, --inverse             the processes that do NOT match\n"
                " -w, --lightweight         each thread's id\n");
    fprintf(out,
            " -c, --count               only how many match\n"
            " -f, --full                match the whole command line\n"
            " -g, --pgroup <PGID,...>   in these process groups (0: ours)\n"
            " -G, --group <GID,...>     with these real group ids\n"
            " -i, --ignore-case         match in any case\n"
            " -n, --newest              only the newest\n"
            " -o, --oldest              only the oldest\n"
            " -O, --older <seconds>     at least this old\n"
            " -P, --parent <PPID,...>   children of these\n"
            " -s, --session <SID,...>   in these sessions (0: ours)\n"
            " -t, --terminal <tty,...>  on these terminals\n"
            " -u, --euid <ID,...>       with these effective user ids\n"
            " -U, --uid <ID,...>        with these real user ids\n"
            " -x, --exact               the whole name must match\n"
            " -F, --pidfile <file>      the pid in FILE\n"
            " -L, --logpidfile          fail unless FILE is locked\n"
            " -r, --runstates <state>   in these states (D, R, S, Z, ...)\n"
            " -A, --ignore-ancestors    not our own ancestors\n"
            "\n"
            " -h, --help     show this help and exit\n"
            " -V, --version  show the version and exit\n");
}

static void pg_try(int status) {
    fprintf(stderr, "Try `%s --help' for more information.\n", su_short);
    exit(status);
}

/* a user's or group's id from DB (/etc/passwd, /etc/group), or a number */
static long pg_id(const char *db, const char *s) {
    char *e;
    long v = strtol(s, &e, 10);
    if (*s && !*e && v >= 0) return v;
    FILE *f = fopen(db, "r");
    if (!f) return -1;
    char line[1024];
    long r = -1;
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        *c1 = 0;
        if (strcmp(line, s)) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (c2) r = strtol(c2 + 1, 0, 10);
        break;
    }
    fclose(f);
    return r;
}

static char *pg_slurp(const char *path, size_t *len);

/* our own session: /proc/self/stat's sixth field (libtoby has no getsid) */
static long pg_own_session(void) {
    char *st = pg_slurp("/proc/self/stat", 0);
    long sid = -1;
    char *rp = st ? strrchr(st, ')') : 0;
    if (rp) {
        int f = 0;
        for (char *t = rp + 1; *t && f < 4; f++) {
            while (*t == ' ') t++;
            if (f == 3) sid = strtol(t, 0, 10);
            while (*t && *t != ' ') t++;
        }
    }
    free(st);
    return sid;
}

/* a comma list into L: KIND 0 numbers ("not a number: X"), 1 users, 2
 * groups, 3 process groups (0 is ours), 4 sessions (0 is ours) */
static void pg_list_parse(struct pg_list *l, const char *arg, int kind) {
    char *copy = strdup(arg);
    if (!copy) su_error(3, ENOMEM, "memory exhausted");
    for (char *tok = copy, *next; tok; tok = next) {
        next = strchr(tok, ',');
        if (next) *next++ = 0;
        long v;
        char *e;
        switch (kind) {
        case 1:
            v = pg_id("/etc/passwd", tok);
            if (v < 0) su_error(2, 0, "invalid user name: %s", tok);
            break;
        case 2:
            v = pg_id("/etc/group", tok);
            if (v < 0) su_error(2, 0, "invalid group name: %s", tok);
            break;
        default:
            v = strtol(tok, &e, 10);
            if (!*tok || *e || v < 0) {
                if (kind == 3) su_error(2, 0, "invalid process group: %s", tok);
                if (kind == 4) su_error(2, 0, "invalid session id: %s", tok);
                su_error(2, 0, "not a number: %s", tok);
            }
            if (kind == 3 && v == 0) v = getpgrp();
            if (kind == 4 && v == 0) v = pg_own_session();
            break;
        }
        pg_push(l, v);
    }
    free(copy);
}

static char *pg_slurp(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    size_t cap = 4096, n = 0;
    char *b = (char *)malloc(cap + 1);
    for (;;) {
        if (!b) break;
        ssize_t r = read(fd, b + n, cap - n);
        if (r <= 0) break;
        n += (size_t)r;
        if (n == cap) {
            cap *= 2;
            b = (char *)realloc(b, cap + 1);
        }
    }
    close(fd);
    if (!b) return 0;
    b[n] = 0;
    if (len) *len = n;
    return b;
}

/* /proc/DIR (a pid, or PID/task/TID) into P; 0, or -1 when it is gone */
static int pg_read(const char *dir, int want_cmd, struct pg_proc *p) {
    char path[128];
    memset(p, 0, sizeof *p);
    snprintf(path, sizeof path, "/proc/%s/stat", dir);
    char *st = pg_slurp(path, 0);
    if (!st) return -1;
    char *lp = strchr(st, '('), *rp = strrchr(st, ')');
    if (!lp || !rp || rp < lp) { free(st); return -1; }
    p->pid = atoi(st);
    snprintf(p->comm, sizeof p->comm, "%.*s", (int)(rp - lp - 1), lp + 1);
    /* after ") ": state ppid pgrp session tty_nr tpgid flags minflt cminflt
     * majflt cmajflt utime stime cutime cstime priority nice num_threads
     * itrealvalue starttime */
    char *f[20];
    int nf = 0;
    for (char *t = rp + 1; nf < 20; ) {
        while (*t == ' ') t++;
        if (!*t) break;
        f[nf++] = t;
        while (*t && *t != ' ') t++;
    }
    if (nf < 20) { free(st); return -1; }
    p->state = f[0][0];
    p->ppid = atoi(f[1]);
    p->pgrp = atoi(f[2]);
    p->session = atoi(f[3]);
    p->tty = atoi(f[4]);
    p->start = strtoull(f[19], 0, 10);
    free(st);
    snprintf(path, sizeof path, "/proc/%s/status", dir);
    char *ss = pg_slurp(path, 0);
    p->ruid = p->euid = p->rgid = -1;
    if (ss) {
        for (char *l = ss; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
            if (!strncmp(l, "Uid:", 4)) {
                char *e;
                p->ruid = strtol(l + 4, &e, 10);
                p->euid = strtol(e, 0, 10);
            } else if (!strncmp(l, "Gid:", 4))
                p->rgid = strtol(l + 4, 0, 10);
        }
        free(ss);
    }
    if (want_cmd) {
        size_t n = 0;
        snprintf(path, sizeof path, "/proc/%s/cmdline", dir);
        char *c = pg_slurp(path, &n);
        if (c) {
            while (n && !c[n - 1]) n--;
            for (size_t i = 0; i < n; i++) if (!c[i]) c[i] = ' ';
            c[n] = 0;
            if (!n) { free(c); c = 0; }
        }
        p->cmd = c;
    }
    return 0;
}

/* glibc's words for a regcomp error -- procps prints regerror()'s */
static const char *pg_regerr(int code, const char *pat) {
    if (code == REG_EBRACK) {
        /* glibc: a '[' (or "[^") with nothing after it is a bad pattern;
         * one with something after it is an unmatched bracket */
        size_t n = strlen(pat);
        if ((n >= 1 && pat[n - 1] == '[') || (n >= 2 && pat[n - 2] == '[' && pat[n - 1] == '^'))
            return "Invalid regular expression";
        return "Unmatched [, [^, [:, [., or [=";
    }
    switch (code) {
    case REG_ECOLLATE: return "Invalid collation character";
    case REG_ECTYPE: return "Invalid character class name";
    case REG_EESCAPE: return "Trailing backslash";
    case REG_ESUBREG: return "Invalid back reference";
    case REG_EPAREN: return "Unmatched ( or \\(";
    case REG_EBRACE: return "Unmatched \\{";
    case REG_BADBR: return "Invalid content of \\{\\}";
    case REG_ERANGE: return "Invalid range end";
    case REG_ESPACE: return "Memory exhausted";
    case REG_BADRPT: return "Invalid preceding regular expression";
    default: return "Invalid regular expression";
    }
}

static int pg_cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return x < y ? -1 : x > y;
}

static int do_pgrep(int argc, char **argv, int do_kill) {
    enum { SIGNAL_OPT = 0x100, NS_OPT, NSLIST_OPT, CGROUP_OPT, HELP_OPT };
    static const struct su_option longs[] = {
        { "signal", SU_REQ_ARG, 0, SIGNAL_OPT }, { "count", SU_NO_ARG, 0, 'c' },
        { "delimiter", SU_REQ_ARG, 0, 'd' }, { "list-name", SU_NO_ARG, 0, 'l' },
        { "list-full", SU_NO_ARG, 0, 'a' }, { "full", SU_NO_ARG, 0, 'f' },
        { "pgroup", SU_REQ_ARG, 0, 'g' }, { "group", SU_REQ_ARG, 0, 'G' },
        { "ignore-case", SU_NO_ARG, 0, 'i' }, { "newest", SU_NO_ARG, 0, 'n' },
        { "oldest", SU_NO_ARG, 0, 'o' }, { "older", SU_REQ_ARG, 0, 'O' },
        { "parent", SU_REQ_ARG, 0, 'P' }, { "session", SU_REQ_ARG, 0, 's' },
        { "terminal", SU_REQ_ARG, 0, 't' }, { "euid", SU_REQ_ARG, 0, 'u' },
        { "uid", SU_REQ_ARG, 0, 'U' }, { "inverse", SU_NO_ARG, 0, 'v' },
        { "lightweight", SU_NO_ARG, 0, 'w' }, { "exact", SU_NO_ARG, 0, 'x' },
        { "pidfile", SU_REQ_ARG, 0, 'F' }, { "logpidfile", SU_NO_ARG, 0, 'L' },
        { "echo", SU_NO_ARG, 0, 'e' }, { "ns", SU_REQ_ARG, 0, NS_OPT },
        { "nslist", SU_REQ_ARG, 0, NSLIST_OPT }, { "cgroup", SU_REQ_ARG, 0, CGROUP_OPT },
        { "runstates", SU_REQ_ARG, 0, 'r' }, { "ignore-ancestors", SU_NO_ARG, 0, 'A' },
        { "require-handler", SU_NO_ARG, 0, 'H' }, { "queue", SU_REQ_ARG, 0, 'q' },
        { "help", SU_NO_ARG, 0, 'h' }, { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    pg_kill_mode = do_kill;
    int sig = SIGTERM;
    if (do_kill) {
        /* the first -SIGNAL anywhere on the line is the signal: procps
         * takes it out before the options are read */
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') continue;
            int s = to_signal(argv[i] + 1);
            if (s < 0 && isdigit((unsigned char)argv[i][1])) s = atoi(argv[i] + 1);
            if (s >= 0) {
                sig = s;
                for (int k = i; k < argc; k++) argv[k] = argv[k + 1];
                argc--;
                break;
            }
        }
    }
    const char *shorts = do_kill ? "eq:LF:cfinoxP:O:g:s:u:U:G:t:r:AHhV"
                                 : "lad:vwLF:cfinoxP:O:g:s:u:U:G:t:r:AHhV";
    struct pg_list parents = { 0, 0, 0 }, pgrps = { 0, 0, 0 }, sids = { 0, 0, 0 };
    struct pg_list euids = { 0, 0, 0 }, ruids = { 0, 0, 0 }, rgids = { 0, 0, 0 };
    const char *delim = "\n", *pidfile = 0, *states = 0, *ttys = 0;
    int list_name = 0, list_full = 0, inverse = 0, lightweight = 0, count = 0;
    int full = 0, icase = 0, newest = 0, oldest = 0, exact = 0, echo = 0;
    int logpid = 0, no_anc = 0, criteria = 0, c;
    long older = 0;
    while ((c = su_getopt_long(argc, argv, shorts, longs, 0)) != -1) {
        switch (c) {
        case SIGNAL_OPT: {
            int s = to_signal(su_optarg);
            if (s < 0 && isdigit((unsigned char)su_optarg[0])) s = atoi(su_optarg);
            if (s < 0) {
                fprintf(stderr, "Unknown signal \"%s\".", su_optarg);
                pg_usage(stderr);
                return 2;
            }
            sig = s;
            break;
        }
        case 'e': echo = 1; break;
        case 'q': break;
        case 'c': count = 1; break;
        case 'd': delim = su_optarg; break;
        case 'l': list_name = 1; break;
        case 'a': list_full = 1; break;
        case 'f': full = 1; break;
        case 'g': pg_list_parse(&pgrps, su_optarg, 3); criteria++; break;
        case 'G': pg_list_parse(&rgids, su_optarg, 2); criteria++; break;
        case 'i': icase = 1; break;
        case 'n': newest = 1; break;
        case 'o': oldest = 1; break;
        case 'O': older = atol(su_optarg); criteria++; break;
        case 'P': pg_list_parse(&parents, su_optarg, 0); criteria++; break;
        case 's': pg_list_parse(&sids, su_optarg, 4); criteria++; break;
        case 't': ttys = su_optarg; criteria++; break;
        case 'u': pg_list_parse(&euids, su_optarg, 1); criteria++; break;
        case 'U': pg_list_parse(&ruids, su_optarg, 1); criteria++; break;
        case 'v': inverse = 1; break;
        case 'w': lightweight = 1; break;
        case 'x': exact = 1; break;
        case 'F': pidfile = su_optarg; criteria++; break;
        case 'L': logpid = 1; break;
        case 'r': states = su_optarg; criteria++; break;
        case 'A': no_anc = 1; break;
        case 'H': break;
        case NS_OPT: case NSLIST_OPT: case CGROUP_OPT: criteria++; break;
        case 'h': pg_usage(stdout); return 0;
        case 'V': return su_version();
        default: pg_usage(stderr); return 2;
        }
    }
    if (newest && oldest) { pg_usage(stderr); return 2; }
    if (logpid && !pidfile) {
        su_error(0, 0, "-L without -F makes no sense");
        pg_try(2);
    }
    const char *pattern = 0;
    if (su_optind < argc) {
        if (argc - su_optind > 1) {
            su_error(0, 0, "only one pattern can be provided");
            pg_try(2);
        }
        pattern = argv[su_optind];
        criteria++;
    }
    if (!criteria) {
        su_error(0, 0, "no matching criteria specified");
        pg_try(2);
    }
    long file_pid = -1;
    if (pidfile) {
        size_t n = 0;
        char *b = pg_slurp(pidfile, &n);
        char *e = 0;
        if (b) file_pid = strtol(b, &e, 10);
        if (!b || e == b || file_pid <= 0 || (*e && *e != '\n')) {
            free(b);
            su_error(0, 0, "pidfile not valid");
            pg_try(1);
        }
        free(b);
    }
    regex_t re;
    int have_re = 0;
    if (pattern) {
        char *p = (char *)pattern;
        if (exact) {
            p = (char *)malloc(strlen(pattern) + 5);
            if (!p) su_error(3, ENOMEM, "memory exhausted");
            sprintf(p, "^(%s)$", pattern);
        }
        int rc = regcomp(&re, p, REG_EXTENDED | REG_NOSUB | (icase ? REG_ICASE : 0));
        if (rc) su_error(2, 0, "regex error: %s", pg_regerr(rc, pattern));
        have_re = 1;
    }
    unsigned long long now = 0;
    if (older) {
        char *u = pg_slurp("/proc/uptime", 0);
        if (u) now = (unsigned long long)(su_strtold(u, 0) * 100);
        free(u);
    }
    struct pg_list anc = { 0, 0, 0 };
    if (no_anc) {
        char d[16];
        int pid = getppid();
        while (pid > 0) {
            pg_push(&anc, pid);
            struct pg_proc q;
            snprintf(d, sizeof d, "%d", pid);
            if (pg_read(d, 0, &q) != 0 || q.ppid == pid) break;
            pid = q.ppid;
        }
    }

    /* the pids /proc lists, in Linux's order */
    int *pids = 0, npids = 0, pcap = 0;
    DIR *dp = opendir("/proc");
    if (!dp) su_error(3, errno, "cannot open /proc");
    struct dirent *de;
    while ((de = readdir(dp)) != NULL) {
        char *e;
        long v = strtol(de->d_name, &e, 10);
        if (!*de->d_name || *e || v <= 0) continue;
        if (npids == pcap) {
            pcap = pcap ? pcap * 2 : 256;
            pids = (int *)realloc(pids, (size_t)pcap * sizeof *pids);
            if (!pids) su_error(3, ENOMEM, "memory exhausted");
        }
        pids[npids++] = (int)v;
    }
    closedir(dp);
    qsort(pids, (size_t)npids, sizeof *pids, pg_cmp_int);

    int me = getpid();
    int want_cmd = full || list_full;
    struct pg_proc *hits = 0;
    int nhits = 0, hcap = 0;
    for (int i = 0; i < npids; i++) {
        if (pids[i] == me) continue;
        if (no_anc && pg_in(&anc, pids[i])) continue;
        char dir[64];
        int ntids = 0, tids[1];
        char **tdirs = 0;
        snprintf(dir, sizeof dir, "%d", pids[i]);
        /* -w: each thread in the pid's task directory */
        if (lightweight) {
            char tpath[64];
            snprintf(tpath, sizeof tpath, "/proc/%d/task", pids[i]);
            DIR *td = opendir(tpath);
            if (td) {
                struct dirent *te;
                while ((te = readdir(td)) != NULL) {
                    if (te->d_name[0] < '0' || te->d_name[0] > '9') continue;
                    tdirs = (char **)realloc(tdirs, (size_t)(ntids + 1) * sizeof *tdirs);
                    char *t = (char *)malloc(strlen(dir) + strlen(te->d_name) + 8);
                    sprintf(t, "%s/task/%s", dir, te->d_name);
                    tdirs[ntids++] = t;
                }
                closedir(td);
            }
        }
        (void)tids;
        int nent = ntids ? ntids : 1;
        for (int k = 0; k < nent; k++) {
            struct pg_proc p;
            if (pg_read(ntids ? tdirs[k] : dir, want_cmd, &p) != 0) continue;
            if (ntids) p.pid = atoi(strrchr(tdirs[k], '/') + 1);
            int ok = 1;
            if (have_re) {
                const char *subj = full && p.cmd ? p.cmd : p.comm;
                if (regexec(&re, subj, 0, 0, 0) != 0) ok = 0;
            }
            if (ok && parents.n && !pg_in(&parents, p.ppid)) ok = 0;
            if (ok && pgrps.n && !pg_in(&pgrps, p.pgrp)) ok = 0;
            if (ok && sids.n && !pg_in(&sids, p.session)) ok = 0;
            if (ok && euids.n && !pg_in(&euids, p.euid)) ok = 0;
            if (ok && ruids.n && !pg_in(&ruids, p.ruid)) ok = 0;
            if (ok && rgids.n && !pg_in(&rgids, p.rgid)) ok = 0;
            if (ok && states && !strchr(states, p.state)) ok = 0;
            if (ok && ttys) ok = 0;   /* no process here has a terminal in its stat */
            if (ok && pidfile && p.pid != file_pid) ok = 0;
            if (ok && older && (now < p.start || (now - p.start) / 100 < (unsigned long long)older)) ok = 0;
            if (inverse) ok = !ok;
            if (!ok) { free(p.cmd); continue; }
            if (nhits == hcap) {
                hcap = hcap ? hcap * 2 : 16;
                hits = (struct pg_proc *)realloc(hits, (size_t)hcap * sizeof *hits);
                if (!hits) su_error(3, ENOMEM, "memory exhausted");
            }
            hits[nhits++] = p;
        }
        for (int k = 0; k < ntids; k++) free(tdirs[k]);
        free(tdirs);
    }
    if ((newest || oldest) && nhits > 1) {
        int best = 0;
        for (int i = 1; i < nhits; i++) {
            if (newest ? hits[i].start >= hits[best].start : hits[i].start < hits[best].start)
                best = i;
        }
        hits[0] = hits[best];
        nhits = 1;
    }
    if (do_kill) {
        int killed = 0;
        for (int i = 0; i < nhits; i++) {
            if (kill(hits[i].pid, sig) == 0) {
                killed++;
                if (echo) printf("%s killed (pid %d)\n", hits[i].comm, hits[i].pid);
            } else
                su_error(0, errno, "killing pid %d failed", hits[i].pid);
        }
        if (count) printf("%d\n", nhits);
        fflush(stdout);
        return killed ? 0 : 1;
    }
    if (count) {
        printf("%d\n", nhits);
        return nhits ? 0 : 1;
    }
    for (int i = 0; i < nhits; i++) {
        if (i) fputs(delim, stdout);
        printf("%d", hits[i].pid);
        if (list_full) printf(" %s", hits[i].cmd ? hits[i].cmd : hits[i].comm);
        else if (list_name) printf(" %s", hits[i].comm);
    }
    if (nhits) putchar('\n');
    return nhits ? 0 : 1;
}
