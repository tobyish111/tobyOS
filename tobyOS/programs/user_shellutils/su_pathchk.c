/* su_pathchk.c -- pathchk, as GNU coreutils 9.7's (logs/utilgate/
 * pathchk.sh): each NAME checked as a file name, one message for the
 * first thing wrong with it, status 1 when any NAME failed.
 *
 * What was measured, in the C locale:
 *
 *   - Options stop at the first operand (`pathchk a -p 'a b'` checks a
 *     file named -p); "--" after an operand is an operand.
 *   - -P: an empty NAME is "empty file name"; a component that starts
 *     with '-' (the first byte, or one after a '/') is "leading '-' in a
 *     component of file name 'NAME'". Checked before -p's tests.
 *   - -p: empty is "empty file name"; the first byte outside
 *     [A-Za-z0-9._/-] is "non-portable character 'C' in file name
 *     'NAME'"; then the whole length against 255 (POSIX's 256 less the
 *     NUL): "limit 255 exceeded by length N of file name 'NAME'"; then
 *     each component against 14: "limit 14 exceeded by length N of file
 *     name component 'COMP'". Nothing is looked up.
 *   - without -p, NAME is looked up (lstat): one that exists passes;
 *     any failure but ENOENT is reported as "NAME: strerror" (EACCES
 *     under a directory without search permission, ENOTDIR, ELOOP,
 *     ENAMETOOLONG for a component too long for an existing directory
 *     or a name of 4096 bytes or more) -- and so is ENOENT for an empty
 *     NAME ("'': No such file or directory"). For a name that does not
 *     exist the components are measured against the file system's 255.
 *
 * Quoting, measured byte by byte: the character is quoted as GNU's
 * quote() (C-style escapes, octal for every byte outside ASCII's
 * printable range), components the same way; whole names in "limit"
 * and "leading" messages always quoted shell-style, and in the strerror
 * messages shell-style only when they need it (a ':' does). The old
 * pathchk had none of the -p limits. */

/* ---- GNU's quoting, C locale ---------------------------------------------- */

/* a rotating set of heap buffers: names here can be thousands of bytes */
static char *fm_pc_qb[4];
static int fm_pc_qslot;

static char *fm_pc_qalloc(size_t need) {
    int i = fm_pc_qslot++ & 3;
    free(fm_pc_qb[i]);
    fm_pc_qb[i] = (char *)malloc(need);
    if (!fm_pc_qb[i]) su_error(1, ENOMEM, "memory exhausted");
    return fm_pc_qb[i];
}

/* a byte that is never written raw: controls and everything past ASCII */
static int fm_pc_ctl(unsigned char c) { return c < 0x20 || c >= 0x7f; }

/* the escape GNU writes for a control byte: \a ... \r by name, octal else */
static size_t fm_pc_esc(char *o, unsigned char c) {
    static const char names[] = "abtnvfr";
    if (c >= 7 && c <= 13) { o[0] = '\\'; o[1] = names[c - 7]; return 2; }
    o[0] = '\\';
    o[1] = (char)('0' + ((c >> 6) & 3));
    o[2] = (char)('0' + ((c >> 3) & 7));
    o[3] = (char)('0' + (c & 7));
    return 4;
}

/* quote(): 'TEXT' with \' and \\, and the escapes above. N bytes of S. */
static const char *fm_pc_quote_n(const char *s, size_t n) {
    char *b = fm_pc_qalloc(n * 4 + 3), *o = b;
    *o++ = '\'';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\'' || c == '\\') { *o++ = '\\'; *o++ = (char)c; }
        else if (fm_pc_ctl(c)) o += fm_pc_esc(o, c);
        else *o++ = (char)c;
    }
    *o++ = '\'';
    *o = 0;
    return b;
}

/* Shell-escape quoting. ALWAYS: quote even a name that needs nothing
 * (quoteaf); else a name of only safe bytes is bare (quotef, whose
 * flavour also quotes a ':'). Measured choices: a name holding a single
 * quote and otherwise only letters, digits, blanks and "%+,-./:@]_" --
 * and '#' or '~' first -- is put in double quotes; anything else is in
 * single quotes, a quote written '\'' and a run of control bytes as
 * '$'...'' escapes. */
static const char *fm_pc_shquote(const char *s, int always) {
    size_t n = strlen(s);
    int needs = (n == 0), squote = 0, dq_ok = 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int safe = isalnum(c) || (c && strchr("%+,-./@]_{}", c)) ||
                   ((c == '#' || c == '~') && i > 0);
        if (c == '\'') squote = 1;
        else if (!(isalnum(c) || (c && strchr(" %+,-./:@]_", c)) ||
                   ((c == '#' || c == '~') && i == 0)))
            dq_ok = 0;
        if (!safe) needs = 1;
    }
    if (!needs && !always) {
        char *b = fm_pc_qalloc(n + 1);
        memcpy(b, s, n + 1);
        return b;
    }
    char *b = fm_pc_qalloc(n * 7 + 8), *o = b;
    if (squote && dq_ok) {
        *o++ = '"';
        memcpy(o, s, n);
        o += n;
        *o++ = '"';
        *o = 0;
        return b;
    }
    int dollar = 0;                 /* inside a $'...' run */
    *o++ = '\'';
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (fm_pc_ctl(c)) {
            if (!dollar) { memcpy(o, "'$'", 3); o += 3; dollar = 1; }
            o += fm_pc_esc(o, c);
            continue;
        }
        /* a quote closes whichever quoting is open, then \' and a new
         * '...'; anything else after a $'...' run needs the new '...' */
        if (c == '\'') { memcpy(o, "'\\''", 4); o += 4; dollar = 0; continue; }
        if (dollar) { memcpy(o, "''", 2); o += 2; dollar = 0; }
        *o++ = (char)c;
    }
    *o++ = '\'';
    *o = 0;
    return b;
}

