/* su_printenv.c -- printenv; included by main.c. */

extern char **environ;

static int do_printenv(int argc, char **argv) {
    if (argc > 1) {
        int rc = 1;
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1]) continue;
            const char *v = getenv(argv[i]);
            if (v) { printf("%s\n", v); rc = 0; }
        }
        return rc;
    }
    for (char **e = environ; e && *e; e++) printf("%s\n", *e);
    return 0;
}
