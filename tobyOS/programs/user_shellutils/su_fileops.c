/* su_fileops.c -- what cat, echo, mkdir, mknod, ln, chmod, tee and chroot
 * (batch 32's "fileops" group) share: a write that finishes or says why
 * not, a descriptor kept off 0-2 (gnulib's open_safer), GNU's close_stdout
 * at exit, GNU's argmatch for an option's word argument, and the mode
 * engine that chmod, mkdir and mknod read MODE with.
 *
 * Every su_<cmd>.c of the group includes this file itself, under a guard,
 * so the order main.c lists them in does not matter. Every name here
 * starts fo_.
 *
 * THE MODE ENGINE is GNU chmod's reading of a MODE, as measured on
 * coreutils 9.7 (logs/utilgate/staging/chmod.sh, mkdir.sh, mknod.sh), not
 * su_common.c's su_mode_compile, which has no [-+=]OCTAL form, no idea of
 * how many octal digits were written, and no record of which bits a MODE
 * names -- three things GNU's answers depend on:
 *
 *   - OCTAL alone sets every bit -- except that a directory keeps its
 *     set-user-ID and set-group-ID bits when fewer than five digits were
 *     written (`chmod 755 d` keeps a setgid d's setgid, `chmod 00755 d`
 *     clears it).
 *   - [ugoa]*OP... clauses, comma-separated, each OP followed by [rwxXst]*,
 *     by one of u g o (that class's bits as they are at that point), or,
 *     with no [ugoa], by OCTAL ("=644", "-022"; the umask does not apply
 *     to it and it is not a directory's symbolic '=').
 *   - A clause with no [ugoa] is masked by the umask; '=' clears what the
 *     clause names (all of it with no [ugoa]), but on a directory it does
 *     not clear set-user-ID or set-group-ID unless it sets them.
 *   - The bits a MODE names (fo_mode_adjust's *bits) -- what mkdir checks
 *     its new directory against: `mkdir -m +t d` names only the sticky
 *     bit, so a d the kernel made 1755 is left so, where `-m 1777` names
 *     all twelve and is chmodded to 1777. */
#ifndef FO_FILEOPS_INCLUDED
#define FO_FILEOPS_INCLUDED

/* write(2) until all of BUF is out: 0, or -1 with errno set */
static int fo_write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (w == 0) { errno = ENOSPC; return -1; }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* FD moved above 2, as gnulib's open_safer leaves one: a file opened while
 * stdin, stdout or stderr is closed must not become that stream */
static int fo_safer(int fd) {
    if (fd < 0 || fd > 2) return fd;
    int nfd = fo_safer(dup(fd));
    int e = errno;
    close(fd);
    errno = e;
    return nfd;
}

/* GNU's close_stdout, run at exit as GNU's is: what stdout still holds
 * written, and a failure -- now, or of an earlier write -- is "PROG:
 * write error[: ERR]", status fo_exit_failure (1; chroot's 125). Nothing
 * written to a closed stdout is no error. */
static int fo_exit_failure = 1;

static void fo_close_stdout_now(void) {
    int failed = fflush(stdout) != 0;
    int e = errno;
    if (!failed && !ferror(stdout)) return;
    fprintf(stderr, "%s: write error", su_short);
    if (failed && e) fprintf(stderr, ": %s", strerror(e));
    fputc('\n', stderr);
    fflush(stderr);
    _exit(fo_exit_failure);
}

static void fo_stdout_guard(void) {
    static int armed;
    if (!armed) {
        armed = 1;
        atexit(fo_close_stdout_now);
    }
}

/* ---- argmatch ----------------------------------------------------------------
 *
 * An option's word, as GNU's argmatch takes it: the whole word, or a prefix
 * of one word, or a prefix of several that all mean the same. Anything else
 * is "invalid argument 'X' for '--OPT'" (or "ambiguous argument"), the
 * valid words listed one meaning to a line, synonyms side by side, then
 * "Try ... --help", status 1. CONTEXT is the option as GNU names it. */
static int fo_argmatch(const char *context, const char *arg,
                       const char *const *names, const int *vals) {
    size_t len = strlen(arg);
    int found = -1, ambiguous = 0;
    for (int i = 0; names[i]; i++) {
        if (strncmp(names[i], arg, len)) continue;
        if (strlen(names[i]) == len) return vals[i];
        if (found < 0) found = i;
        else if (vals[i] != vals[found]) ambiguous = 1;
    }
    if (found >= 0 && !ambiguous) return vals[found];
    su_error(0, 0, "%s argument %s for %s", ambiguous ? "ambiguous" : "invalid",
             su_quote(arg), su_quote(context));
    fputs("Valid arguments are:", stderr);
    for (int i = 0; names[i]; i++) {
        if (i == 0 || vals[i] != vals[i - 1]) fprintf(stderr, "\n  - %s", su_quote(names[i]));
        else fprintf(stderr, ", %s", su_quote(names[i]));
    }
    fputc('\n', stderr);
    su_try_help(1);
}

/* ---- modes ------------------------------------------------------------------- */

#define FO_MODE_MAX 128

struct fo_action {
    char op;            /* '=', '+', '-' */
    char copy;          /* 'u', 'g', 'o', or 0 */
    char xifany;        /* X: x for a directory, or for a file with any x */
    char numeric;       /* OCTAL after the operator, or the whole MODE */
    mode_t who;         /* [ugoa] bits; 0 when none were written */
    mode_t value;       /* the bits named */
};

