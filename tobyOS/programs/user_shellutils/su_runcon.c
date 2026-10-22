/* su_runcon.c -- runcon, as GNU coreutils 9.7's (logs/utilgate/staging/
 * runcon.sh):
 *   runcon CONTEXT COMMAND [ARG]...
 *   runcon [-c] [-u USER] [-r ROLE] [-t TYPE] [-l RANGE] COMMAND [ARG]...
 *
 * Measured in WSL, which runs no SELinux:
 *   - with no operand (options or not) it prints the current context:
 *     /proc/thread-self/attr/current up to its first NUL, then a newline;
 *     a file that cannot be read is "failed to get current context:
 *     strerror", status 125 (tobyOS has no such file: ENOENT).
 *   - with operands, the first is CONTEXT unless one of -c -u -r -t -l
 *     was given, and then COMMAND must follow it, or "no command
 *     specified" and the usage hint, 125;
 *   - and otherwise, as no SELinux is running, "runcon: ARGV0 may be used
 *     only on a SELinux kernel", 125, and COMMAND is not run.
 * Options end at the first operand (`runcon x -u` is CONTEXT x and
 * COMMAND -u).
 *
 * The old runcon said only that SELinux was not enabled. */

static int do_runcon(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "role", SU_REQ_ARG, 0, 'r' },
        { "type", SU_REQ_ARG, 0, 't' },
        { "user", SU_REQ_ARG, 0, 'u' },
        { "range", SU_REQ_ARG, 0, 'l' },
        { "compute", SU_NO_ARG, 0, 'c' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int parts = 0, c;
    while ((c = su_getopt_long(argc, argv, "+r:t:u:l:c", longs, 0)) != -1) {
        switch (c) {
        case 'r': case 't': case 'u': case 'l': case 'c': parts = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s CONTEXT COMMAND [args]\n"
                   "  or:  %s [ -c ] [-u USER] [-r ROLE] [-t TYPE] [-l RANGE] COMMAND [args]\n"
                   "Run COMMAND in another SELinux security context. With no CONTEXT and\n"
                   "no COMMAND, show the context this process runs in.\n"
                   "\n"
                   "An argument a long option needs, its short form needs too.\n"
                   "  CONTEXT            a whole security context\n"
                   "  -c, --compute      work out the transition context before changing it\n"
                   "  -t, --type=TYPE    the type (the role stays the parent's)\n"
                   "  -u, --user=USER    the user\n"
                   "  -r, --role=ROLE    the role\n"
                   "  -l, --range=RANGE  the level range\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "The exit status is 125 when runcon itself fails, 126 when COMMAND\n"
                   "cannot be run, 127 when it is not found, else COMMAND's.\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(125);
        }
    }
    int nop = argc - su_optind;
    if (nop == 0) {
        int fd = su_open_read("/proc/thread-self/attr/current", 0);
        if (fd < 0) su_error(125, errno, "failed to get current context");
        char buf[4096];
        size_t len = 0;
        for (;;) {
            ssize_t r = read(fd, buf + len, sizeof buf - 1 - len);
            if (r < 0) { int e = errno; close(fd); su_error(125, e, "failed to get current context"); }
            if (r == 0 || len + (size_t)r >= sizeof buf - 1) { len += (size_t)(r > 0 ? r : 0); break; }
            len += (size_t)r;
        }
        close(fd);
        buf[len] = 0;
        printf("%s\n", buf);
        return 0;
    }
    if (!parts && nop < 2) {
        su_error(0, 0, "no command specified");
        su_try_help(125);
    }
    su_error(125, 0, "%s may be used only on a SELinux kernel", su_name);
    return 125;
}
