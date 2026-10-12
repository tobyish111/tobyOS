/* su_timeout.c -- timeout; included by main.c. */

/* timeout -- start a COMMAND, and signal it if it outlasts a DURATION.
 *
 *     timeout [OPTION] DURATION COMMAND [ARG]...
 *
 * AS GNU COREUTILS' TIMEOUT, measured on 9.7 in the C locale (tools/agent/
 * probes/b24/timeoutcases*.sh; logs/b24-gnu-timeout*.out):
 *
 *   - DURATION as sleep reads one (a number as strtod reads it, one suffix
 *     of s m h d; 0 or inf: no timeout); -k DURATION also KILLs the command
 *     that long after the first signal; -s SIGNAL, a name in any case with
 *     or without SIG, RTMIN/RTMAX(+-n), or a number 0..64.
 *   - The status: the command's own; 124 if it timed out (with -p, the
 *     command's own then too); 125 when timeout itself fails, 126 when the
 *     command cannot be run, 127 when it is not found. A command a signal
 *     killed, not by timing out: timeout dies of the same signal, so its
 *     shell reports it as for the command. A KILL sent to the group takes
 *     timeout down with it (137, "Killed").
 *   - Unless -f, timeout leads a process group and signals all of it -- the
 *     command's own children too -- ignoring the signal itself, then a
 *     CONT for a stopped command. A HUP INT QUIT or TERM sent to timeout is
 *     passed on to the command the same way.
 *   - Options until the first operand (getopt's "+"): -f -k -p -s -v and
 *     their long names, any unambiguous prefix; the errors and "Try
 *     'timeout --help' ..." getopt's; too few operands, the Try line alone.
 *
 * It took whole seconds (atol), ignored -s, -k and -p, signalled the
 * command alone, said 1 for a command a signal killed, and -- polling once
 * a second and killing without a last look -- said 124 for a command that
 * failed inside its first second: `timeout 1 false` (finding 130). */

static const char *g_to_name;          /* argv[0] as typed */
static const char *g_to_short;         /* its last component */

static void to_try(void) {
    fprintf(stderr, "Try '%s --help' for more information.\n", g_to_name);
    exit(125);
}

static const struct { int num; const char *name; } k_to_sigs[] = {
    { 1,  "HUP"  }, { 2,  "INT"  }, { 3,  "QUIT" }, { 4,  "ILL"  },
    { 5,  "TRAP" }, { 6,  "ABRT" }, { 6,  "IOT"  }, { 7,  "BUS"  },
    { 8,  "FPE"  }, { 9,  "KILL" }, { 10, "USR1" }, { 11, "SEGV" },
    { 12, "USR2" }, { 13, "PIPE" }, { 14, "ALRM" }, { 15, "TERM" },
    { 16, "STKFLT" }, { 17, "CHLD" }, { 17, "CLD" }, { 18, "CONT" },
    { 19, "STOP" }, { 20, "TSTP" }, { 21, "TTIN" }, { 22, "TTOU" },
    { 23, "URG"  }, { 24, "XCPU" }, { 25, "XFSZ" }, { 26, "VTALRM" },
    { 27, "PROF" }, { 28, "WINCH" }, { 29, "IO" }, { 29, "POLL" },
    { 30, "PWR" }, { 31, "SYS" },
};
#define TO_NSIGS ((int)(sizeof k_to_sigs / sizeof k_to_sigs[0]))
#define TO_RTMIN 34
#define TO_RTMAX 64

/* SIGNAL's number, or -1. */
static int to_signal(const char *s) {
    if (*s >= '0' && *s <= '9') {
        int v = 0;
        for (; *s; s++) {
            if (*s < '0' || *s > '9' || v > 64) return -1;
            v = v * 10 + (*s - '0');
        }
        return v <= 64 ? v : -1;
    }
    char up[16];
    size_t n = strlen(s);
    if (n >= sizeof up) return -1;
    for (size_t k = 0; k <= n; k++) up[k] = (char)toupper((unsigned char)s[k]);
    const char *u = strncmp(up, "SIG", 3) == 0 ? up + 3 : up;
    for (int k = 0; k < TO_NSIGS; k++)
        if (strcmp(u, k_to_sigs[k].name) == 0) return k_to_sigs[k].num;
    int base = strncmp(u, "RTMIN", 5) == 0 ? TO_RTMIN
             : strncmp(u, "RTMAX", 5) == 0 ? TO_RTMAX : 0;
    if (!base) return -1;
    const char *p = u + 5;
    if (!*p) return base;
    if ((*p != '+' && *p != '-') || p[1] < '0' || p[1] > '9') return -1;
    int off = atoi(p + 1), v = *p == '+' ? base + off : base - off;
    return (v >= TO_RTMIN && v <= TO_RTMAX) ? v : -1;
}

