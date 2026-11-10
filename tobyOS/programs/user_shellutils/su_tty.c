/* su_tty.c -- tty, as GNU coreutils': the name of the terminal on
 * standard input, or "not a tty"; -s/--silent/--quiet prints nothing.
 * Status 0 a terminal, 1 not one, 2 a usage error (an operand too), 3 a
 * write error. Measured on coreutils 9.7: logs/utilgate/tty.sh.
 *
 * It had no -s, took operands and options as nothing, and said 0 for a
 * usage error. */

/* ttyname(0): what /proc/self/fd/0 names -- /dev/pts/N for a pty (an SSH
 * session, a terminal window), /dev/console where there is no link. */
static int tty_name(char *buf, size_t cap) {
    long n = readlink("/proc/self/fd/0", buf, cap - 1);
    if (n > 5 && strncmp(buf, "/dev/", 5) == 0) {
        buf[n] = 0;
        return 0;
    }
    snprintf(buf, cap, "/dev/console");
    return 0;
}

static int do_tty(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "silent", SU_NO_ARG, 0, 's' },
        { "quiet", SU_NO_ARG, 0, 's' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int silent = 0, c;
    while ((c = su_getopt_long(argc, argv, "s", longs, 0)) != -1) {
        switch (c) {
        case 's': silent = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]...\n"
                   "Print the file name of the terminal on standard input.\n"
                   "\n"
                   "  -s, --silent, --quiet   print nothing; only the status says\n"
                   "      --help              show this help and exit\n"
                   "      --version           show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(2);
        }
    }
    if (su_optind < argc) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(2);
    }
    int is = isatty(0);
    if (!silent) {
        char name[256];
        if (is) {
            tty_name(name, sizeof name);
            puts(name);
        } else {
            puts("not a tty");
        }
        if (fflush(stdout) != 0) su_error(3, errno, "write error");
    }
    return is ? 0 : 1;
}
