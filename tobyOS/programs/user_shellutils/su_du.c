/* su_du.c -- du, as GNU coreutils 9.7's (logs/utilgate/du.sh).
 *
 *   du [OPTION]... [FILE]...
 *   du [OPTION]... --files0-from=F
 *
 *   - Each FILE's usage, its directories' after what they hold: blocks
 *     (st_blocks, 512 bytes each), or --apparent-size (a directory's own
 *     counting nothing, as GNU's), or --inodes; in 1K units, or -B SIZE /
 *     -k / -m / -b / -h (ceiling, "1.1K") / --si, or DU_BLOCK_SIZE,
 *     BLOCK_SIZE, BLOCKSIZE, POSIXLY_CORRECT's 512 -- a SIZE with no digits
 *     shows its unit ("1465K", "1500kB", "2MiB").
 *   - -a every file, -s only the FILEs, -d N to depth N, -S a directory
 *     without its subdirectories, -c a "total" line, -t SIZE above (or,
 *     negative, below) a threshold, -0 NUL-ended lines, --time[=atime|
 *     ctime] with --time-style (full-iso long-iso iso +FORMAT).
 *   - A file is counted once: one with more than one link (but -l), and
 *     with more than one FILE or -L, anything met again -- GNU's rule.
 *   - -L follows every link, -D/-H the operands', -P none (the default);
 *     -x stays on each FILE's file system; --exclude=PATTERN and -X FILE
 *     drop what matches (the whole path or any tail after a '/').
 *   - GNU's messages: "cannot access 'F'", "cannot read directory 'D'",
 *     "invalid maximum depth 'N'", "cannot both summarize and show all
 *     entries", "warning: summarizing conflicts with --max-depth=N",
 *     "invalid -B argument", "invalid suffix in -t argument", "-B argument
 *     'N' too large", "F:N: invalid zero-length file name", "file operands
 *     cannot be combined with --files0-from", the --time argmatch lists.
 *
 * It took -a -s -h -k, counted a file's size not its blocks (and a
 * directory as nothing), printed "%ldM" for -h, and said no errors. */

struct du_size { uint64_t n; time_t t; long tns; };

static int du_all, du_apparent, du_inodes, du_count_links, du_separate, du_total;
static int du_null, du_xdev, du_hash_all, du_time = -1, du_human;   /* human: 1024, 1000 */
static int du_deref;        /* 0 -P, 1 -D/-H, 2 -L */
static long du_max_depth = LONG_MAX;
static int64_t du_threshold;
static uint64_t du_block = 1024;
static char du_unit[8];     /* a -B SIZE's shown unit, "" for none */
static const char *du_time_fmt;
static char **du_excl;
static int du_nexcl, du_excl_cap;
static int du_rc;

/* ---- files met already: (dev, ino) */
static uint64_t *du_set;
static size_t du_set_cap, du_set_n;

static int du_seen(uint64_t dev, uint64_t ino) {
    if (du_set_n * 2 >= du_set_cap) {
        size_t ncap = du_set_cap ? du_set_cap * 2 : 1024;
        uint64_t *n = (uint64_t *)calloc(ncap * 2, sizeof *n);
        if (!n) su_error(1, ENOMEM, "memory exhausted");
        for (size_t i = 0; i < du_set_cap; i++) {
            if (!du_set[2 * i] && !du_set[2 * i + 1]) continue;
            size_t h = (size_t)((du_set[2 * i] * 0x9e3779b97f4a7c15ull) ^ du_set[2 * i + 1]) & (ncap - 1);
            while (n[2 * h] || n[2 * h + 1]) h = (h + 1) & (ncap - 1);
            n[2 * h] = du_set[2 * i];
            n[2 * h + 1] = du_set[2 * i + 1];
        }
        free(du_set);
        du_set = n;
        du_set_cap = ncap;
    }
    uint64_t k0 = dev + 1, k1 = ino;   /* +1: (0, 0) marks an empty slot */
    size_t h = (size_t)((k0 * 0x9e3779b97f4a7c15ull) ^ k1) & (du_set_cap - 1);
    while (du_set[2 * h] || du_set[2 * h + 1]) {
        if (du_set[2 * h] == k0 && du_set[2 * h + 1] == k1) return 1;
        h = (h + 1) & (du_set_cap - 1);
    }
    du_set[2 * h] = k0;
    du_set[2 * h + 1] = k1;
    du_set_n++;
    return 0;
}

