/* su_env.c -- env, as GNU coreutils' (logs/utilgate/env.sh):
 * env [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]
 *
 * With no COMMAND, the environment, one NAME=VALUE a line (-0: NUL after
 * each). Options end at the first operand, as POSIX has it, so a COMMAND's
 * own options are its own:
 *   -i, -  start from an empty environment      -u NAME  remove NAME
 *   -C DIR change to DIR first                   -a ARG   ARG as argv[0]
 *   -S STR STR split into arguments, in place of this option -- blanks
 *          separate, '...' and "..." quote, \" \' \\ \$ \# \_ (a space; a
 *          separator outside quotes) \t \n \v \f \r and \c (the rest is
 *          ignored, not inside "...") are escapes, ${NAME} is NAME's value
 *          (unquoted or in "..."), and # starting an argument starts a
 *          comment; what it yields is read as options and operands again
 *   --ignore-signal[=SIG,...], --default-signal[=SIG], --block-signal[=SIG]
 *          the signals' handling for COMMAND (all of them without SIG);
 *          --list-signal-handling says what is not the default, on stderr
 *   -v     what it does, step by step, on stderr
 * Status: COMMAND's; 125 env's own failure, 126 COMMAND found but not run,
 * 127 not found. The old env took -i alone. */

static const char *const env_signames[] = {
    0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
    "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP",
    "TSTP", "TTIN", "TTOU", "URG", "XCPU", "XFSZ", "VTALRM", "PROF", "WINCH",
    "POLL", "PWR", "SYS"
};
#define ENV_NSIG 32

static int env_sig_of(const char *s) {
    if (isdigit((unsigned char)*s)) {
        char *e;
        long v = strtol(s, &e, 10);
        return (!*e && v > 0 && v < ENV_NSIG) ? (int)v : -1;
    }
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (int i = 1; i < ENV_NSIG; i++) if (!strcmp(s, env_signames[i])) return i;
    if (!strcmp(s, "IO")) return 29;
    if (!strcmp(s, "IOT")) return 6;
    if (!strcmp(s, "CLD")) return 17;
    return -1;
}

enum { ENV_UNSET, ENV_DEFAULT, ENV_IGNORE };
static int env_sig_action[ENV_NSIG];       /* per signal: what to set */
static int env_sig_given[ENV_NSIG];
static int env_sig_block[ENV_NSIG];

/* a comma list of signals (or none: all of them) into the tables */
static void env_sig_list(const char *arg, int action, int block) {
    if (!arg) {
        for (int i = 1; i < ENV_NSIG; i++) {
            if (i == 9 || i == 19) continue;            /* KILL, STOP */
            if (block) env_sig_block[i] = 1;
            else { env_sig_action[i] = action; env_sig_given[i] = 1; }
        }
        return;
    }
    char buf[256];
    snprintf(buf, sizeof buf, "%s", arg);
    for (char *t = strtok(buf, ","); t; t = strtok(0, ",")) {
        int s = env_sig_of(t);
        if (s < 0) {
            su_error(0, 0, "%s: invalid signal", su_quote(t));
            fflush(stdout);
            fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
            exit(125);
        }
        if (block) env_sig_block[s] = 1;
        else { env_sig_action[s] = action; env_sig_given[s] = 1; }
    }
}

/* ---- -S ----------------------------------------------------------------- */

static void env_split_fail(const char *msg) {
    su_error(125, 0, "%s", msg);
}