struct fo_mode {
    int n;
    int octal_digits;   /* the whole MODE is octal: how many digits; else 0 */
    struct fo_action a[FO_MODE_MAX];
};

/* 0, or -1 when S is not a MODE */
static int fo_mode_compile(const char *s, struct fo_mode *m) {
    m->n = 0;
    m->octal_digits = 0;
    if (*s >= '0' && *s <= '7') {
        unsigned long v = 0;
        const char *p = s;
        for (; *p >= '0' && *p <= '7'; p++) {
            v = v * 8 + (unsigned long)(*p - '0');
            if (v > 07777) return -1;
        }
        if (*p) return -1;
        m->octal_digits = (int)(p - s);
        m->n = 1;
        m->a[0].op = '=';
        m->a[0].copy = 0;
        m->a[0].xifany = 0;
        m->a[0].numeric = 1;
        m->a[0].who = 07777;
        m->a[0].value = (mode_t)v;
        return 0;
    }
    const char *p = s;
    for (;;) {
        mode_t who = 0;
        for (;; p++) {
            if (*p == 'u') who |= 04700;
            else if (*p == 'g') who |= 02070;
            else if (*p == 'o') who |= 01007;
            else if (*p == 'a') who |= 07777;
            else break;
        }
        if (*p != '=' && *p != '+' && *p != '-') return -1;
        while (*p == '=' || *p == '+' || *p == '-') {
            if (m->n >= FO_MODE_MAX) return -1;
            struct fo_action *a = &m->a[m->n++];
            a->op = *p++;
            a->copy = 0;
            a->xifany = 0;
            a->numeric = 0;
            a->who = who;
            a->value = 0;
            if (*p >= '0' && *p <= '7') {
                /* [-+=]OCTAL: only with no [ugoa] before it */
                if (who) return -1;
                unsigned long v = 0;
                for (; *p >= '0' && *p <= '7'; p++) {
                    v = v * 8 + (unsigned long)(*p - '0');
                    if (v > 07777) return -1;
                }
                a->numeric = 1;
                a->who = 07777;
                a->value = (mode_t)v;
                /* nothing may follow the number but the next clause */
                if (*p && *p != ',') return -1;
                break;
            }
            if (*p == 'u' || *p == 'g' || *p == 'o') {
                a->copy = *p++;
            } else {
                for (;; p++) {
                    if (*p == 'r') a->value |= 0444;
                    else if (*p == 'w') a->value |= 0222;
                    else if (*p == 'x') a->value |= 0111;
                    else if (*p == 'X') a->xifany = 1;
                    else if (*p == 's') a->value |= 06000;
                    else if (*p == 't') a->value |= 01000;
                    else break;
                }
            }
        }
        if (*p == ',') { p++; continue; }
        if (*p) return -1;
        return 0;
    }
}

/* What MODE makes of a file whose mode is OLD (a directory when IS_DIR),
 * with the umask UMASK_V; *BITS, if asked, gets the bits MODE names. */
static mode_t fo_mode_adjust(mode_t old, int is_dir, mode_t umask_v,
                             const struct fo_mode *m, mode_t *bits) {
    mode_t nm = old & 07777, named = 0;
    if (m->octal_digits) {
        nm = m->a[0].value;
        if (is_dir && m->octal_digits < 5) nm |= old & 06000;
        if (bits) *bits = 07777;
        return nm;
    }
    for (int i = 0; i < m->n; i++) {
        const struct fo_action *a = &m->a[i];
        mode_t value = a->value, mask;
        if (a->copy) {
            mode_t c = a->copy == 'u' ? (nm >> 6) & 7
                     : a->copy == 'g' ? (nm >> 3) & 7 : nm & 7;
            value = (mode_t)(c << 6 | c << 3 | c);
        }
        if (a->xifany && (is_dir || (nm & 0111))) value |= 0111;
        if (a->who) {
            mask = a->who;
            value &= a->who;
        } else {
            mask = 07777;
            value &= ~umask_v;
        }
        switch (a->op) {
        case '=': {
            mode_t clear = mask;
            /* a directory's set-id bits survive a symbolic '=' */
            if (is_dir && !a->numeric) clear &= ~(mode_t)06000;
            nm = (nm & ~clear) | value;
            named |= mask;
            break;
        }
        case '+': nm |= value; named |= value; break;
        case '-': nm &= ~value; named |= value; break;
        }
    }
    if (bits) *bits = named & 07777;
    return nm & 07777;
}

/* MODE's permission bits as ls shows them: "rwxr-sr-T" */
static const char *fo_perm_string(mode_t m, char *b) {
    b[0] = (m & 0400) ? 'r' : '-';
    b[1] = (m & 0200) ? 'w' : '-';
    b[2] = (m & 04000) ? ((m & 0100) ? 's' : 'S') : ((m & 0100) ? 'x' : '-');
    b[3] = (m & 040) ? 'r' : '-';
    b[4] = (m & 020) ? 'w' : '-';
    b[5] = (m & 02000) ? ((m & 010) ? 's' : 'S') : ((m & 010) ? 'x' : '-');
    b[6] = (m & 04) ? 'r' : '-';
    b[7] = (m & 02) ? 'w' : '-';
    b[8] = (m & 01000) ? ((m & 01) ? 't' : 'T') : ((m & 01) ? 'x' : '-');
    b[9] = 0;
    return b;
}

/* the umask, read without changing it */
static mode_t fo_get_umask(void) {
    mode_t u = umask(0);
    umask(u);
    return u & 0777;
}

#endif /* FO_FILEOPS_INCLUDED */