/* ---- --exclude: fnmatch() with no flags, on the path or a tail after '/' */
static int du_bracket(const char **pp, unsigned char c) {
    const char *p = *pp;
    int neg = 0, hit = 0;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    int first = 1;
    while (*p && (first || *p != ']')) {
        first = 0;
        unsigned char lo = (unsigned char)*p, hi;
        if (*p == '\\' && p[1]) lo = (unsigned char)*++p;
        p++;
        hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            if (*p == '\\' && p[1]) p++;
            hi = (unsigned char)*p++;
        }
        if (lo <= c && c <= hi) hit = 1;
    }
    if (*p != ']') return -1;           /* no closing bracket: a literal '[' */
    *pp = p + 1;
    return hit != neg;
}

static int du_match(const char *pat, const char *s) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (; *s; s++) if (du_match(pat, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        if (*pat == '?') { pat++; s++; continue; }
        if (*pat == '[') {
            const char *p = pat + 1;
            int r = du_bracket(&p, (unsigned char)*s);
            if (r >= 0) {
                if (!r) return 0;
                pat = p;
                s++;
                continue;
            }
        }
        if (*pat == '\\' && pat[1]) pat++;
        if (*pat != *s) return 0;
        pat++;
        s++;
    }
    return !*s;
}

static int du_excluded(const char *path) {
    for (int i = 0; i < du_nexcl; i++) {
        if (du_match(du_excl[i], path)) return 1;
        for (const char *p = path; *p; p++)
            if (*p == '/' && p[1] && p[1] != '/' && du_match(du_excl[i], p + 1)) return 1;
    }
    return 0;
}

/* ---- numbers: a value in base 0 (strtoumax's), a suffix from VALID */
static int du_parse(const char *s, uint64_t *out, const char *valid, char *unit) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return SU_NUM_INVALID;
    char *end;
    errno = 0;
    uint64_t v = 1;
    int digits = isdigit((unsigned char)*p) != 0;
    if (digits) {
        v = strtoull(p, &end, 0);
        if (errno == ERANGE) return SU_NUM_OVERFLOW;
    } else {
        end = (char *)p;
        if (!*p || !strchr(valid, *p)) return SU_NUM_INVALID;
    }
    if (unit) unit[0] = 0;
    if (!*end) { *out = v; return SU_NUM_OK; }
    if (!strchr(valid, *end)) return SU_NUM_BADSUFFIX;
    int base = 1024, extra = 1, power;
    if (end[1] == 'i' && end[2] == 'B') extra = 3;
    else if (end[1] == 'B' || end[1] == 'D') { base = 1000; extra = 2; }
    switch (*end) {
    case 'k': case 'K': power = 1; break;
    case 'm': case 'M': power = 2; break;
    case 'g': case 'G': power = 3; break;
    case 't': case 'T': power = 4; break;
    case 'P': power = 5; break;           /* no lowercase e p z y: GNU's */
    case 'E': power = 6; break;           /* reader has none (1y is a bad */
    case 'Z': power = 7; break;           /* suffix, not an overflow) */
    case 'Y': power = 8; break;
    case 'R': power = 9; break;
    case 'Q': power = 10; break;
    default: return SU_NUM_BADSUFFIX;
    }
    if (end[extra]) return SU_NUM_BADSUFFIX;
    if (unit && !digits) {
        char L = (char)toupper((unsigned char)*end);
        if (base == 1000) snprintf(unit, 8, "%cB", L == 'K' ? 'k' : L);
        else snprintf(unit, 8, "%c%s", L, extra == 3 ? "iB" : "");
    }
    int rc = SU_NUM_OK;
    for (int i = 0; i < power; i++) {
        if (v > UINT64_MAX / (uint64_t)base) { rc = SU_NUM_OVERFLOW; v = UINT64_MAX; break; }
        v *= (uint64_t)base;
    }
    *out = v;
    return rc;
}