/* STR split as -S splits it, its arguments appended to *V */
static void env_split(const char *str, char ***v, int *n, int *cap) {
    struct ls_sb cur = { 0, 0, 0 };
    int have = 0;           /* an argument is being built (maybe empty: "") */
    const char *p = str;
#define ENV_PUSH() do { if (*n + 1 >= *cap) { *cap = *cap ? *cap * 2 : 16; \
        *v = (char **)realloc(*v, (size_t)*cap * sizeof **v); if (!*v) su_error(125, ENOMEM, "memory exhausted"); } \
        (*v)[(*n)++] = strdup(cur.s ? cur.s : ""); cur.n = 0; if (cur.s) cur.s[0] = 0; have = 0; } while (0)
    while (*p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') {
            if (have) ENV_PUSH();
            p++;
            continue;
        }
        if (c == '#' && !have) break;               /* a comment, to the end */
        if (c == '\'') {
            have = 1;
            p++;
            for (;;) {
                if (!*p) env_split_fail("no terminating quote in -S string");
                if (*p == '\'') { p++; break; }
                if (*p == '\\' && (p[1] == '\'' || p[1] == '\\')) { ls_putc(&cur, p[1]); p += 2; continue; }
                ls_putc(&cur, *p++);
            }
            continue;
        }
        if (c == '"') {
            have = 1;
            p++;
            for (;;) {
                if (!*p) env_split_fail("no terminating quote in -S string");
                if (*p == '"') { p++; break; }
                if (*p == '\\') {
                    char e = p[1];
                    if (e == 'c') env_split_fail("'\\c' must not appear in double-quoted -S string");
                    const char *rep = 0;
                    char one[2] = { 0, 0 };
                    switch (e) {
                    case '"': case '\'': case '\\': case '$': case '#': one[0] = e; rep = one; break;
                    case '_': rep = " "; break;
                    case 't': rep = "\t"; break;
                    case 'n': rep = "\n"; break;
                    case 'v': rep = "\v"; break;
                    case 'f': rep = "\f"; break;
                    case 'r': rep = "\r"; break;
                    case 0: env_split_fail("invalid backslash at end of string in -S");
                    }
                    if (!rep) {
                        char m[64];
                        snprintf(m, sizeof m, "invalid sequence '\\%c' in -S", e);
                        env_split_fail(m);
                    }
                    ls_put(&cur, rep, strlen(rep));
                    p += 2;
                    continue;
                }
                if (*p == '$') {
                    if (p[1] != '{') {
                        char m[300];
                        snprintf(m, sizeof m, "only ${VARNAME} expansion is supported, error at: %s", p);
                        env_split_fail(m);
                    }
                    const char *q = p + 2, *s = q;
                    while (isalnum((unsigned char)*q) || *q == '_') q++;
                    if (*q != '}' || q == s) {
                        char m[300];
                        snprintf(m, sizeof m, "only ${VARNAME} expansion is supported, error at: %s", p);
                        env_split_fail(m);
                    }
                    char name[128];
                    size_t nl = (size_t)(q - s) < sizeof name - 1 ? (size_t)(q - s) : sizeof name - 1;
                    memcpy(name, s, nl);
                    name[nl] = 0;
                    const char *val = getenv(name);
                    if (val) ls_put(&cur, val, strlen(val));
                    p = q + 1;
                    continue;
                }
                ls_putc(&cur, *p++);
            }
            continue;
        }
        if (c == '\\') {
            char e = p[1];
            if (e == 'c') break;                     /* the rest is ignored */
            if (e == '_') { if (have) ENV_PUSH(); p += 2; continue; }
            const char *rep = 0;
            char one[2] = { 0, 0 };
            switch (e) {
            case '"': case '\'': case '\\': case '$': case '#': one[0] = e; rep = one; break;
            case 't': rep = "\t"; break;
            case 'n': rep = "\n"; break;
            case 'v': rep = "\v"; break;
            case 'f': rep = "\f"; break;
            case 'r': rep = "\r"; break;
            case 0: env_split_fail("invalid backslash at end of string in -S");
            }
            if (!rep) {
                char m[64];
                snprintf(m, sizeof m, "invalid sequence '\\%c' in -S", e);
                env_split_fail(m);
            }
            have = 1;
            ls_put(&cur, rep, strlen(rep));
            p += 2;
            continue;
        }
        if (c == '$') {
            const char *q = p + 2, *s = q;
            if (p[1] == '{') while (isalnum((unsigned char)*q) || *q == '_') q++;
            if (p[1] != '{' || *q != '}' || q == s) {
                char m[300];
                snprintf(m, sizeof m, "only ${VARNAME} expansion is supported, error at: %s", p);
                env_split_fail(m);
            }
            char name[128];
            size_t nl = (size_t)(q - s) < sizeof name - 1 ? (size_t)(q - s) : sizeof name - 1;
            memcpy(name, s, nl);
            name[nl] = 0;
            const char *val = getenv(name);
            if (val) ls_put(&cur, val, strlen(val));
            have = 1;
            p = q + 1;
            continue;
        }
        have = 1;
        ls_putc(&cur, *p++);
    }
    if (have) ENV_PUSH();
