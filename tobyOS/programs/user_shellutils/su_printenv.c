/* su_printenv.c -- printenv, as GNU coreutils': with no operand the whole
 * environment as NAME=VALUE; else each named variable's value, in the
 * order named. -0/--null ends each with a NUL instead of a newline.
 * Status 0 when every name was found, 1 when one was not (a name with
 * '=' in it never is), 2 for a usage error. Measured on coreutils 9.7:
 * logs/utilgate/printenv.sh.
 *
 * It had no -0, skipped anything starting with '-' as an option without
 * a word, and found "A=1" as a variable. */

extern char **environ;

static int do_printenv(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "null", SU_NO_ARG, 0, '0' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int nul = 0, c;
    while ((c = su_getopt_long(argc, argv, "0", longs, 0)) != -1) {
        switch (c) {
        case '0': nul = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [VARIABLE]...\n"
                   "Print the value of each VARIABLE named, or with none, every\n"
                   "variable in the environment as NAME=VALUE.\n"
                   "\n"
                   "  -0, --null     end each output item with a NUL, not a newline\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(2);
        }
    }
    char end = nul ? '\0' : '\n';
    if (su_optind == argc) {
        for (char **e = environ; e && *e; e++) {
            fputs(*e, stdout);
            putchar(end);
        }
        return 0;
    }
    int found = 0;
    for (int i = su_optind; i < argc; i++) {
        const char *name = argv[i];
        if (strchr(name, '=')) continue;
        size_t n = strlen(name);
        for (char **e = environ; e && *e; e++) {
            if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') {
                fputs(*e + n + 1, stdout);
                putchar(end);
                found++;
                break;
            }
        }
    }
    return found == argc - su_optind ? 0 : 1;
}