static const char *to_signame(int sig, char *buf, size_t cap) {
    for (int k = 0; k < TO_NSIGS; k++)
        if (k_to_sigs[k].num == sig) return k_to_sigs[k].name;
    snprintf(buf, cap, "%d", sig);
    return buf;
}

/* A DURATION into nanoseconds, saturating (TO_FOREVER), as sleep reads its
 * operands -- ports/sbase/sleep.c parse_interval, the same rules (this
 * multi-call binary and the ports share no source). 0 if one, -1 if not.
 * Integer arithmetic: built without SSE. */
#define TO_FOREVER (~0ULL)
static unsigned long long to_mul(unsigned long long a, unsigned long long b) {
    if (a == 0 || b == 0) return 0;
    if (a == TO_FOREVER || b == TO_FOREVER || b > TO_FOREVER / a) return TO_FOREVER;
    return a * b;
}
static long to_exponent(const char **pp) {
    const char *q = *pp + 1;
    long sign = 1, e = 0;
    if (*q == '+' || *q == '-') { if (*q == '-') sign = -1; q++; }
    if (*q < '0' || *q > '9') return 0;
    for (; *q >= '0' && *q <= '9'; q++) if (e < 100000) e = e * 10 + (*q - '0');
    *pp = q;
    return sign * e;
}
static int to_hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}
static int to_duration(const char *arg, unsigned long long *out) {
    const char *p = arg;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    enum { DEC, HEX, INF } kind;
    unsigned long long m = 0;
    long e = 0;
    if (strncasecmp(p, "inf", 3) == 0) {
        kind = INF;
        p += strncasecmp(p, "infinity", 8) == 0 ? 8 : 3;
    } else if (strncasecmp(p, "nan", 3) == 0) {
        return -1;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
               (to_hexval(p[2]) >= 0 || (p[2] == '.' && to_hexval(p[3]) >= 0))) {
        kind = HEX;
        for (p += 2; to_hexval(*p) >= 0; p++) {
            if (m < (1ULL << 56)) m = m * 16 + (unsigned)to_hexval(*p);
            else e += 4;
        }
        if (*p == '.')
            for (p++; to_hexval(*p) >= 0; p++)
                if (m < (1ULL << 56)) { m = m * 16 + (unsigned)to_hexval(*p); e -= 4; }
        if (*p == 'p' || *p == 'P') e += to_exponent(&p);
    } else {
        kind = DEC;
        int digits = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            if (m < 100000000000000000ULL) m = m * 10 + (unsigned)(*p - '0');
            else e++;
        }
        if (*p == '.')
            for (p++; *p >= '0' && *p <= '9'; p++, digits++)
                if (m < 100000000000000000ULL) { m = m * 10 + (unsigned)(*p - '0'); e--; }
        if (!digits) return -1;
        if (*p == 'e' || *p == 'E') e += to_exponent(&p);
    }
    unsigned long long mult = 1;
    switch (*p) {
    case '\0': break;
    case 's': p++; break;
    case 'm': mult = 60; p++; break;
    case 'h': mult = 3600; p++; break;
    case 'd': mult = 86400; p++; break;
    default:  return -1;
    }
    if (*p) return -1;
    if (neg && (kind == INF || m != 0)) return -1;
    unsigned long long ns;
    if (kind == INF) {
        ns = TO_FOREVER;
    } else if (m == 0) {
        ns = 0;
    } else if (kind == DEC) {
        long k = e + 9;
        ns = m;
        while (k < 0 && ns > TO_FOREVER / mult) { ns /= 10; k++; }
        ns = to_mul(ns, mult);
        for (; k > 0 && ns != TO_FOREVER; k--) ns = to_mul(ns, 10);
        for (; k < 0 && ns; k++) ns /= 10;
    } else {
        ns = to_mul(m, mult);
        while (e < 0 && ns > TO_FOREVER / 1000000000ULL) { ns >>= 1; e++; }
        ns = to_mul(ns, 1000000000ULL);
        for (; e > 0 && ns != TO_FOREVER; e--) ns = to_mul(ns, 2);
        for (; e < 0 && ns; e++) ns >>= 1;
    }
    *out = ns;
    return 0;
}

static unsigned long long to_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + (unsigned long long)ts.tv_nsec;
}