static void du_num_fail(int e, const char *opt, const char *arg) {
    if (e == SU_NUM_OVERFLOW) su_error(1, 0, "%s argument %s too large", opt, su_quote(arg));
    if (e == SU_NUM_BADSUFFIX) su_error(1, 0, "invalid suffix in %s argument %s", opt, su_quote(arg));
    su_error(1, 0, "invalid %s argument %s", opt, su_quote(arg));
}

/* -B SIZE (or an environment's): human-readable, si, or a block size */
static void du_set_block(const char *spec, const char *opt) {
    if (*spec == '\'') spec++;
    if (!strcmp(spec, "human-readable")) { du_human = 1024; return; }
    if (!strcmp(spec, "si")) { du_human = 1000; return; }
    uint64_t v;
    char unit[8];
    int e = du_parse(spec, &v, "eEgGkKmMpPtTyYzZ0", unit);
    if (e == SU_NUM_OK && v == 0) e = SU_NUM_INVALID;
    if (e != SU_NUM_OK) du_num_fail(e, opt, spec);
    du_human = 0;
    du_block = v;
    snprintf(du_unit, sizeof du_unit, "%s", unit);
}

/* N bytes (or inodes) as the options print them */
static void du_fmt(uint64_t n, char *out, size_t cap) {
    if (du_inodes) { snprintf(out, cap, "%" PRIu64, n); return; }
    if (du_human) {
        static const char units[] = "KMGTPE";
        unsigned base = (unsigned)du_human;
        uint64_t b = 1;
        int e = 0;
        while (e < 6 && n / b >= base) { b *= base; e++; }
        if (!e) { snprintf(out, cap, "%" PRIu64, n); return; }
        uint64_t q = n / b, r = n % b;
        if (q < 10) {
            uint64_t t = q * 10 + (r * 10 + b - 1) / b;
            if (t < 100) {
                char u = base == 1000 && e == 1 ? 'k' : units[e - 1];
                snprintf(out, cap, "%d.%d%c", (int)(t / 10), (int)(t % 10), u);
                return;
            }
            q = 10;
        } else if (r) {
            q++;
            if (q >= base && e < 6) {
                e++;
                snprintf(out, cap, "1.0%c", base == 1000 && e == 1 ? 'k' : units[e - 1]);
                return;
            }
        }
        snprintf(out, cap, "%" PRIu64 "%c", q, base == 1000 && e == 1 ? 'k' : units[e - 1]);
        return;
    }
    uint64_t v = n / du_block + (n % du_block != 0);
    snprintf(out, cap, "%" PRIu64 "%s", v, du_unit);
}

static void du_print(const struct du_size *s, const char *path) {
    char num[64];
    du_fmt(s->n, num, sizeof num);
    if (du_time >= 0) {
        char tb[256];
        struct tm tm;
        time_t t = s->t;
        localtime_r(&t, &tm);
        const char *f = du_time_fmt;
        if (!strcmp(f, "+full-iso")) {
            char d[64];
            strftime(d, sizeof d, "%Y-%m-%d %H:%M:%S", &tm);
            snprintf(tb, sizeof tb, "%s.%09ld +0000", d, s->tns);
        } else
            strftime(tb, sizeof tb, f + 1, &tm);
        printf("%s\t%s\t%s%c", num, tb, path, du_null ? 0 : '\n');
    } else
        printf("%s\t%s%c", num, path, du_null ? 0 : '\n');
}

static int du_shown(uint64_t n) {
    if (du_threshold < 0) return n <= (uint64_t)-du_threshold;
    return n >= (uint64_t)du_threshold;
}

