/* su_logname.c -- logname, as GNU coreutils': the name the user logged in
 * as (logs/utilgate/logname.sh). On Linux that is getlogin(): the session's
 * login uid (pam_loginuid, which sshd's login sets) and then the utmp entry
 * of the terminal; a process with neither is "no login name", status 1.
 * Here a login -- the console, the desktop, an SSH session -- runs as the
 * user it logged in, so it is the real uid's name; a uid without one is
 * "no login name". No operands. It looked at argv[1] alone, as arch did. */
static int do_logname(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s [OPTION]\n"
                   "Print the name the user logged in as.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name);
            return 0;
        }
        if (c == SU_VERSION_OPT) return su_version();
        su_try_help(1);
    }
    if (su_optind < argc) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
#ifdef SU_HOST
    const char *n = getlogin();
    if (!n) su_error(1, 0, "no login name");
    puts(n);
#else
    char name[1024];
    if (su_user_name(getuid(), name, sizeof name) != 0) su_error(1, 0, "no login name");
    puts(name);
#endif
    return 0;
}