struct to_opts {
    int sig, verbose, preserve, foreground;
    unsigned long long kill_after;
};

/* One option: f k p s v, or H (--help) and V (--version), which end
 * timeout here (1). */
static int to_apply(int opt, const char *val, struct to_opts *o) {
    switch (opt) {
    case 'f': o->foreground = 1; break;
    case 'p': o->preserve = 1; break;
    case 'v': o->verbose = 1; break;
    case 'k':
        if (to_duration(val, &o->kill_after) != 0) {
            fprintf(stderr, "%s: invalid time interval '%s'\n", g_to_short, val);
            to_try();
        }
        break;
    case 's':
        o->sig = to_signal(val);
        if (o->sig < 0) {
            fprintf(stderr, "%s: '%s': invalid signal\n", g_to_short, val);
            to_try();
        }
        break;
    case 'H':
        printf("Usage: %s [OPTION] DURATION COMMAND [ARG]...\n"
               "  or:  %s [OPTION]\n"
               "Run COMMAND; if it is still running after DURATION, send it a signal.\n"
               "\n"
               "  -f, --foreground       signal COMMAND alone, not its process group,\n"
               "                         and leave it the terminal\n"
               "  -k, --kill-after=DUR   send KILL too if COMMAND is still running\n"
               "                         DUR after the first signal\n"
               "  -p, --preserve-status  exit with COMMAND's status even when it times out\n"
               "  -s, --signal=SIGNAL    the signal to send (TERM unless given): a name\n"
               "                         such as HUP, or a number\n"
               "  -v, --verbose          say on stderr each signal sent\n"
               "      --help             show this help and exit\n"
               "      --version          show the version and exit\n"
               "\n"
               "DURATION is a number with an optional s, m, h or d suffix; 0 means none.\n"
               "Exit status: COMMAND's; 124 if it timed out, 125 if timeout failed,\n"
               "126 if COMMAND could not be run, 127 if it was not found.\n",
               g_to_name, g_to_name);
        return 1;
    case 'V':
        printf("timeout (tobyOS) 1.0\n");
        return 1;
    }
    return 0;
}

/* A HUP INT QUIT or TERM sent to timeout, for the wait to pass on. */
static volatile int g_to_pending;
static void to_note(int sig) { g_to_pending = sig; }

/* Send SIG to the command -- and, unless -f, to the whole group, timeout
 * ignoring it (a KILL it cannot: that ends timeout too), then a CONT for a
 * stopped command. */
static void to_send(pid_t pid, int sig, const struct to_opts *o,
                    const char *cmd) {
    if (o->verbose) {
        char nb[16];
        fprintf(stderr, "%s: sending signal %s to command '%s'\n",
                g_to_short, to_signame(sig, nb, sizeof nb), cmd);
    }
    kill(pid, sig);
    if (!o->foreground) {
        if (sig > 0 && sig != SIGKILL && sig != SIGSTOP) signal(sig, SIG_IGN);
        kill(0, sig);
        if (sig != SIGKILL && sig != SIGCONT) {
            kill(pid, SIGCONT);
            kill(0, SIGCONT);
        }
    }
}

/* Wait for PID until DEADLINE (TO_FOREVER: none): 1 and *ST if it ended, 0
 * if the deadline came first, minus the signal if timeout was sent one.
 * Polled at 10 ms: libtoby has no alarm(). */
static int to_wait(pid_t pid, unsigned long long deadline, int *st) {
    for (;;) {
        pid_t r = waitpid(pid, st, WNOHANG);
        if (r == pid) return 1;
        if (r < 0 && errno != EINTR) { *st = 0; return 1; }
        if (g_to_pending) {
            int s = g_to_pending;
            g_to_pending = 0;
            return -s;
        }
        if (deadline != TO_FOREVER && to_now() >= deadline) {
            /* a last look: an end that came with the deadline is an end */
            if (waitpid(pid, st, WNOHANG) == pid) return 1;
            return 0;
        }
        struct timespec ts = { 0, 10000000L };
        nanosleep(&ts, 0);
    }
}

