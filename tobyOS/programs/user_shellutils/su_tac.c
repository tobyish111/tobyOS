/* su_tac.c -- tac; included by main.c. */

static int do_tac(int argc, char **argv) {
    int rc = 0;
    int nfiles = 0;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-' || !argv[i][1]) nfiles++;
    for (int pass = 0; pass < (nfiles ? nfiles : 1); pass++) {
        const char *path = NULL;
        int seen = 0;
        for (int i = 1; i < argc && nfiles; i++) {
            if (argv[i][0] == '-' && argv[i][1]) continue;
            if (seen++ == pass) { path = argv[i]; break; }
        }
        FILE *f = path ? fopen(path, "rb") : stdin;
        if (!f) { fprintf(stderr, "tac: %s: %s\n", path, strerror(errno)); rc = 1; continue; }
        char **lines = NULL;
        size_t n = 0, cap = 0;
        char buf[8192];
        while (fgets(buf, sizeof buf, f)) {
            if (n == cap) { cap = cap ? cap * 2 : 256;
                            lines = (char **)realloc(lines, cap * sizeof(char *)); }
            lines[n++] = strdup(buf);
        }
        if (f != stdin) fclose(f);
        for (size_t i = n; i > 0; i--) {
            char *l = lines[i - 1];
            size_t L = strlen(l);
            /* A last line with no newline gains one when it moves up, and
             * the line that ends up last loses it -- which is what tac
             * does, and why it is not just "print backwards". */
            if (L && l[L-1] == '\n') fputs(l, stdout);
            else { fputs(l, stdout); if (i != 1) putchar('\n'); }
            free(l);
        }
        free(lines);
    }
    return rc;
}