#undef ENV_PUSH
    free(cur.s);
}

/* ---- env ----------------------------------------------------------------- */

static int env_debug;

static void env_usage_fail(const char *msg) {
    su_error(0, 0, "%s", msg);
    fflush(stdout);
    fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
    exit(125);
}

static int do_env(int argc, char **argv) {
    enum { O_BLOCK = 0x100, O_DEFAULT, O_IGNORE, O_LIST };
    static const struct su_option longs[] = {
        { "ignore-environment", SU_NO_ARG, 0, 'i' },
        { "null", SU_NO_ARG, 0, '0' },
        { "unset", SU_REQ_ARG, 0, 'u' },
        { "chdir", SU_REQ_ARG, 0, 'C' },
        { "split-string", SU_REQ_ARG, 0, 'S' },
        { "argv0", SU_REQ_ARG, 0, 'a' },
        { "block-signal", SU_OPT_ARG, 0, O_BLOCK },
        { "default-signal", SU_OPT_ARG, 0, O_DEFAULT },
        { "ignore-signal", SU_OPT_ARG, 0, O_IGNORE },
        { "list-signal-handling", SU_NO_ARG, 0, O_LIST },
        { "debug", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    /* the argument vector, which -S grows in place */
    int n = 0, cap = argc + 16;
    char **av = (char **)malloc((size_t)cap * sizeof *av);
    if (!av) su_error(125, ENOMEM, "memory exhausted");
    for (int i = 0; i < argc; i++) av[n++] = argv[i];
    av[n] = 0;
    int c, nul = 0, list = 0;
    const char *dir = 0, *argv0 = 0;
    for (;;) {
        c = su_getopt_long(n, av, "+0a:C:iS:u:v", longs, 0);
        if (c == -1) break;
        switch (c) {
        case 'i':
            if (env_debug) fprintf(stderr, "cleaning environ\n");
            clearenv();
            break;
        case '0': nul = 1; break;
        case 'u':
            if (!*su_optarg || strchr(su_optarg, '='))
                su_error(125, EINVAL, "cannot unset %s", su_quote(su_optarg));
            if (env_debug) fprintf(stderr, "unset:    %s\n", su_optarg);
            unsetenv(su_optarg);
            break;
        case 'C': dir = su_optarg; break;
        case 'a': argv0 = su_optarg; break;
        case 'v': env_debug = 1; break;
        case O_BLOCK: env_sig_list(su_optarg && *su_optarg ? su_optarg : (su_optarg ? "" : 0), 0, 1); break;
        case O_DEFAULT: env_sig_list(su_optarg && *su_optarg ? su_optarg : (su_optarg ? "" : 0), ENV_DEFAULT, 0); break;
        case O_IGNORE: env_sig_list(su_optarg && *su_optarg ? su_optarg : (su_optarg ? "" : 0), ENV_IGNORE, 0); break;
        case O_LIST: list = 1; break;
        case 'S': {
            /* the pieces take this option's place, and are read again */
            char **pieces = 0;
            int np = 0, pc = 0;
            if (env_debug) fprintf(stderr, "split -S:  %s\n", su_quote(su_optarg));
            env_split(su_optarg, &pieces, &np, &pc);
            int rest = su_optind;
            int nn = 1 + np + (n - rest);
            char **na = (char **)malloc((size_t)(nn + 1) * sizeof *na);
            if (!na) su_error(125, ENOMEM, "memory exhausted");
            na[0] = av[0];
            for (int k = 0; k < np; k++) na[1 + k] = pieces[k];
            for (int k = rest; k < n; k++) na[1 + np + (k - rest)] = av[k];
            na[nn] = 0;
            av = na;
            n = nn;
            /* the parser starts over on the new vector */
            su_optind = su_nextind = 1;
            su_cluster = 0;
            su_noptelem = su_nnonopt = 0;
            free(pieces);
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]\n"
                   "Run COMMAND with each NAME set to VALUE in its environment; with no\n"
                   "COMMAND, write the environment.\n"
                   "\n"
                   "  -a, --argv0=ARG           ARG as COMMAND's zeroth argument\n"
                   "  -i, --ignore-environment  start with an empty environment (as - does)\n"
                   "  -0, --null                end each line written with NUL, not newline\n"
                   "  -u, --unset=NAME          take NAME out of the environment\n"
                   "  -C, --chdir=DIR           run COMMAND in DIR\n"
                   "  -S, --split-string=S      split S into arguments, for #! lines\n"
                   "      --block-signal[=SIG]    COMMAND starts with SIG blocked\n"
                   "      --default-signal[=SIG]  ...with SIG's default handling\n"
                   "      --ignore-signal[=SIG]   ...with SIG ignored\n"
                   "      --list-signal-handling  say which signals are not handled the\n"
                   "                            default way, on standard error\n"
                   "  -v, --debug               say each step on standard error\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "SIG is a name (PIPE) or a number (13), or several with commas; with no\n"
                   "SIG, every signal. Status: COMMAND's, or 125 for env's own failure, 126\n"
                   "when COMMAND could not be run, 127 when it was not found.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            fflush(stdout);
            fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
            return 125;
        }
    }
    int i = su_optind;
    if (i < n && !strcmp(av[i], "-")) {
        if (env_debug) fprintf(stderr, "cleaning environ\n");
        clearenv();
        i++;
    }
    for (; i < n && strchr(av[i], '='); i++) {
        if (env_debug) fprintf(stderr, "setenv:   %s\n", av[i]);
        if (putenv(av[i]) != 0) su_error(125, errno, "cannot set %s", su_quote(av[i]));
    }
    if (i >= n) {
        if (dir) env_usage_fail("must specify command with --chdir (-C)");
        extern char **environ;
        for (char **e = environ; e && *e; e++) { fputs(*e, stdout); putchar(nul ? 0 : '\n'); }
        return 0;
    }
    if (nul) env_usage_fail("cannot specify --null (-0) with command");
    /* the signals, for COMMAND to inherit */
    sigset_t blk;
    int any_block = 0;
    sigemptyset(&blk);
    for (int s = 1; s < ENV_NSIG; s++) {
        if (env_sig_given[s]) {
            struct sigaction sa;
            memset(&sa, 0, sizeof sa);
            sa.sa_handler = env_sig_action[s] == ENV_IGNORE ? SIG_IGN : SIG_DFL;
            if (sigaction(s, &sa, 0) != 0 && (s == 9 || s == 19 || errno == EINVAL))
                su_error(125, EINVAL, "failed to set signal action for signal %d", s);
        }
        if (env_sig_block[s]) { sigaddset(&blk, s); any_block = 1; }
    }
    if (any_block && sigprocmask(SIG_BLOCK, &blk, 0) != 0)
        su_error(125, errno, "failed to set signal process mask");
    if (list) {
        sigset_t cur;
        sigemptyset(&cur);
        sigprocmask(SIG_BLOCK, 0, &cur);
        for (int s = 1; s < ENV_NSIG; s++) {
            struct sigaction sa;
            int ign = sigaction(s, 0, &sa) == 0 && sa.sa_handler == SIG_IGN;
            int b = sigismember(&cur, s) == 1;
            if (!ign && !b) continue;
            fprintf(stderr, "%-10s (%2d): %s%s%s\n", env_signames[s], s, b ? "BLOCK" : "",
                    b && ign ? "," : "", ign ? "IGNORE" : "");
        }
    }
    if (dir) {
        if (env_debug) fprintf(stderr, "chdir:    %s\n", su_quote(dir));
        if (chdir(dir) != 0) su_error(125, errno, "cannot change directory to %s", su_quote(dir));
    }
    const char *prog = av[i];
    if (argv0) {
        if (env_debug) fprintf(stderr, "argv0:     %s\n", su_quote(argv0));
        av[i] = (char *)argv0;
    }
    if (env_debug) {
        fprintf(stderr, "executing: %s\n", prog);
        for (int k = i; k < n; k++) fprintf(stderr, "   arg[%d]= %s\n", k - i, su_quote(av[k]));
    }
    fflush(stdout);
    execvp(prog, av + i);
    int e = errno;
    su_error(0, e, "%s", su_quote(prog));
    return e == ENOENT ? 127 : 126;
}