/* PATH, at LEVEL under its operand on ROOTDEV: its whole size in TOT;
 * 0, or -1 when it does not count (excluded, met before, unreadable) */
static int du_walk(const char *path, int level, uint64_t rootdev, struct du_size *tot) {
    struct stat st;
    int follow = du_deref == 2 || (du_deref == 1 && level == 0);
    int r = follow ? stat(path, &st) : lstat(path, &st);
    if (r != 0 && follow && errno == ENOENT) r = lstat(path, &st);
    if (r != 0) {
        su_error(0, errno, "cannot access %s", su_quoteaf(path));
        du_rc = 1;
        return -1;
    }
    if (du_nexcl && du_excluded(path)) return -1;
    if (level > 0 && du_xdev && (uint64_t)st.st_dev != rootdev) return -1;
    int isdir = S_ISDIR(st.st_mode);
    if (!du_count_links && (du_hash_all || (!isdir && st.st_nlink > 1)) &&
        du_seen((uint64_t)st.st_dev, (uint64_t)st.st_ino))
        return -1;
    struct du_size me = { 0, 0, 0 };
    if (du_inodes) me.n = 1;
    else if (du_apparent) me.n = isdir ? 0 : (st.st_size > 0 ? (uint64_t)st.st_size : 0);
    else me.n = (uint64_t)st.st_blocks * 512;
    if (du_time >= 0) {
        me.t = du_time == 0 ? st.st_mtime : du_time == 1 ? st.st_atime : st.st_ctime;
        me.tns = 0;
    }
    uint64_t subdirs = 0;
    if (isdir) {
        DIR *d = opendir(path);
        if (!d) {
            su_error(0, errno, "cannot read directory %s", su_quoteaf(path));
            du_rc = 1;
        } else {
            size_t pl = strlen(path);
            while (pl > 1 && path[pl - 1] == '/') pl--;
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                size_t nl = strlen(e->d_name);
                char *sub = (char *)malloc(pl + nl + 2);
                if (!sub) su_error(1, ENOMEM, "memory exhausted");
                memcpy(sub, path, pl);
                size_t k = pl;
                if (!(pl == 1 && path[0] == '/')) sub[k++] = '/';
                memcpy(sub + k, e->d_name, nl + 1);
                struct du_size cs;
                struct stat cst;
                int cdir = lstat(sub, &cst) == 0 && S_ISDIR(cst.st_mode);
                if (!cdir && du_deref == 2 && stat(sub, &cst) == 0) cdir = S_ISDIR(cst.st_mode);
                if (du_walk(sub, level + 1, rootdev, &cs) == 0) {
                    me.n += cs.n;
                    if (cdir) subdirs += cs.n;
                    if (cs.t > me.t || (cs.t == me.t && cs.tns > me.tns)) { me.t = cs.t; me.tns = cs.tns; }
                }
                free(sub);
            }
            closedir(d);
        }
    }
    struct du_size show = me;
    if (du_separate && isdir) show.n -= subdirs;
    if ((level <= du_max_depth && (isdir || du_all)) || level == 0)
        if (du_shown(show.n)) du_print(&show, path);
    *tot = me;
    return 0;
}

static void du_add_excl(const char *p) {
    if (du_nexcl == du_excl_cap) {
        du_excl_cap = du_excl_cap ? du_excl_cap * 2 : 8;
        du_excl = (char **)realloc(du_excl, (size_t)du_excl_cap * sizeof *du_excl);
        if (!du_excl) su_error(1, ENOMEM, "memory exhausted");
    }
    du_excl[du_nexcl++] = strdup(p);
}

