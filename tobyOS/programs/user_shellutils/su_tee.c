/* su_tee.c -- tee, as GNU coreutils 9.7's (logs/utilgate/staging/tee.sh):
 * standard input copied to standard output and to each FILE.
 *
 * Measured on GNU's (gnutee in WSL; the pipe cases inside a pid namespace
 * of their own):
 *   - Each FILE is opened first (created 0666 less the umask, truncated,
 *     or appended to with -a); one that does not open is "tee: FILE: ERR",
 *     status 1, and is left out. "-" is a file named "-".
 *   - Input is read 8 KiB at a time and each read written to standard
 *     output, then to the FILEs in order. When no output is left, tee
 *     stops reading (the rest of a shared input is still there).
 *   - A write that fails: the output is dropped, "tee: NAME: ERR" (NAME
 *     'standard output' for stdout), status 1 -- except EPIPE, which by
 *     default and in the -nopipe modes drops the output silently (by
 *     default only reachable with SIGPIPE ignored by whoever ran tee).
 *     --output-error=exit and exit-nopipe exit 1 at the first error they
 *     report. Any --output-error, and -p (warn-nopipe), ignore SIGPIPE;
 *     without one, SIGPIPE kills tee as it would any writer.
 *   - --output-error with no MODE is warn-nopipe; MODE as GNU's argmatch
 *     takes it (a unique prefix; "e" is ambiguous).
 *   - A read error is "tee: read error: ERR", status 1; standard input is
 *     closed at the end and a failure is "tee: standard input: ERR".
 *   - -i ignores SIGINT.
 * The old /bin/tee (ports/sbase/tee.c) had -a and -i only and stopped at
 * the first write error. */

#include "su_fileops.c"

enum { TE_DEFAULT, TE_WARN, TE_WARN_NOPIPE, TE_EXIT, TE_EXIT_NOPIPE };

static int do_tee(int argc, char **argv) {
    fo_stdout_guard();
    enum { OUTPUT_ERROR_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "append", SU_NO_ARG, 0, 'a' },
        { "ignore-interrupts", SU_NO_ARG, 0, 'i' },
        { "output-error", SU_OPT_ARG, 0, OUTPUT_ERROR_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    static const char *const mode_names[] = { "warn", "warn-nopipe", "exit", "exit-nopipe", 0 };
    static const int mode_vals[] = { TE_WARN, TE_WARN_NOPIPE, TE_EXIT, TE_EXIT_NOPIPE };
    int append = 0, ignore_int = 0, mode = TE_DEFAULT, c;
    while ((c = su_getopt_long(argc, argv, "aip", longs, 0)) != -1) {
        switch (c) {
        case 'a': append = 1; break;
        case 'i': ignore_int = 1; break;
        case 'p': mode = TE_WARN_NOPIPE; break;
        case OUTPUT_ERROR_OPT:
            mode = su_optarg ? fo_argmatch("--output-error", su_optarg, mode_names, mode_vals)
                             : TE_WARN_NOPIPE;
            break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Copy standard input to standard output and to every FILE.\n"
                   "\n"
                   "  -a, --append              add to the end of each FILE instead of\n"
                   "                              emptying it first\n"
                   "  -i, --ignore-interrupts   ignore SIGINT\n"
                   "  -p                        the same as --output-error=warn-nopipe\n"
                   "      --output-error[=MODE]   what a failed write does (MODE below)\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "MODE, for a write that fails:\n"
                   "  warn           report it, and go on with the other outputs\n"
                   "  warn-nopipe    the same, but a pipe with no reader is dropped quietly\n"
                   "  exit           report it and stop\n"
                   "  exit-nopipe    the same, but a pipe with no reader is dropped quietly\n"
                   "--output-error with no MODE, and -p, mean warn-nopipe; the -nopipe\n"
                   "modes stop once every output is a pipe with no reader.  With no\n"
                   "--output-error, a pipe with no reader ends tee (SIGPIPE) and any\n"
                   "other failed write is reported, its output dropped.\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (ignore_int) signal(SIGINT, SIG_IGN);
    if (mode != TE_DEFAULT) signal(SIGPIPE, SIG_IGN);

    int nfiles = argc - su_optind;
    int *fds = (int *)malloc(sizeof(int) * (size_t)(nfiles + 1));
    const char **names = (const char **)malloc(sizeof(char *) * (size_t)(nfiles + 1));
    if (!fds || !names) su_error(1, ENOMEM, "memory exhausted");
    int rc = 0, live = 1;
    fds[0] = 1;
    names[0] = "standard output";
    for (int i = 0; i < nfiles; i++) {
        const char *f = argv[su_optind + i];
        names[i + 1] = f;
        fds[i + 1] = fo_safer(open(f, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0666));
        if (fds[i + 1] < 0) {
            su_error(0, errno, "%s", su_quotef(f));
            rc = 1;
        } else {
            live++;
        }
    }

    static char buf[8192];
    for (;;) {
        ssize_t n = read(0, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            su_error(0, errno, "read error");
            rc = 1;
            break;
        }
        if (n == 0) break;
        for (int i = 0; i <= nfiles; i++) {
            if (fds[i] < 0) continue;
            if (fo_write_all(fds[i], buf, (size_t)n) == 0) continue;
            int e = errno;
            int quiet = e == EPIPE && (mode == TE_DEFAULT || mode == TE_WARN_NOPIPE ||
                                       mode == TE_EXIT_NOPIPE);
            if (!quiet) {
                su_error(0, e, "%s", su_quotef(names[i]));
                rc = 1;
                if (mode == TE_EXIT || mode == TE_EXIT_NOPIPE) exit(1);
            }
            if (i > 0) close(fds[i]);
            fds[i] = -1;
            live--;
        }
        if (live == 0) break;
    }
    for (int i = 1; i <= nfiles; i++) {
        if (fds[i] >= 0 && close(fds[i]) != 0) {
            su_error(0, errno, "%s", su_quotef(names[i]));
            rc = 1;
        }
    }
    if (close(0) != 0) su_error(1, errno, "standard input");
    return rc;
}
