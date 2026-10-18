/* su_rev.c -- rev, as util-linux 2.41.3's (logs/utilgate/rev.sh):
 * each line of each FILE (or standard input) written backwards, its
 * terminator still last; -0 makes NUL the terminator.
 *
 * Measured in the C locale: util-linux reads wide characters, and there
 * a byte past 0x7f is not one -- "fgetwc() failed: Invalid or incomplete
 * multibyte or wide character" (glibc's words, written out here: musl's
 * and libtoby's say "Illegal byte sequence"), the line it was in is lost
 * and nothing more is read. A file that cannot be read (a directory) is
 * reported the same way. One that cannot be opened is "cannot open
 * NAME: ERROR" and the next is tried. "-" is a file name, not standard
 * input. A last line with no terminator is written without one. The
 * help is util-linux's layout (a blank line, then "Usage:"); -V is the
 * version.
 *
 * The old rev reversed lines of standard input only and had no -0. */

static int do_rev(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "zero", SU_NO_ARG, 0, '0' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int sep = '\n', c;
    while ((c = su_getopt_long(argc, argv, "0hV", longs, 0)) != -1) {
        switch (c) {
        case '0': sep = 0; break;
        case 'h':
            printf("\n"
                   "Usage:\n"
                   " %s [options] [<file> ...]\n"
                   "\n"
                   "Write every line backwards, character by character.\n"
                   "\n"
                   "Options:\n"
                   " -0, --zero     lines end in NUL, not newline\n"
                   " -h, --help     show this help\n"
                   " -V, --version  show the version\n", su_short);
            return 0;
        case 'V': return su_version();
        default: su_try_help(1);
        }
    }
    static char *none[] = { 0, 0 };
    char **names = argv + su_optind;
    int n = argc - su_optind;
    if (n <= 0) { names = none; n = 1; }
    char *line = 0;
    size_t len = 0, cap = 0;
    static unsigned char buf[16384];
    int status = 0;
    for (int i = 0; i < n; i++) {
        int fd = 0;
        if (names[i]) {
            fd = su_open_read(names[i], 0);
            if (fd < 0) {
                int e = errno;
                fflush(stdout);
                fprintf(stderr, "%s: cannot open %s: %s\n", su_short, names[i], strerror(e));
                status = 1;
                continue;
            }
        }
        len = 0;
        for (;;) {
            ssize_t r = read(fd, buf, sizeof buf);
            if (r < 0 && errno == EINTR) continue;
            const char *why = 0;
            if (r < 0) why = strerror(errno);
            for (ssize_t k = 0; !why && k < r; k++) {
                unsigned char b = buf[k];
                if (b >= 0x80) { why = "Invalid or incomplete multibyte or wide character"; break; }
                if (b == (unsigned char)sep) {
                    for (size_t j = len; j > 0; j--) putchar(line[j - 1]);
                    putchar(sep);
                    len = 0;
                    continue;
                }
                if (len == cap) {
                    cap = cap ? cap * 2 : 256;
                    line = (char *)realloc(line, cap);
                    if (!line) su_error(1, ENOMEM, "memory exhausted");
                }
                line[len++] = (char)b;
            }
            if (why) {
                fflush(stdout);
                fprintf(stderr, "%s: fgetwc() failed: %s\n", su_short, why);
                if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
                exit(1);
            }
            if (r == 0) break;
        }
        for (size_t j = len; j > 0; j--) putchar(line[j - 1]);
        if (fd > 0) close(fd);
    }
    free(line);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return status;
}
