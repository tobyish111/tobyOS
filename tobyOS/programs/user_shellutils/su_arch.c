/* su_arch.c -- arch, as GNU coreutils': the machine's hardware name, what
 * `uname -m` prints. No operands ("extra operand"); options are GNU's
 * (logs/utilgate/arch.sh). It looked at argv[1] alone, so `arch -x` was an
 * extra operand and nothing named --help. */
static int do_arch(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s [OPTION]...\n"
                   "Print the machine's hardware name, as uname -m does.\n"
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
    puts("x86_64");
    return 0;
}
