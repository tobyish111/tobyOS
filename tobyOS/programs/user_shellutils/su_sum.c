/* su_sum.c -- sum, as GNU coreutils' (logs/utilgate/sum.sh): a 16-bit
 * checksum and a block count for each FILE (no FILE, or "-": stdin).
 *
 *   -r          the BSD sum, the default: a rotating checksum, printed as
 *               five digits, and 1024-byte blocks right-aligned in five --
 *               "03762     1 a";
 *   -s, --sysv  the System V sum: the bytes added up and folded to 16 bits,
 *               and 512-byte blocks -- "1126 1 a".
 * The last of -r and -s wins. A FILE's name follows when operands were
 * given (stdin's "-" too); with none, no name. A file that cannot be read
 * is "sum: FILE: ERR", and the rest go on; status 1.
 *
 * The old sum printed no name at all, and stopped at nothing. */

static int sm_one(const char *name, int sysv, int named) {
    int fd = 0;
    if (strcmp(name, "-") != 0) {
        fd = su_open_read(name, 0);
        if (fd < 0) { su_error(0, errno, "%s", su_quotef(name)); return 1; }
    }
    unsigned char buf[8192];
    uintmax_t bytes = 0;
    unsigned bsd = 0;
    uint32_t sv = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            int e = errno;
            if (fd != 0) close(fd);
            su_error(0, e, "%s", su_quotef(name));
            return 1;
        }
        if (n == 0) break;
        bytes += (uintmax_t)n;
        for (ssize_t i = 0; i < n; i++) {
            if (sysv) sv += buf[i];
            else bsd = (((bsd >> 1) | ((bsd & 1) << 15)) + buf[i]) & 0xffff;
        }
    }
    if (fd != 0) close(fd);
    if (sysv) {
        uint32_t r = (sv & 0xffff) + (sv >> 16);
        unsigned ck = (r & 0xffff) + (r >> 16);
        printf("%u %ju", ck, (bytes + 511) / 512);
    } else {
        printf("%05u %5ju", bsd, (bytes + 1023) / 1024);
    }
    if (named) printf(" %s", name);
    putchar('\n');
    return 0;
}

static int do_sum(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "sysv", SU_NO_ARG, 0, 's' }, SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int c, sysv = 0;
    while ((c = su_getopt_long(argc, argv, "rs", longs, 0)) != -1) {
        switch (c) {
        case 'r': sysv = 0; break;
        case 's': sysv = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Print a 16-bit checksum and the block count of each FILE;\n"
                   "with no FILE, or when FILE is -, of standard input.\n"
                   "\n"
                   "  -r          the BSD checksum, 1024-byte blocks (the default)\n"
                   "  -s, --sysv  the System V checksum, 512-byte blocks\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int rc = 0;
    if (su_optind >= argc) return sm_one("-", sysv, 0);
    for (int i = su_optind; i < argc; i++) rc |= sm_one(argv[i], sysv, 1);
    return rc;
}
