/* su_whoami.c -- whoami; included by main.c. */

static int do_whoami(void) {
    /* The name behind the uid, from the identity database, not $USER --
     * `whoami` exists precisely to be the thing an environment variable
     * cannot lie about. */
    uid_t u = getuid();
    char buf[8192];
    if (slurp("/etc/passwd", buf, sizeof buf) > 0) {
        for (char *p = buf; p && *p; ) {
            char *eol = strchr(p, '\n');
            if (eol) *eol = 0;
            char *n1 = strchr(p, ':');
            if (n1) {
                char *n2 = strchr(n1 + 1, ':');
                if (n2) {
                    long id = atol(n2 + 1);
                    if (id == (long)u) { *n1 = 0; printf("%s\n", p); return 0; }
                }
            }
            if (!eol) break;
            p = eol + 1;
        }
    }
    printf("%u\n", (unsigned)u);
    return 0;
}