static int do_du(int argc, char **argv) {
    enum { APPARENT_OPT = 0x100, EXCLUDE_OPT, FILES0_OPT, INODES_OPT, SI_OPT, TIME_OPT, TSTYLE_OPT };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "apparent-size", SU_NO_ARG, 0, APPARENT_OPT },
        { "block-size", SU_REQ_ARG, 0, 'B' },
        { "bytes", SU_NO_ARG, 0, 'b' },
        { "count-links", SU_NO_ARG, 0, 'l' },
        { "dereference", SU_NO_ARG, 0, 'L' },
        { "dereference-args", SU_NO_ARG, 0, 'D' },
        { "exclude", SU_REQ_ARG, 0, EXCLUDE_OPT },
        { "exclude-from", SU_REQ_ARG, 0, 'X' },
        { "files0-from", SU_REQ_ARG, 0, FILES0_OPT },
        { "human-readable", SU_NO_ARG, 0, 'h' },
        { "inodes", SU_NO_ARG, 0, INODES_OPT },
        { "si", SU_NO_ARG, 0, SI_OPT },
        { "max-depth", SU_REQ_ARG, 0, 'd' },
        { "null", SU_NO_ARG, 0, '0' },
        { "no-dereference", SU_NO_ARG, 0, 'P' },
        { "one-file-system", SU_NO_ARG, 0, 'x' },
        { "separate-dirs", SU_NO_ARG, 0, 'S' },
        { "summarize", SU_NO_ARG, 0, 's' },
        { "total", SU_NO_ARG, 0, 'c' },
        { "threshold", SU_REQ_ARG, 0, 't' },
        { "time", SU_OPT_ARG, 0, TIME_OPT },
        { "time-style", SU_REQ_ARG, 0, TSTYLE_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *env = getenv("DU_BLOCK_SIZE");
    if (!env || !*env) env = getenv("BLOCK_SIZE");
    if (!env || !*env) env = getenv("BLOCKSIZE");
    if (env && *env) {
        /* an environment's bad SIZE is passed over, as GNU passes it */
        uint64_t v;
        const char *e = *env == '\'' ? env + 1 : env;
        if (!strcmp(e, "human-readable")) du_human = 1024;
        else if (!strcmp(e, "si")) du_human = 1000;
        else if (du_parse(e, &v, "eEgGkKmMpPtTyYzZ0", du_unit) == SU_NUM_OK && v) du_block = v;
        else du_unit[0] = 0;
    } else if (getenv("POSIXLY_CORRECT"))
        du_block = 512;
    int summarize = 0, max_depth_set = 0, c, li;
    const char *files0 = 0, *tstyle = 0;
    long max_depth = 0;
    while (li = -1, (c = su_getopt_long(argc, argv, "0abd:chHklmsxB:DLPSt:X:", longs, &li)) != -1) {
        switch (c) {
        case '0': du_null = 1; break;
        case 'a': du_all = 1; break;
        case APPARENT_OPT: du_apparent = 1; break;
        case 'b': du_apparent = 1; du_human = 0; du_block = 1; du_unit[0] = 0; break;
        case 'c': du_total = 1; break;
        case 'd': {
            char *end;
            errno = 0;
            long v = strtol(su_optarg, &end, 10);
            if (end == su_optarg || *end || errno) {
                su_error(0, 0, "invalid maximum depth %s", su_quote(su_optarg));
                su_try_help(1);
            }
            max_depth = v;
            max_depth_set = 1;
            break;
        }
        case 'h': du_human = 1024; break;
        case SI_OPT: du_human = 1000; break;
        case 'k': du_human = 0; du_block = 1024; du_unit[0] = 0; break;
        case 'm': du_human = 0; du_block = 1024 * 1024; du_unit[0] = 0; break;
        case 'B': du_set_block(su_optarg, li >= 0 ? "--block-size" : "-B"); break;
        case INODES_OPT: du_inodes = 1; break;
        case 'l': du_count_links = 1; break;
        case 's': summarize = 1; break;
        case 'x': du_xdev = 1; break;
        case 'D': case 'H': du_deref = 1; break;
        case 'L': du_deref = 2; break;
        case 'P': du_deref = 0; break;
        case 'S': du_separate = 1; break;
        case 't': {
            const char *opt = li >= 0 ? "--threshold" : "-t";
            const char *s = su_optarg;
            int neg = *s == '-';
            uint64_t v;
            int e = du_parse(neg ? s + 1 : s, &v, "kKmMGTPEZYRQ0", 0);
            if (e == SU_NUM_OK && v > (uint64_t)INT64_MAX) e = SU_NUM_OVERFLOW;
            if (e != SU_NUM_OK) du_num_fail(e, opt, s);
            if (neg && v == 0)
                su_error(1, 0, "invalid --threshold argument %s", su_quote(s));
            du_threshold = neg ? -(int64_t)v : (int64_t)v;
            break;
        }
        case EXCLUDE_OPT: du_add_excl(su_optarg); break;
        case 'X': {
            FILE *f = !strcmp(su_optarg, "-") ? stdin : fopen(su_optarg, "r");
            if (!f) su_error(1, errno, "%s", su_quotef(su_optarg));
            char *line = 0;
            size_t cap = 0;
            ssize_t n;
            while ((n = getline(&line, &cap, f)) > 0) {
                if (line[n - 1] == '\n') line[--n] = 0;
                du_add_excl(line);
            }
            free(line);
            if (f != stdin) fclose(f);
            break;
        }
        case FILES0_OPT: files0 = su_optarg; break;
        case TIME_OPT:
            du_time = 0;
            if (su_optarg) {
                static const char *const names[] = { "atime", "access", "use", "ctime", "status", 0 };
                int k = -1;
                for (int i = 0; names[i]; i++) if (!strcmp(names[i], su_optarg)) k = i;
                if (k < 0) {
                    fflush(stdout);
                    fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n"
                            "  - 'atime', 'access', 'use'\n  - 'ctime', 'status'\n",
                            su_short, su_quote(su_optarg), su_quote("--time"));
                    su_try_help(1);
                }
                du_time = k < 3 ? 1 : 2;
            }
            break;
        case TSTYLE_OPT: tstyle = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "  or:  %s [OPTION]... --files0-from=F\n"
                   "Show how much space each FILE takes, a directory's after what\n"
                   "is in it (the current directory with no FILE).\n"
                   "\n"
                   "  -0, --null            end each line with NUL, not newline\n"
                   "  -a, --all             every file, not only directories\n"
                   "      --apparent-size   sizes, not the blocks they occupy\n"
                   "  -B, --block-size=SIZE count in units of SIZE (1M, KiB, kB...)\n"
                   "  -b, --bytes           --apparent-size --block-size=1\n"
                   "  -c, --total           a grand total last\n"
                   "  -D, -H, --dereference-args  follow links named on the line\n"
                   "  -d, --max-depth=N     show N levels below each FILE at most\n"
                   "      --files0-from=F   the FILEs from F, NUL-separated (- stdin)\n"
                   "  -h, --human-readable  1K 23M 2G, in powers of 1024\n"
                   "      --inodes          count inodes, not space\n"
                   "  -k                    --block-size=1K\n"
                   "  -L, --dereference     follow every symbolic link\n"
                   "  -l, --count-links     count a file each time it is linked\n"
                   "  -m                    --block-size=1M\n"
                   "  -P, --no-dereference  follow no links (the default)\n"
                   "  -S, --separate-dirs   a directory without its subdirectories\n"
                   "      --si              like -h, in powers of 1000\n"
                   "  -s, --summarize       only a total for each FILE\n"
                   "  -t, --threshold=SIZE  only what is at least SIZE (at most, if\n"
                   "                        negative)\n"
                   "      --time            the latest modification time too\n"
                   "      --time=WORD       atime, access, use, ctime or status instead\n"
                   "      --time-style=STYLE  full-iso, long-iso, iso or +FORMAT\n"
                   "  -X, --exclude-from=FILE  leave out what FILE's patterns match\n"
                   "      --exclude=PATTERN leave out what PATTERN matches\n"
                   "  -x, --one-file-system stay on each FILE's file system\n"
                   "      --help            show this help and exit\n"
                   "      --version         show the version and exit\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (du_all && summarize) {
        su_error(0, 0, "cannot both summarize and show all entries");
        su_try_help(1);
    }
    if (summarize && max_depth_set && max_depth != 0) {
        su_error(0, 0, "warning: summarizing conflicts with --max-depth=%ld", max_depth);
        su_try_help(1);
    }
    if (summarize && max_depth_set)
        su_error(0, 0, "warning: summarizing is the same as using --max-depth=0");
    if (summarize) du_max_depth = 0;
    else if (max_depth_set) du_max_depth = max_depth;
    if (du_inodes && du_apparent)
        su_error(0, 0, "warning: options --apparent-size and -b are ineffective with --inodes");
    if (du_time >= 0) {
        if (!tstyle) tstyle = getenv("TIME_STYLE");
        if (!tstyle) tstyle = "long-iso";
        if (*tstyle == '+') du_time_fmt = tstyle;
        else if (!strcmp(tstyle, "full-iso")) du_time_fmt = "+full-iso";
        else if (!strcmp(tstyle, "long-iso")) du_time_fmt = "+%Y-%m-%d %H:%M";
        else if (!strcmp(tstyle, "iso")) du_time_fmt = "+%Y-%m-%d";
        else {
            fflush(stdout);
            fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n"
                    "  - 'full-iso'\n  - 'long-iso'\n  - 'iso'\n",
                    su_short, su_quote(tstyle), su_quote("time style"));
            su_try_help(1);
        }
    }

    char **ops = argv + su_optind;
    int nops = argc - su_optind;
    char **names = 0;
    int nnames = 0, cap = 0;
    if (files0) {
        if (nops) {
            su_error(0, 0, "extra operand %s", su_quote(ops[0]));
            fprintf(stderr, "file operands cannot be combined with --files0-from\n");
            su_try_help(1);
        }
        int is_stdin = !strcmp(files0, "-");
        FILE *f = is_stdin ? stdin : fopen(files0, "r");
        if (!f) su_error(1, errno, "cannot open %s for reading", su_quoteaf(files0));
        char *line = 0;
        size_t lcap = 0;
        ssize_t n;
        long idx = 0;
        while ((n = getdelim(&line, &lcap, 0, f)) > 0) {
            idx++;
            if (line[n - 1] == 0) n--;
            line[n] = 0;
            if (!n) {
                su_error(0, 0, "%s:%ld: invalid zero-length file name", su_quotef(files0), idx);
                du_rc = 1;
                continue;
            }
            if (is_stdin && !strcmp(line, "-")) {
                su_error(0, 0, "when reading file names from standard input, no file name of %s allowed",
                         su_quoteaf("-"));
                du_rc = 1;
                continue;
            }
            if (nnames == cap) {
                cap = cap ? cap * 2 : 16;
                names = (char **)realloc(names, (size_t)cap * sizeof *names);
                if (!names) su_error(1, ENOMEM, "memory exhausted");
            }
            names[nnames++] = strdup(line);
        }
        free(line);
        if (!is_stdin) fclose(f);
    } else if (nops) {
        names = ops;
        nnames = nops;
    } else {
        static char dot[] = ".";
        static char *one[] = { dot };
        names = one;
        nnames = 1;
    }
    du_hash_all = nnames > 1 || du_deref == 2;
    struct du_size grand = { 0, 0, 0 };
    for (int i = 0; i < nnames; i++) {
        struct stat st;
        uint64_t rootdev = 0;
        if (stat(names[i], &st) == 0) rootdev = (uint64_t)st.st_dev;
        struct du_size t;
        if (du_walk(names[i], 0, rootdev, &t) == 0) {
            grand.n += t.n;
            if (t.t > grand.t) { grand.t = t.t; grand.tns = t.tns; }
        }
    }
    if (du_total) du_print(&grand, "total");
    return du_rc;
}
