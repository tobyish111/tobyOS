/* su_mkfifo.c -- mkfifo, as GNU coreutils': each NAME a FIFO, 0666 less
 * the umask; with -m MODE, MODE applied to 0666 the way chmod applies it
 * (a clause with no [ugoa] masked by the umask) and set exactly, not less
 * the umask again. A MODE with more than the permission bits is "mode
 * must specify only file permission bits"; one chmod would refuse is
 * "invalid mode". -Z and --context=CTX ask for an SELinux context: there
 * is none here, so they are taken and do nothing, as GNU's on a kernel
 * without SELinux. Measured on coreutils 9.7: logs/utilgate/mkfifo.sh.
 *
 * Its own mode reader took "u=rw,g=r" from a=rw, not from 0666 less what
 * the clauses clear, and the umask never applied to a clause with no
 * [ugoa] (finding 96's mkfifo). */

static int do_mkfifo(int argc, char **argv) {
    enum { CONTEXT_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "mode", SU_REQ_ARG, 0, 'm' },
        { "context", SU_OPT_ARG, 0, CONTEXT_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *mstr = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "m:Z", longs, 0)) != -1) {
        switch (c) {
        case 'm': mstr = su_optarg; break;
        case 'Z': case CONTEXT_OPT: break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... NAME...\n"
                   "Make each NAME a named pipe (a FIFO).\n"
                   "\n"
                   "  -m, --mode=MODE    give it MODE, as chmod reads one, instead\n"
                   "                     of a=rw less the umask\n"
                   "  -Z                 its SELinux context: the default (there is\n"
                   "                     no SELinux here)\n"
                   "      --context[=CTX]  likewise\n"
                   "      --help         show this help and exit\n"
                   "      --version      show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind == argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    mode_t mode = 0666;
    if (mstr) {
        struct su_mode m;
        if (su_mode_compile(mstr, &m) != 0) su_error(1, 0, "invalid mode");
        mode_t um = umask(0);
        umask(um);
        mode = su_mode_adjust(0666, 0, um, &m);
        if (mode & ~(mode_t)0777)
            su_error(1, 0, "mode must specify only file permission bits");
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        if (mkfifo(argv[i], mode) != 0) {
            su_error(0, errno, "cannot create fifo %s", su_quoteaf(argv[i]));
            rc = 1;
            continue;
        }
        if (mstr && chmod(argv[i], mode) != 0) {
            su_error(0, errno, "cannot set permissions of %s", su_quoteaf(argv[i]));
            rc = 1;
        }
    }
    return rc;
}
