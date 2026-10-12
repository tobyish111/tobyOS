/* su_truncate.c -- truncate; included by main.c. */

static long size_arg(const char *s) {
    long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == 'K' || *p == 'k') v *= 1024L;
    else if (*p == 'M' || *p == 'm') v *= 1024L * 1024L;
    else if (*p == 'G' || *p == 'g') v *= 1024L * 1024L * 1024L;
    return v;
}

static int do_truncate(int argc, char **argv) {
    long size = -1;
    int rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) { size = size_arg(argv[++i]); continue; }
        if (!strncmp(argv[i], "-s", 2) && argv[i][2]) { size = size_arg(argv[i] + 2); continue; }
        if (!strncmp(argv[i], "--size=", 7)) { size = size_arg(argv[i] + 7); continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        any = 1;
        if (size < 0) { fprintf(stderr, "truncate: -s is required\n"); return 1; }
        /* Created if absent, as truncate does. */
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd >= 0) close(fd);
        if (truncate(argv[i], (off_t)size) != 0) {
            fprintf(stderr, "truncate: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    if (!any) { fprintf(stderr, "usage: truncate -s SIZE file...\n"); return 1; }
    return rc;
}
