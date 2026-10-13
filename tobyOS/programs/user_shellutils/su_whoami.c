/* su_whoami.c -- whoami, as GNU coreutils': the effective user's name,
 * from the user database, not $USER -- whoami exists to be what an
 * environment variable cannot lie about. No operands ("extra operand");
 * a uid with no name is "cannot find name for user ID N", status 1.
 * Measured on coreutils 9.7: logs/utilgate/whoami.sh.
 *
 * It took the real uid, and any argument at all as nothing. */

/* The name /etc/passwd gives UID, into OUT; 0, or -1. */
static int su_user_name(uid_t uid, char *out, size_t cap) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[1024];
    int rc = -1;
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        char *end;
        unsigned long id = strtoul(c2 + 1, &end, 10);
        if (end == c2 + 1 || *end != ':' || id != (unsigned long)uid) continue;
        *c1 = 0;
        snprintf(out, cap, "%s", line);
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

static int do_whoami(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s [OPTION]...\n"
                   "Print the name of the effective user -- the one this\n"
                   "process runs as -- from the user database.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n",
                   su_name);
            return 0;
        }
        if (c == SU_VERSION_OPT) return su_version();
        su_try_help(1);
    }
    if (su_optind < argc) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    uid_t u = geteuid();
    char name[1024];
    if (su_user_name(u, name, sizeof name) != 0)
        su_error(1, 0, "cannot find name for user ID %lu", (unsigned long)u);
    puts(name);
    return 0;
}
