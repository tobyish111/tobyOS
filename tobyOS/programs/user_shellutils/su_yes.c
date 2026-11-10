/* su_yes.c -- yes, as GNU coreutils': the operands joined by spaces, or
 * "y", one per line, forever. --help and --version are options wherever
 * they stand (getopt permutes: `yes a --help` is the help), `--` ends
 * the options (`yes a -- b` says "a b"), anything else that looks like an
 * option is getopt's error. A write that fails -- EPIPE with SIGPIPE
 * ignored -- is "yes: standard output: Broken pipe", status 1.
 * Measured on coreutils 9.7: logs/utilgate/yes.sh.
 *
 * It kept the first 4 KiB of its operands and took every argument as
 * text, options too: `yes -x` printed "-x" forever. */

static int do_yes(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s [STRING]...\n"
                   "  or:  %s OPTION\n"
                   "Print STRING -- the operands separated by spaces, or 'y'\n"
                   "if there are none -- on a line of its own, over and over\n"
                   "until it is killed or its output goes away.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n",
                   su_name, su_name);
            return 0;
        }
        if (c == SU_VERSION_OPT) return su_version();
        su_try_help(1);
    }

    /* one line, then as many whole copies of it as fill 8 KiB */
    size_t len = 0;
    for (int i = su_optind; i < argc; i++) len += strlen(argv[i]) + 1;
    if (su_optind == argc) len = 2;
    size_t copies = len < 8192 ? 8192 / len : 1;
    char *buf = (char *)malloc(len * copies);
    if (!buf) su_error(1, ENOMEM, "memory exhausted");
    size_t n = 0;
    if (su_optind == argc) {
        buf[n++] = 'y';
        buf[n++] = '\n';
    } else {
        for (int i = su_optind; i < argc; i++) {
            size_t l = strlen(argv[i]);
            memcpy(buf + n, argv[i], l);
            n += l;
            buf[n++] = i + 1 < argc ? ' ' : '\n';
        }
    }
    for (size_t k = 1; k < copies; k++) memcpy(buf + k * len, buf, len);
    n = len * copies;

    for (;;) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(1, buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                su_error(1, errno, "standard output");
            }
            off += (size_t)w;
        }
    }
}