static int do_timeout(int argc, char **argv) {
    g_to_name = (argc > 0 && argv[0] && argv[0][0]) ? argv[0] : "timeout";
    g_to_short = strrchr(g_to_name, '/') ? strrchr(g_to_name, '/') + 1 : g_to_name;

    struct to_opts o = { SIGTERM, 0, 0, 0, 0 };
    static const char *const longs[] = {
        "foreground", "kill-after", "preserve-status", "signal", "verbose",
        "help", "version",
    };
    static const char long_opt[] = "fkpsvHV";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || a[1] == '\0') break;          /* the first operand */
        if (a[1] == '-') {
            const char *name = a + 2;
            size_t n = strcspn(name, "=");
            int hit = -1, hits = 0;
            for (int k = 0; k < 7; k++) {
                if (n == 0 || strncmp(name, longs[k], n) != 0) continue;
                if (strlen(longs[k]) == n) { hit = k; hits = 1; break; }
                hit = k;
                hits++;
            }
            if (hits == 0) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", g_to_name, a);
                to_try();
            }
            if (hits > 1) {
                fprintf(stderr, "%s: option '%.*s' is ambiguous; possibilities:",
                        g_to_name, (int)(n + 2), a);
                for (int k = 0; k < 7; k++)
                    if (strncmp(name, longs[k], n) == 0)
                        fprintf(stderr, " '--%s'", longs[k]);
                fprintf(stderr, "\n");
                to_try();
            }
            int opt = long_opt[hit];
            int takes = opt == 'k' || opt == 's';
            const char *val = 0;
            if (name[n] == '=') {
                if (!takes) {
                    fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                            g_to_name, longs[hit]);
                    to_try();
                }
                val = name + n + 1;
            } else if (takes) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "%s: option '--%s' requires an argument\n",
                            g_to_name, longs[hit]);
                    to_try();
                }
                val = argv[++i];
            }
            if (to_apply(opt, val, &o)) return 0;
            continue;
        }
        /* a cluster of short options: -vsINT, -k 1 */
        for (const char *c = a + 1; *c; c++) {
            if (!strchr("fkpsv", *c)) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", g_to_name, *c);
                to_try();
            }
            const char *val = 0;
            if (*c == 'k' || *c == 's') {
                if (c[1]) val = c + 1;
                else if (i + 1 < argc) val = argv[++i];
                else {
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                            g_to_name, *c);
                    to_try();
                }
            }
            to_apply(*c, val, &o);
            if (val) break;
        }
    }
    if (argc - i < 2) to_try();

    unsigned long long dur;
    if (to_duration(argv[i], &dur) != 0) {
        fprintf(stderr, "%s: invalid time interval '%s'\n", g_to_short, argv[i]);
        to_try();
    }
    char **cmdv = argv + i + 1;

    if (!o.foreground) (void)setpgid(0, 0);
    static const int passed_on[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
    for (int k = 0; k < 4; k++) signal(passed_on[k], to_note);
    if (o.sig != SIGKILL && o.sig != SIGSTOP && o.sig != 0) signal(o.sig, to_note);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: fork system call failed: %s\n", g_to_short,
                strerror(errno));
        return 125;
    }
    if (pid == 0) {
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        execvp(cmdv[0], cmdv);
        int en = errno;
        fprintf(stderr, "%s: failed to run command '%s': %s\n", g_to_short,
                cmdv[0], strerror(en));
        _exit(en == ENOENT ? 127 : 126);
    }

    /* The deadline sends -s's signal, then -- with -k -- a KILL that long
     * after the first signal sent, whether a timeout's or one passed on. */
    int st = 0, timed_out = 0, next = o.sig;
    unsigned long long deadline = (dur == 0 || dur == TO_FOREVER)
                                ? TO_FOREVER : to_now() + dur;
    for (;;) {
        int r = to_wait(pid, deadline, &st), sig;
        if (r == 1) break;
        if (r == 0) {
            timed_out = 1;
            sig = next;
            deadline = TO_FOREVER;
        } else {
            sig = -r;
        }
        to_send(pid, sig, &o, cmdv[0]);
        if (o.kill_after) {
            next = SIGKILL;
            deadline = o.kill_after == TO_FOREVER ? TO_FOREVER
                                                  : to_now() + o.kill_after;
            o.kill_after = 0;
        }
    }

    /* WIFSIGNALED first: libtoby's WIFEXITED is true of a child a signal
     * killed too (128+N, sys/wait.h). */
    int status;
    if (WIFSIGNALED(st)) {
        int s = WTERMSIG(st);
        if (!timed_out) {
            /* the command died of it: so does timeout, for its shell */
            signal(s, SIG_DFL);
            raise(s);
        }
        if (timed_out && s == SIGKILL) o.preserve = 1;
        status = 128 + s;
    } else if (WIFEXITED(st)) {
        status = WEXITSTATUS(st);
    } else {
        status = 125;
    }
    if (timed_out && !o.preserve) status = 124;
    return status;
}
