/* su_mkfifo.c -- mkfifo; included by main.c. */

/* mkfifo [-m MODE] NAME... -- GNU coreutils': each NAME a FIFO, 0666
 * less the umask; with -m, MODE exactly (octal, or symbolic from a=rw).
 * It refused: there were no FIFOs (finding 96). */
static int fifo_mode(const char *s, mode_t *out) {
    if (*s >= '0' && *s <= '7') {
        char *e;
        long v = strtol(s, &e, 8);
        if (*e || v < 0 || v > 07777) return -1;
        *out = (mode_t)v;
        return 0;
    }
    mode_t m = 0666;
    while (*s) {
        mode_t who = 0;
        for (; *s == 'u' || *s == 'g' || *s == 'o' || *s == 'a'; s++)
            who |= *s == 'u' ? 04700 : *s == 'g' ? 02070 : *s == 'o' ? 01007 : 07777;
        if (!who) who = 07777;
        char op = *s++;
        if (op != '+' && op != '-' && op != '=') return -1;
        mode_t bits = 0;
        for (; *s == 'r' || *s == 'w' || *s == 'x'; s++)
            bits |= *s == 'r' ? 0444 : *s == 'w' ? 0222 : 0111;
        bits &= who;
        if (op == '+') m |= bits;
        else if (op == '-') m &= ~bits;
        else m = (m & ~(who & 0777)) | bits;
        if (*s == ',') { s++; continue; }
        if (*s) return -1;
    }
    *out = m;
    return 0;
}

static int do_mkfifo(int argc, char **argv) {
    const char *mstr = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strncmp(a, "--mode=", 7)) { mstr = a + 7; continue; }
        if (a[0] == '-' && a[1] == 'm') {
            mstr = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : 0);
            if (!mstr) {
                fprintf(stderr, "mkfifo: option requires an argument -- 'm'\n"
                                "Try 'mkfifo --help' for more information.\n");
                return 1;
            }
            continue;
        }
        if (a[0] == '-' && a[1]) {
            fprintf(stderr, "mkfifo: invalid option -- '%c'\n"
                            "Try 'mkfifo --help' for more information.\n", a[1]);
            return 1;
        }
        break;
    }
    if (i >= argc) {
        fprintf(stderr, "mkfifo: missing operand\n"
                        "Try 'mkfifo --help' for more information.\n");
        return 1;
    }
    mode_t mode = 0666;
    if (mstr && fifo_mode(mstr, &mode) < 0) {
        fprintf(stderr, "mkfifo: invalid mode '%s'\n", mstr);
        return 1;
    }
    int rc = 0;
    for (; i < argc; i++) {
        if (mkfifo(argv[i], mode) != 0) {
            fprintf(stderr, "mkfifo: cannot create fifo '%s': %s\n",
                    argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        if (mstr) (void)chmod(argv[i], mode);   /* exactly MODE, not less the umask */
    }
    return rc;
}
