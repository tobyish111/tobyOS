/* su_install.c -- install; included by main.c. */

static int copy_one(const char *src, const char *dst, int mode) {
    FILE *in = fopen(src, "rb");
    if (!in) { fprintf(stderr, "install: %s: %s\n", src, strerror(errno)); return 1; }
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); fprintf(stderr, "install: %s: %s\n", dst, strerror(errno)); return 1; }
    char b[8192];
    size_t n;
    while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
    fclose(in);
    fclose(out);
    if (mode >= 0) chmod(dst, (mode_t)mode);
    return 0;
}

static int do_install(int argc, char **argv) {
    int mode = 0755, dirs = 0;
    const char *args[64];
    int n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) {
            mode = (int)strtol(argv[++i], NULL, 8);
            continue;
        }
        if (!strcmp(argv[i], "-d")) { dirs = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        if (n < 64) args[n++] = argv[i];
    }
    if (n == 0) { fprintf(stderr, "usage: install [-m mode] src dst | -d dir...\n"); return 1; }
    if (dirs) {
        /* `install -d a/b/c` creates EVERY component, as GNU does -- that
         * is the whole reason a Makefile reaches for it over mkdir. */
        int rc = 0;
        for (int i = 0; i < n; i++) {
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%s", args[i]);
            for (char *q = tmp + 1; *q; q++) {
                if (*q != '/') continue;
                *q = 0;
                mkdir(tmp, (mode_t)mode);
                *q = '/';
            }
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) {
                fprintf(stderr, "install: %s: %s\n", args[i], strerror(errno));
                rc = 1;
            }
        }
        return rc;
    }
    if (n < 2) { fprintf(stderr, "install: need a source and a destination\n"); return 1; }
    struct stat st;
    if (stat(args[n-1], &st) == 0 && S_ISDIR(st.st_mode)) {
        int rc = 0;
        for (int i = 0; i < n - 1; i++) {
            const char *b = args[i];
            for (const char *q = args[i]; *q; q++) if (*q == '/') b = q + 1;
            char dst[1024];
            snprintf(dst, sizeof dst, "%s/%s", args[n-1], b);
            if (copy_one(args[i], dst, mode)) rc = 1;
        }
        return rc;
    }
    return copy_one(args[0], args[n-1], mode);
}
