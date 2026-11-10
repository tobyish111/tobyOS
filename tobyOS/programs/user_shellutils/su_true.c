/* su_true.c -- true and false, as GNU coreutils' (logs/utilgate/true.sh,
 * false.sh): nothing, with status 0 or 1. --help or --version is read
 * only as the one argument, spelled out (not "--he"), and even then the
 * status is the command's: `false --help` prints the help and fails. They
 * are the /bin binaries for `env true` and for whatever execs by name;
 * tsh's builtins answer first in a shell. */

static int do_true(int argc, char **argv, int status) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        printf("Usage: %s [ignored command line arguments]\n"
               "  or:  %s OPTION\n"
               "Do nothing, %s.\n"
               "\n"
               "      --help     show this help and exit\n"
               "      --version  show the version and exit\n"
               "\n"
               "Your shell may have its own %s, which this is not; run this one\n"
               "as env %s, or by its path.\n",
               su_name, su_name, status ? "unsuccessfully" : "successfully", su_short, su_short);
    } else if (argc == 2 && !strcmp(argv[1], "--version")) {
        su_version();
    }
    return status;
}
