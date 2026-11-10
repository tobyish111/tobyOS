/* su_nice.c -- nice, as GNU coreutils 9.7's (logs/utilgate/nice.sh).
 *
 *   nice [OPTION] [COMMAND [ARG]...]
 *
 *   - With no COMMAND, the niceness this runs at; with one, COMMAND run at
 *     that plus -n N (--adjustment=N, the old forms -N, --N and -+N, the
 *     last of them winning), 10 if none is given. N is taken to -39..39,
 *     the niceness the kernel then keeps in -20..19.
 *   - A niceness one may not take (a lower one, without root) is a warning
 *     -- "cannot set niceness: Permission denied" -- and COMMAND still
 *     runs; any other failure is status 125.
 *   - GNU's statuses and messages: 125 "invalid adjustment 'N'", "a command
 *     must be given with an adjustment"; 126 COMMAND found but not run, 127
 *     not found ("'CMD': No such file or directory").
 *
 * The niceness is the kernel's (getpriority/setpriority, ABI_SYS_GETNICE
 * and SETNICE, batch 25): it carries the process's scheduling class with it
 * and /proc/PID/stat reports it. There was no niceness to set before; this
 * ran COMMAND unchanged with a warning saying so. */

static int do_nice(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "adjustment", SU_REQ_ARG, 0, 'n' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *adj = 0;
    int i = 1;
    while (i < argc) {
        const char *s = argv[i];
        if (s[0] == '-' && isdigit((unsigned char)s[1 + (s[1] == '-' || s[1] == '+')])) {
            adj = s + 1;
            i++;
            continue;
        }
        /* one option (or the end of them) at a time, from here, '+' order:
         * the old forms may come between -n's */
        su_nextind = i;
        su_cluster = 0;
        int c = su_getopt_long(argc, argv, "+n:", longs, 0);
        if (c == -1) { i = su_optind; break; }
        i = su_optind;
        switch (c) {
        case 'n': adj = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION] [COMMAND [ARG]...]\n"
                   "Run COMMAND at a niceness changed by N, or with no COMMAND say the\n"
                   "niceness this runs at. A higher niceness asks for less of the CPU;\n"
                   "they run from -20 to 19, and only root may lower one.\n"
                   "\n"
                   "  -n, --adjustment=N   add N (10 unless given)\n"
                   "      --help           show this help and exit\n"
                   "      --version        show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(125);
        }
    }
    if (!adj && i >= argc) {
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, 0);
        if (cur == -1 && errno) su_error(125, errno, "cannot get niceness");
        printf("%d\n", cur);
        return 0;
    }
    long adjustment = 10;
    if (adj) {
        char *end;
        errno = 0;
        long v = strtol(adj, &end, 10);
        if (end == adj || *end) su_error(125, 0, "invalid adjustment %s", su_quote(adj));
        /* an overflow is only a very large adjustment */
        adjustment = v < -39 ? -39 : v > 39 ? 39 : v;
    }
    if (i >= argc) {
        su_error(0, 0, "a command must be given with an adjustment");
        su_try_help(125);
    }
    errno = 0;
    int cur = getpriority(PRIO_PROCESS, 0);
    if (cur == -1 && errno) su_error(125, errno, "cannot get niceness");
    if (setpriority(PRIO_PROCESS, 0, (int)(cur + adjustment)) != 0) {
        int e = errno;
        su_error(e == EACCES || e == EPERM ? 0 : 125, e, "cannot set niceness");
    }
    fflush(stderr);
    execvp(argv[i], argv + i);
    int e = errno;
    su_error(0, e, "%s", su_quote(argv[i]));
    return e == ENOENT ? 127 : 126;
}