/* ---- the checks --------------------------------------------------------- */

#define FM_PC_POSIX_NAME 14     /* _POSIX_NAME_MAX */
#define FM_PC_POSIX_PATH 256    /* _POSIX_PATH_MAX, the NUL included */
#define FM_PC_NAME_MAX 255      /* Linux's, which tobyOS's names follow */
#define FM_PC_PATH_MAX 4096

#ifndef SU_HOST
/* What Linux's lookup answers that the guest's may not: ENAMETOOLONG for
 * a name of PATH_MAX bytes or more, and for a component longer than
 * NAME_MAX that the walk reaches -- every directory before it exists.
 * Only asked when some limit is passed, so a normal name costs nothing. */
static int fm_pc_linux_toolong(const char *file) {
    size_t len = strlen(file);
    if (len >= FM_PC_PATH_MAX) return 1;
    const char *p = file;
    int any = 0;
    for (const char *q = file; *q;) {
        size_t cl = strcspn(q, "/");
        if (cl > FM_PC_NAME_MAX) any = 1;
        q += cl;
        while (*q == '/') q++;
    }
    if (!any) return 0;
    char *pre = (char *)malloc(len + 2);
    if (!pre) return 0;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        size_t cl = strcspn(p, "/");
        if (cl > FM_PC_NAME_MAX) {
            /* the directory it would be looked up in */
            size_t pl = (size_t)(p - file);
            struct stat st;
            int reached;
            if (pl == 0) reached = 1;
            else {
                memcpy(pre, file, pl);
                pre[pl] = 0;
                reached = stat(pre, &st) == 0 && S_ISDIR(st.st_mode);
            }
            free(pre);
            return reached;
        }
        p += cl;
    }
    free(pre);
    return 0;
}
#endif

/* 1 when FILE passes */
static int fm_pc_check(const char *file, int basic, int extra) {
    size_t len = strlen(file);
    if (extra) {
        if (!len) { su_error(0, 0, "empty file name"); return 0; }
        for (size_t i = 0; i < len; i++)
            if (file[i] == '-' && (i == 0 || file[i - 1] == '/')) {
                su_error(0, 0, "leading '-' in a component of file name %s",
                         fm_pc_shquote(file, 1));
                return 0;
            }
    }
    size_t maxpath, maxname;
    if (basic) {
        if (!len) { su_error(0, 0, "empty file name"); return 0; }
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)file[i];
            if (!(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '/') || c >= 0x80) {
                su_error(0, 0, "non-portable character %s in file name %s",
                         fm_pc_quote_n(file + i, 1), fm_pc_shquote(file, 1));
                return 0;
            }
        }
        maxpath = FM_PC_POSIX_PATH;
        maxname = FM_PC_POSIX_NAME;
    } else {
        struct stat st;
        int e;
#ifndef SU_HOST
        if (fm_pc_linux_toolong(file)) e = ENAMETOOLONG;
        else
#endif
        e = lstat(file, &st) == 0 ? 0 : errno;
        if (e == 0) return 1;
        if (e != ENOENT || !len) {
            su_error(0, e, "%s", fm_pc_shquote(file, 0));
            return 0;
        }
        maxpath = FM_PC_PATH_MAX;
        maxname = FM_PC_NAME_MAX;
    }
    if (len > maxpath - 1) {
        su_error(0, 0, "limit %zu exceeded by length %zu of file name %s",
                 maxpath - 1, len, fm_pc_shquote(file, 1));
        return 0;
    }
    for (const char *p = file; *p;) {
        while (*p == '/') p++;
        size_t cl = strcspn(p, "/");
        if (cl > maxname) {
            su_error(0, 0, "limit %zu exceeded by length %zu of file name component %s",
                     maxname, cl, fm_pc_quote_n(p, cl));
            return 0;
        }
        p += cl;
    }
    return 1;
}

static int do_pathchk(int argc, char **argv) {
    enum { FM_PC_PORT = 0x100 };
    static const struct su_option longs[] = {
        { "portability", SU_NO_ARG, 0, FM_PC_PORT },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int basic = 0, extra = 0, c;
    while ((c = su_getopt_long(argc, argv, "+pP", longs, 0)) != -1) {
        switch (c) {
        case 'p': basic = 1; break;
        case 'P': extra = 1; break;
        case FM_PC_PORT: basic = extra = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... NAME...\n"
                   "Report each NAME that is not a valid file name here, or that would not be\n"
                   "portable.\n"
                   "\n"
                   "  -p                  test against the limits of most POSIX systems\n"
                   "                        (14-byte components, 255-byte names, and only\n"
                   "                        letters, digits, '.', '_', '-' and '/') instead\n"
                   "                        of asking the file system\n"
                   "  -P                  also refuse an empty NAME and a component that\n"
                   "                        starts with '-'\n"
                   "      --portability   test against every POSIX system: -p and -P\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "Without -p, a NAME that does not exist passes if it could be created:\n"
                   "every directory on its way searchable and no component too long.\n"
                   "Options come before the first NAME.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind >= argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    int ok = 1;
    for (int i = su_optind; i < argc; i++)
        if (!fm_pc_check(argv[i], basic, extra)) ok = 0;
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return ok ? 0 : 1;
}
