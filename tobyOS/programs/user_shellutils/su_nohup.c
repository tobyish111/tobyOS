/* su_nohup.c -- nohup, as GNU coreutils': run COMMAND with hangups
 * ignored. Where standard input is a terminal it is replaced by /dev/null
 * ("ignoring input"); where standard output is one, output is appended to
 * nohup.out (or $HOME/nohup.out); where standard error is one, it goes
 * where standard output does -- each said once, in GNU's words, on the
 * original standard error. Options only before COMMAND. Status 125 when
 * nohup itself fails, 126 when COMMAND cannot be run, 127 when it is not
 * found, else COMMAND's. Measured on coreutils 9.7: logs/utilgate/
 * nohup.sh (not on a terminal; the terminal cases are GNU's logic).
 *
 * It said "usage: nohup ..." and 127 for everything, ran "-x" as a
 * command, and redirected stdout but not stdin or stderr. */

static int do_nohup(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "+", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s COMMAND [ARG]...\n"
                   "  or:  %s OPTION\n"
                   "Run COMMAND so that it is not ended by a hangup.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "Standard input that is a terminal is read from /dev/null\n"
                   "instead; standard output that is one is appended to\n"
                   "nohup.out (or $HOME/nohup.out); standard error that is one\n"
                   "goes where standard output goes.\n",
                   su_name, su_name);
            return 0;
        }
        if (c == SU_VERSION_OPT) return su_version();
        su_try_help(125);
    }
    if (su_optind == argc) {
        su_error(0, 0, "missing operand");
        su_try_help(125);
    }
    int in_tty = isatty(0), out_tty = isatty(1), err_tty = isatty(2);
    /* messages go to the standard error nohup was given, kept aside */
    int saved_err = err_tty ? dup(2) : 2;
    if (in_tty) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd < 0 || dup2(fd, 0) < 0) su_error(125, errno, "failed to render standard input unusable");
        if (fd > 0) close(fd);
        if (!out_tty && !err_tty) su_error(0, 0, "ignoring input");
    }
    const char *outname = 0;
    if (out_tty) {
        const char *name = "nohup.out";
        int fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0600);
        static char home_out[4096];
        if (fd < 0) {
            int e = errno;
            const char *home = getenv("HOME");
            if (home) {
                snprintf(home_out, sizeof home_out, "%s/nohup.out", home);
                name = home_out;
                fd = open(name, O_WRONLY | O_CREAT | O_APPEND, 0600);
            }
            if (fd < 0) {
                su_error(0, e, "failed to open %s", su_quoteaf("nohup.out"));
                if (home) su_error(0, errno, "failed to open %s", su_quoteaf(home_out));
                return 125;
            }
        }
        dup2(fd, 1);
        close(fd);
        outname = name;
        fprintf(stderr, "%s: %s %s\n", su_short,
                in_tty ? "ignoring input and appending output to"
                       : "appending output to", su_quoteaf(outname));
    }
    if (err_tty) {
        if (!out_tty)
            fprintf(stderr, "%s: %s\n", su_short,
                    in_tty ? "ignoring input and redirecting stderr to stdout"
                           : "redirecting stderr to stdout");
        dup2(1, 2);
    }
    signal(SIGHUP, SIG_IGN);
    char **cmd = argv + su_optind;
    execvp(cmd[0], cmd);
    int e = errno;
    /* the message on the standard error nohup was given */
    if (saved_err != 2) dup2(saved_err, 2);
    su_error(0, e, "failed to run command %s", su_quoteaf(cmd[0]));
    return e == ENOENT ? 127 : 126;
}
