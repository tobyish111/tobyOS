/* su_link.c -- link and unlink, as GNU coreutils': the bare system calls
 * (logs/utilgate/link.sh, unlink.sh). link takes exactly two operands
 * ("missing operand", "missing operand after 'A'", "extra operand"),
 * unlink exactly one; a failure is "cannot create link 'B' to 'A': ERR" or
 * "cannot unlink 'F': ERR", status 1. No options but --help/--version
 * (and "--"). Both looked at argv alone before: `link -x a b` was an extra
 * operand, `unlink --` a file named --. The kernel's answers they print
 * changed too: a directory is EPERM to link(2) and EISDIR to unlink(2) --
 * unlink(2) used to remove an empty directory (batch 30). */
static int lk_options(int argc, char **argv, const char *usage, const char *what) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s %s\n%s\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name, usage, what);
            exit(0);
        }
        if (c == SU_VERSION_OPT) exit(su_version());
        su_try_help(1);
    }
    return su_optind;
}

static int do_link(int argc, char **argv) {
    int i = lk_options(argc, argv, "FILE1 FILE2",
                       "Make FILE2 a new name for FILE1 -- a hard link, as link(2) makes one.");
    int n = argc - i;
    if (n < 1) { su_error(0, 0, "missing operand"); su_try_help(1); }
    if (n < 2) { su_error(0, 0, "missing operand after %s", su_quote(argv[i])); su_try_help(1); }
    if (n > 2) { su_error(0, 0, "extra operand %s", su_quote(argv[i + 2])); su_try_help(1); }
    if (link(argv[i], argv[i + 1]) != 0)
        su_error(1, errno, "cannot create link %s to %s",
                 su_quote(argv[i + 1]), su_quote(argv[i]));
    return 0;
}

static int do_unlink(int argc, char **argv) {
    int i = lk_options(argc, argv, "FILE",
                       "Remove the name FILE, as unlink(2) does: not a directory.");
    int n = argc - i;
    if (n < 1) { su_error(0, 0, "missing operand"); su_try_help(1); }
    if (n > 1) { su_error(0, 0, "extra operand %s", su_quote(argv[i + 1])); su_try_help(1); }
    if (unlink(argv[i]) != 0) su_error(1, errno, "cannot unlink %s", su_quote(argv[i]));
    return 0;
}
