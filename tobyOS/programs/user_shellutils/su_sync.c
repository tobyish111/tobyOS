/* su_sync.c -- sync, as GNU coreutils': with no FILE, everything; with
 * FILEs, each one (opened first: "error opening 'F'", status 1, and the
 * rest still done). -d/--data needs a FILE ("--data needs at least one
 * argument"); -d with -f/--file-system is "cannot specify both".
 * Measured on coreutils 9.7: logs/utilgate/sync.sh.
 *
 * tobyOS has one flush, the whole block cache (ABI_SYS_SYNC): a FILE's
 * sync, its data's or its file system's is that -- more than asked,
 * never less. It took no options and no FILEs. */

static int sync_all(void) {
#ifdef SU_HOST
    sync();
    return 0;
#else
    long rc = sc0(ABI_SYS_SYNC);
    return rc < 0 ? (int)-rc : 0;
#endif
}

static int do_sync(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "data", SU_NO_ARG, 0, 'd' },
        { "file-system", SU_NO_ARG, 0, 'f' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int data = 0, fs = 0, c;
    while ((c = su_getopt_long(argc, argv, "df", longs, 0)) != -1) {
        switch (c) {
        case 'd': data = 1; break;
        case 'f': fs = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION] [FILE]...\n"
                   "Write what is cached in memory out to storage: everything,\n"
                   "or what holds each FILE.\n"
                   "\n"
                   "  -d, --data          only each FILE's data, not its metadata\n"
                   "  -f, --file-system   the whole file system each FILE is on\n"
                   "      --help          show this help and exit\n"
                   "      --version       show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (data && fs) su_error(1, 0, "cannot specify both --data and --file-system");
    if (data && su_optind == argc) su_error(1, 0, "--data needs at least one argument");
    if (su_optind == argc) {
        int e = sync_all();
        if (e) su_error(1, e, "sync failed");
        return 0;
    }
    int rc = 0, any = 0;
    for (int i = su_optind; i < argc; i++) {
        int fd = su_open_read(argv[i], O_NONBLOCK);
        if (fd < 0 && (errno == EACCES || errno == EISDIR))
            fd = open(argv[i], O_WRONLY | O_NONBLOCK);
        if (fd < 0) {
            su_error(0, errno, "error opening %s", su_quoteaf(argv[i]));
            rc = 1;
            continue;
        }
        close(fd);
        any = 1;
    }
    if (any) {
        int e = sync_all();
        if (e) { su_error(0, e, "error syncing"); rc = 1; }
    }
    return rc;
}
