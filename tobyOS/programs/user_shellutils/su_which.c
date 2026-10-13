/* su_which.c -- which, as debianutils' (the which a Debian or Ubuntu user
 * has): for each NAME, the first executable regular file DIR/NAME along
 * $PATH -- every one with -a, nothing printed with -s. An empty PATH
 * entry is the current directory, printed as "./NAME". A NAME holding a
 * '/' is itself checked and printed as given. Status 0 when every NAME
 * was found, 1 when one was not or there was no NAME, 2 for a bad option:
 * "Illegal option -x" (dash's getopts words, on stderr) then "Usage:
 * WHICH [-as] args" on stdout. Options stop at the first NAME.
 * Measured on debianutils 5.x: logs/utilgate/which.sh.
 *
 * PATH IS SPLIT AS THE SCRIPT'S SHELL SPLITS IT: a single trailing ':'
 * is doubled first (so "a:" has its empty last entry), then IFS=:
 * splitting -- every ':' ends an entry, empty ones included, and nothing
 * after the last ':' is no entry at all.
 *
 * It found directories, took -s as a name, skipped anything starting with
 * '-' anywhere, and lost every empty PATH entry. */

/* an executable regular file, as test -f F && test -x F */
static int which_exec(const char *f) {
    struct stat st;
    if (stat(f, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    uid_t u = geteuid();
    if (u == 0) return (st.st_mode & 0111) != 0;
    if (st.st_uid == u) return (st.st_mode & 0100) != 0;
    if (st.st_gid == getegid()) return (st.st_mode & 0010) != 0;
    return (st.st_mode & 0001) != 0;
}

/* CAND if it is one, said unless SILENT; 1 if it was */
static int which_try(const char *dir, size_t dl, const char *name, int silent) {
    char *cand = (char *)malloc(dl + strlen(name) + 3);
    if (!cand) su_error(1, ENOMEM, "memory exhausted");
    if (dl == 0) sprintf(cand, "./%s", name);
    else sprintf(cand, "%.*s/%s", (int)dl, dir, name);
    int ok = which_exec(cand);
    if (ok && !silent) puts(cand);
    free(cand);
    return ok;
}

static int do_which(int argc, char **argv) {
    int all = 0, silent = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) { i++; break; }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'a') all = 1;
            else if (*p == 's') silent = 1;
            else {
                fflush(stdout);
                fprintf(stderr, "Illegal option -%c\n", *p);
                printf("Usage: %s [-as] args\n", argv[0]);
                return 2;
            }
        }
    }
    int rc = i == argc ? 1 : 0;
    const char *env = getenv("PATH");
    size_t el = env ? strlen(env) : 0;
    char *path = (char *)malloc(el + 2);
    if (!path) su_error(1, ENOMEM, "memory exhausted");
    memcpy(path, env ? env : "", el + 1);
    if (el >= 2 && path[el - 1] == ':' && path[el - 2] != ':') {
        path[el] = ':';
        path[el + 1] = 0;
    }
    for (; i < argc; i++) {
        const char *name = argv[i];
        int found = 0;
        if (strchr(name, '/')) {
            found = which_exec(name);
            if (found && !silent) puts(name);
        } else {
            const char *p = path;
            while (*p) {
                const char *e = strchr(p, ':');
                size_t dl = e ? (size_t)(e - p) : strlen(p);
                if (which_try(p, dl, name, silent)) {
                    found = 1;
                    if (!all) break;
                }
                if (!e) break;
                p = e + 1;
            }
        }
        if (!found) rc = 1;
    }
    free(path);
    return rc;
}
