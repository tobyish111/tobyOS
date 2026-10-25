/* su_ls.c -- ls, as GNU coreutils 9.7's (logs/utilgate/ls.sh): finding
 * 149. The old ls (programs/user_ls) took a dozen options and printed its
 * own approximation of the rest.
 *
 * WHAT IS LISTED: each FILE operand that is not a directory (or every one,
 * with -d), then each directory's entries -- with a "NAME:" heading when
 * there are several or with -R. A symbolic link operand to a directory is
 * followed unless -d, -F or -l (GNU's --dereference-command-line-symlink-
 * to-dir default); -H follows any operand, -L every link. Entries start
 * with '.' only with -a (and . and ..) or -A; --hide and -I patterns, and
 * -B's backups (a trailing ~), are left out.
 *
 * ORDER: by name (byte order, this C locale's), or -t time (newest first),
 * -S size (largest first), -X the extension after the last '.', -v
 * version (gnulib's filevercmp: digits as numbers, a file suffix last), -U
 * none, --sort=width; ties by name; -r reverses all of it, and
 * --group-directories-first puts directories (and links to them) first.
 * -c, -u, --time=birth sort by that time too unless -l without -t.
 *
 * FORMAT: -l long, -1 one per line (the default into a pipe), -C columns
 * (the default on a terminal) or -x across, laid out as GNU does -- the
 * most columns whose widths, each its widest name and two blanks, fit in
 * -w (COLUMNS, the terminal, 80), padded with tabs on tab stops (-T, 8);
 * -m commas, wrapped at the width. In a name a control or non-ASCII byte
 * is zero columns wide. --zero ends each with NUL.
 *
 * LONG: [inode] [blocks] mode, links, owner, group, [author], size ("M,
 * m" for a device), time, name, " -> target" for a link -- the numbers
 * right-aligned and the names left-aligned in each listing; -n numbers,
 * -g no owner, -G/-o no group; -h/--si/--block-size sizes rounded up;
 * --time-style full-iso, long-iso, iso, locale or +FORMAT (a second
 * format after a newline for recent files), recent meaning the last six
 * months. "total" counts 1 KiB blocks (-k, --block-size, -h change that).
 *
 * NAMES are quoted by --quoting-style (QUOTING_STYLE): literal (the pipe's
 * default, -N), shell, shell-always, shell-escape (a terminal's default),
 * shell-escape-always, c (-Q), c-maybe, escape (-b), locale, clocale; -q
 * shows a control byte as '?'. In columns and long listings, when a shell
 * style quoted any name, the others get a leading blank to line up.
 * Indicators: -F * / @ | = after the name, -p / alone, --file-type no *.
 * --color: GNU's built-in colours, or LS_COLORS.
 *
 * STATUS: 0; 1 for a problem below the top (a subdirectory -R cannot
 * read); 2 for an operand that cannot be read, or a usage error. */

enum { LF_LONG, LF_ONE, LF_COLS, LF_ACROSS, LF_COMMAS };
enum { LS_NAME, LS_NONE, LS_SIZE, LS_TIME, LS_VERSION, LS_EXT, LS_WIDTH };
enum { LT_M, LT_C, LT_A, LT_B };
enum { LI_NONE, LI_SLASH, LI_FILETYPE, LI_CLASSIFY };
enum { LQ_LITERAL, LQ_SHELL, LQ_SHELL_ALWAYS, LQ_SHELL_ESC, LQ_SHELL_ESC_ALWAYS,
       LQ_C, LQ_C_MAYBE, LQ_ESCAPE, LQ_LOCALE, LQ_CLOCALE };
enum { LD_UNDEF, LD_NEVER, LD_CMDLINE, LD_CMDLINE_DIR, LD_ALWAYS };

struct ls_ent {
    char *name;             /* as listed: the operand, or the entry's name */
    char *path;             /* where to stat it */
    struct stat st;
    int ok;                 /* st is real */
    char *link;             /* a link's target */
    struct stat lst;        /* what the target is, when link_ok */
    int link_ok;
    int is_dir;             /* to be listed as a directory (an operand) */
    int typed;              /* st.st_mode's TYPE is known: from st, or readdir */
    int failed;             /* its stat failed: printed with ?s */
    char *q;                /* the quoted name */
    size_t qw;              /* its width in columns */
    int quoted;             /* it starts with a quote */
};

static struct {
    int fmt, sort, timetype, ind, quoting, qmark, deref;
    int all;                /* 1 -A, 2 -a */
    int ignore_backups, dirs, recursive, reverse, numeric, no_owner, no_group;
    int author, inode, blocks, group_dirs, zero, color, dired, sort_given;
    int si, human, kib;     /* sizes */
    uint64_t block_size;    /* -l sizes, 0 = bytes */
    char block_unit[8];
    uint64_t total_unit;    /* "total" and -s */
    char total_suffix[8];
    int tabsize;
    size_t width;           /* 0 = no limit */
    const char *tfmt_old, *tfmt_recent;
    char **hide, **ignore;
    int nhide, nignore;
    int status;
    int first_out;          /* nothing written yet */
    int print_dirname;
    int align_quotes;
    int context;            /* -Z: a '?' context column (no SELinux here) */
    int tfmt_custom;        /* --time-style=+FORMAT */
} ls;

/* dir and vdir (su_dir.c) quote with -b's escapes by default: a default,
 * not an option, so QUOTING_STYLE still applies -- and is warned of when
 * bad -- unless an option of their caller's chose the quoting (finding 263) */
static int ls_default_escape;

/* ---- names and ids ---------------------------------------------------- */

struct ls_idname { unsigned long id; char name[64]; struct ls_idname *next; };
static struct ls_idname *ls_users, *ls_groups;

static const char *ls_idname(const char *db, unsigned long id, struct ls_idname **cache) {
    for (struct ls_idname *c = *cache; c; c = c->next) if (c->id == id) return c->name;
    struct ls_idname *c = (struct ls_idname *)malloc(sizeof *c);
    if (!c) su_error(2, ENOMEM, "memory exhausted");
    c->id = id;
    snprintf(c->name, sizeof c->name, "%lu", id);
    FILE *f = fopen(db, "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *c1 = strchr(line, ':');
            if (!c1) continue;
            char *c2 = strchr(c1 + 1, ':');
            if (!c2) continue;
            char *e;
            unsigned long v = strtoul(c2 + 1, &e, 10);
            if (e == c2 + 1 || *e != ':' || v != id) continue;
            *c1 = 0;
            snprintf(c->name, sizeof c->name, "%.63s", line);
            break;
        }
        fclose(f);
    }
    c->next = *cache;
    *cache = c;
    return c->name;
}

/* ---- quoting ---------------------------------------------------------- */

struct ls_sb { char *s; size_t n, cap; };
static void ls_put(struct ls_sb *b, const char *t, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 64;
        while (c < b->n + n + 1) c *= 2;
        char *ns = (char *)realloc(b->s, c);
        if (!ns) su_error(2, ENOMEM, "memory exhausted");
        b->s = ns;
        b->cap = c;
    }
    memcpy(b->s + b->n, t, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void ls_putc(struct ls_sb *b, char c) { ls_put(b, &c, 1); }

static int ls_ctl(unsigned char c) { return c < 0x20 || c >= 0x7f; }

/* C's escape for C, or 0 */
static char ls_cesc(unsigned char c) {
    switch (c) {
    case '\a': return 'a'; case '\b': return 'b'; case '\f': return 'f';
    case '\n': return 'n'; case '\r': return 'r'; case '\t': return 't';
    case '\v': return 'v';
    }
    return 0;
}

/* a shell needs NAME quoted (shell and shell-escape styles' rule) */
static int ls_shell_needs(const char *s, int escape_style) {
    size_t n = strlen(s);
    if (!n) return 1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || strchr("%+,-./:@]_", c)) continue;
        if ((c == '#' || c == '~') && i > 0) continue;
        if ((c == '{' || c == '}') && n > 1) continue;
        if (ls_ctl(c)) {
            if (escape_style) return 1;
            if (c == '\t' || c == '\n' || c == '\r') return 1;
            continue;
        }
        return 1;
    }
    return 0;
}

/* the quoted form of NAME into B; its width in columns */
static size_t ls_quote(const char *name, struct ls_sb *b) {
    size_t w = 0, n = strlen(name);
    int st = ls.quoting;
    b->n = 0;
    if (st == LQ_LITERAL) {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];
            if (ls_ctl(c)) { if (ls.qmark) { ls_putc(b, '?'); w++; } else ls_putc(b, (char)c); }
            else { ls_putc(b, (char)c); w++; }
        }
        if (!b->s) ls_put(b, "", 0);
        return w;
    }
    if (st == LQ_SHELL || st == LQ_SHELL_ALWAYS) {
        int need = st == LQ_SHELL_ALWAYS || ls_shell_needs(name, 0);
        int squote = strchr(name, '\'') != 0;
        int dq_ok = 1;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c == '\'') continue;
            if (!(isalnum(c) || c == ' ' || strchr("%+,-./:@]_", c) || ((c == '#' || c == '~') && i == 0)))
                dq_ok = 0;
        }
        if (!need) {
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)name[i];
                if (ls_ctl(c)) { if (ls.qmark) { ls_putc(b, '?'); w++; } else ls_putc(b, (char)c); }
                else { ls_putc(b, (char)c); w++; }
            }
            if (!b->s) ls_put(b, "", 0);
            return w;
        }
        if (squote && dq_ok) {
            ls_putc(b, '"');
            ls_put(b, name, n);
            ls_putc(b, '"');
            return n + 2;
        }
        ls_putc(b, '\''); w++;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];
            if (c == '\'') { ls_put(b, "'\\''", 4); w += 4; }
            else if (ls_ctl(c)) { if (ls.qmark) { ls_putc(b, '?'); w++; } else ls_putc(b, (char)c); }
            else { ls_putc(b, (char)c); w++; }
        }
        ls_putc(b, '\''); w++;
        return w;
    }
    if (st == LQ_SHELL_ESC || st == LQ_SHELL_ESC_ALWAYS) {
        int need = st == LQ_SHELL_ESC_ALWAYS || ls_shell_needs(name, 1);
        if (!need) { ls_put(b, name, n); return n; }
        int squote = strchr(name, '\'') != 0, ctl = 0, dq_ok = 1;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];
            if (ls_ctl(c)) ctl = 1;
            if (c == '\'') continue;
            if (!(isalnum(c) || c == ' ' || strchr("%+,-./:@]_", c) || ((c == '#' || c == '~') && i == 0)))
                dq_ok = 0;
        }
        if (squote && !ctl && dq_ok) {
            ls_putc(b, '"');
            ls_put(b, name, n);
            ls_putc(b, '"');
            return n + 2;
        }
        int in_q = 1;
        ls_putc(b, '\'');
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];
            if (ls_ctl(c)) {
                if (in_q) { ls_putc(b, '\''); in_q = 0; }
                ls_put(b, "$'", 2);
                for (; i < n && ls_ctl((unsigned char)name[i]); i++) {
                    char e = ls_cesc((unsigned char)name[i]);
                    char t[8];
                    if (e) snprintf(t, sizeof t, "\\%c", e);
                    else snprintf(t, sizeof t, "\\%03o", (unsigned char)name[i]);
                    ls_put(b, t, strlen(t));
                }
                i--;
                ls_putc(b, '\'');
            } else if (c == '\'') {
                if (in_q) ls_putc(b, '\'');
                ls_put(b, "\\''", 3);
                in_q = 1;
            } else {
                if (!in_q) { ls_putc(b, '\''); in_q = 1; }
                ls_putc(b, (char)c);
            }
        }
        if (in_q) ls_putc(b, '\'');
        return b->n;
    }
    /* c, c-maybe, escape, locale, clocale: backslash escapes */
    char quote = st == LQ_LOCALE ? '\'' : (st == LQ_ESCAPE ? 0 : '"');
    int wrap = st != LQ_ESCAPE;
    if (st == LQ_C_MAYBE) {
        wrap = 0;
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];
            if (ls_ctl(c) || c == '"' || c == '\\') { wrap = 1; break; }
        }
    }
    if (wrap) ls_putc(b, quote);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        char e = ls_cesc(c);
        char t[8];
        if (c == '\\') ls_put(b, "\\\\", 2);
        else if (wrap && c == (unsigned char)quote) { ls_putc(b, '\\'); ls_putc(b, (char)c); }
        else if (st == LQ_ESCAPE && c == ' ') ls_put(b, "\\ ", 2);
        else if (e) { snprintf(t, sizeof t, "\\%c", e); ls_put(b, t, 2); }
        else if (ls_ctl(c)) { snprintf(t, sizeof t, "\\%03o", c); ls_put(b, t, 4); }
        else ls_putc(b, (char)c);
    }
    if (wrap) ls_putc(b, quote);
    if (!b->s) ls_put(b, "", 0);
    return b->n;
}

/* ---- colours ------------------------------------------------------------ */

struct ls_color { char key[24]; char seq[48]; struct ls_color *next; };
static struct ls_color *ls_colors;
static int ls_color_started;

static const char *ls_color_get(const char *key) {
    for (struct ls_color *c = ls_colors; c; c = c->next) if (!strcmp(c->key, key)) return c->seq;
    return 0;
}

static void ls_color_set(const char *key, const char *seq, size_t sl) {
    struct ls_color *c = (struct ls_color *)calloc(1, sizeof *c);
    if (!c) su_error(2, ENOMEM, "memory exhausted");
    snprintf(c->key, sizeof c->key, "%s", key);
    if (sl >= sizeof c->seq) sl = sizeof c->seq - 1;
    memcpy(c->seq, seq, sl);
    c->seq[sl] = 0;
    c->next = ls_colors;
    ls_colors = c;
}

static void ls_color_init(void) {
    /* GNU's built-in defaults, measured with no LS_COLORS */
    static const char *const def[][2] = {
        { "di", "01;34" }, { "ln", "01;36" }, { "pi", "33" }, { "so", "01;35" },
        { "do", "01;35" }, { "bd", "01;33" }, { "cd", "01;33" }, { "su", "37;41" },
        { "sg", "30;43" }, { "tw", "30;42" }, { "ow", "34;42" }, { "st", "37;44" },
        { "ex", "01;32" },
    };
    const char *env = getenv("LS_COLORS");
    if (env && *env) {
        /* KEY=SEQ:... and *.ext=SEQ; later ones win */
        const char *p = env;
        while (*p) {
            const char *e = strchr(p, ':');
            if (!e) e = p + strlen(p);
            const char *eq = (const char *)memchr(p, '=', (size_t)(e - p));
            if (eq && eq > p) {
                char key[24];
                size_t kl = (size_t)(eq - p);
                if (kl >= sizeof key) kl = sizeof key - 1;
                memcpy(key, p, kl);
                key[kl] = 0;
                ls_color_set(key, eq + 1, (size_t)(e - eq - 1));
            }
            p = *e ? e + 1 : e;
        }
        /* keys LS_COLORS does not set keep the built-in ones */
        for (size_t i = 0; i < sizeof def / sizeof def[0]; i++)
            if (!ls_color_get(def[i][0])) ls_color_set(def[i][0], def[i][1], strlen(def[i][1]));
        return;
    }
    for (size_t i = 0; i < sizeof def / sizeof def[0]; i++)
        ls_color_set(def[i][0], def[i][1], strlen(def[i][1]));
}

static const char *ls_color_of(const struct ls_ent *e) {
    if (!e->ok && !e->typed) return ls_color_get("mi");
    mode_t m = e->st.st_mode;
    if (!e->ok) {
        /* known by readdir's type alone (not statted, or the stat failed):
         * its type's colour, as GNU gives it -- nothing a mode says */
        const char *k = S_ISLNK(m) ? "ln" : S_ISDIR(m) ? "di" : S_ISFIFO(m) ? "pi" :
                        S_ISSOCK(m) ? "so" : S_ISBLK(m) ? "bd" : S_ISCHR(m) ? "cd" : "fi";
        return ls_color_get(k);
    }
    const char *k = 0;
    if (S_ISLNK(m)) k = e->link_ok ? "ln" : (ls_color_get("or") ? "or" : "ln");
    else if (S_ISDIR(m)) {
        if ((m & S_ISVTX) && (m & S_IWOTH)) k = "tw";
        else if (m & S_IWOTH) k = "ow";
        else if (m & S_ISVTX) k = "st";
        else k = "di";
    } else if (S_ISFIFO(m)) k = "pi";
    else if (S_ISSOCK(m)) k = "so";
    else if (S_ISBLK(m)) k = "bd";
    else if (S_ISCHR(m)) k = "cd";
    else if (S_ISREG(m)) {
        if (m & S_ISUID) k = "su";
        else if (m & S_ISGID) k = "sg";
        else if (m & 0111) k = "ex";
        else {
            /* *.ext patterns, matched at the end of the name */
            size_t nl = strlen(e->name);
            for (struct ls_color *c = ls_colors; c; c = c->next) {
                if (c->key[0] != '*') continue;
                size_t sl = strlen(c->key + 1);
                if (sl <= nl && !strcmp(e->name + nl - sl, c->key + 1)) return c->seq;
            }
            k = "fi";
        }
    }
    const char *s = k ? ls_color_get(k) : 0;
    return s && *s && strcmp(s, "0") && strcmp(s, "00") ? s : 0;
}

/* ---- sorting ------------------------------------------------------------ */

static int ls_order(unsigned char c) {
    if (isdigit(c)) return 0;
    if (isalpha(c)) return c;
    if (c == '~') return -1;
    return (int)c + 256;
}

/* Debian's version comparison of A[0..AL) and B[0..BL) */
static int ls_verrevcmp(const char *a, size_t al, const char *b, size_t bl) {
    size_t ai = 0, bi = 0;
    while (ai < al || bi < bl) {
        int first_diff = 0;
        while ((ai < al && !isdigit((unsigned char)a[ai])) || (bi < bl && !isdigit((unsigned char)b[bi]))) {
            int ac = ai < al ? ls_order((unsigned char)a[ai]) : 0;
            int bc = bi < bl ? ls_order((unsigned char)b[bi]) : 0;
            if (ac != bc) return ac - bc;
            ai++;
            bi++;
        }
        while (ai < al && a[ai] == '0') ai++;
        while (bi < bl && b[bi] == '0') bi++;
        while (ai < al && bi < bl && isdigit((unsigned char)a[ai]) && isdigit((unsigned char)b[bi])) {
            if (!first_diff) first_diff = a[ai] - b[bi];
            ai++;
            bi++;
        }
        if (ai < al && isdigit((unsigned char)a[ai])) return 1;
        if (bi < bl && isdigit((unsigned char)b[bi])) return -1;
        if (first_diff) return first_diff;
    }
    return 0;
}

/* where a file suffix -- (\.[A-Za-z~][A-Za-z0-9~]*)* to the end -- starts:
 * the first '.' from which the rest is all such pieces; N if none */
static size_t ls_suffix_at(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '.') continue;
        size_t j = i;
        while (j < n) {
            if (s[j] != '.' || j + 1 >= n || !(isalpha((unsigned char)s[j + 1]) || s[j + 1] == '~')) break;
            j += 2;
            while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '~')) j++;
        }
        if (j == n) return i;
    }
    return n;
}

static int ls_filevercmp(const char *a, const char *b) {
    if (!strcmp(a, b)) return 0;
    if (!*a) return -1;
    if (!*b) return 1;
    if (!strcmp(a, ".")) return -1;
    if (!strcmp(b, ".")) return 1;
    if (!strcmp(a, "..")) return -1;
    if (!strcmp(b, "..")) return 1;
    if (*a == '.' && *b != '.') return -1;
    if (*a != '.' && *b == '.') return 1;
    if (*a == '.' && *b == '.') { a++; b++; }
    size_t al = strlen(a), bl = strlen(b);
    size_t ap = ls_suffix_at(a, al), bp = ls_suffix_at(b, bl);
    int r = ls_verrevcmp(a, ap, b, bp);
    if (!r) r = ls_verrevcmp(a, al, b, bl);
    return r;
}

/* -X's key: from the last '.', a leading one too (".bashrc" is all
 * extension, as GNU has it) */
static const char *ls_ext(const char *name) {
    const char *d = strrchr(name, '.');
    return d ? d : "";
}

static void ls_time_of(const struct stat *st, int64_t *sec, long *nsec) {
    *nsec = 0;
    switch (ls.timetype) {
    case LT_C: *sec = (int64_t)st->st_ctime; break;
    case LT_A: *sec = (int64_t)st->st_atime; break;
    case LT_B: *sec = (int64_t)st->st_ctime; break;   /* no birth time here */
    default: *sec = (int64_t)st->st_mtime;
    }
    switch (ls.timetype) {     /* the nanoseconds: libtoby's too (239) */
    case LT_C: case LT_B: *nsec = st->st_ctim.tv_nsec; break;
    case LT_A: *nsec = st->st_atim.tv_nsec; break;
    default: *nsec = st->st_mtim.tv_nsec;
    }
}

static int ls_isdirish(const struct ls_ent *e) {
    if (!e->ok) return e->typed && S_ISDIR(e->st.st_mode);
    if (S_ISDIR(e->st.st_mode)) return 1;
    return S_ISLNK(e->st.st_mode) && e->link_ok && S_ISDIR(e->lst.st_mode);
}

static int ls_cmp(const void *pa, const void *pb) {
    const struct ls_ent *a = *(const struct ls_ent *const *)pa;
    const struct ls_ent *b = *(const struct ls_ent *const *)pb;
    if (ls.group_dirs) {
        int da = ls_isdirish(a), db = ls_isdirish(b);
        if (da != db) return db - da;
    }
    int r = 0;
    switch (ls.sort) {
    case LS_SIZE:
        if (a->st.st_size != b->st.st_size) r = a->st.st_size > b->st.st_size ? -1 : 1;
        break;
    case LS_TIME: {
        int64_t as, bs;
        long an, bn;
        ls_time_of(&a->st, &as, &an);
        ls_time_of(&b->st, &bs, &bn);
        if (as != bs) r = as > bs ? -1 : 1;
        else if (an != bn) r = an > bn ? -1 : 1;
        break;
    }
    case LS_EXT: r = strcmp(ls_ext(a->name), ls_ext(b->name)); break;
    case LS_VERSION: r = ls_filevercmp(a->name, b->name); if (!r) r = strcmp(a->name, b->name); break;
    case LS_WIDTH:
        if (a->qw != b->qw) r = a->qw < b->qw ? -1 : 1;
        break;
    }
    if (!r) r = strcmp(a->name, b->name);
    return ls.reverse ? -r : r;
}

/* ---- gathering ---------------------------------------------------------- */

static int ls_hidden(const char *name) {
    if (name[0] == '.' && ls.all == 0) return 1;
    if (ls.all == 1 && (!strcmp(name, ".") || !strcmp(name, ".."))) return 1;
    for (int i = 0; i < ls.nignore; i++) if (du_match(ls.ignore[i], name)) return 1;
    if (ls.all == 0)
        for (int i = 0; i < ls.nhide; i++) if (du_match(ls.hide[i], name)) return 1;
    if (ls.ignore_backups) {
        size_t n = strlen(name);
        if (n && name[n - 1] == '~') return 1;
    }
    return 0;
}

/* stat E's path: lstat, or stat where links are followed */
static int ls_stat(struct ls_ent *e, int follow, int cmdline) {
    int r;
    (void)cmdline;
    /* under -L a dangling link or a loop is an entry whose stat failed --
     * GNU lists it with ?s and says why; it is not lstatted instead */
    if (follow) r = stat(e->path, &e->st);
    else        r = lstat(e->path, &e->st);
    if (r != 0) return -1;
    e->ok = 1;
    e->typed = 1;
    if (S_ISLNK(e->st.st_mode)) {
        char buf[4096];
        ssize_t n = readlink(e->path, buf, sizeof buf - 1);
        if (n >= 0) {
            buf[n] = 0;
            e->link = strdup(buf);
        }
        e->link_ok = stat(e->path, &e->lst) == 0;
    }
    return 0;
}

static void ls_free(struct ls_ent **v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(v[i]->name);
        free(v[i]->path);
        free(v[i]->link);
        free(v[i]->q);
        free(v[i]);
    }
    free(v);
}

static void ls_quote_all(struct ls_ent **v, size_t n) {
    struct ls_sb b = { 0, 0, 0 };
    int any = 0;
    for (size_t i = 0; i < n; i++) {
        v[i]->qw = ls_quote(v[i]->name, &b);
        free(v[i]->q);
        v[i]->q = strdup(b.s ? b.s : "");
        v[i]->quoted = v[i]->q[0] == '\'' || v[i]->q[0] == '"';
        if (v[i]->quoted) any = 1;
    }
    free(b.s);
    ls.align_quotes = 0;
    if (any && ls.fmt != LF_COMMAS && ls.fmt != LF_ONE &&
        (ls.quoting == LQ_SHELL || ls.quoting == LQ_SHELL_ESC || ls.quoting == LQ_C_MAYBE))
        ls.align_quotes = 1;
}

/* ---- output -------------------------------------------------------------- */

static char ls_indicator(const struct ls_ent *e) {
    if (ls.ind == LI_NONE || (!e->ok && !e->typed)) return 0;
    mode_t m = e->st.st_mode;
    if (S_ISDIR(m)) return '/';
    if (ls.ind == LI_SLASH) return 0;
    if (S_ISLNK(m)) return ls.fmt == LF_LONG ? 0 : '@';
    if (S_ISFIFO(m)) return '|';
    if (S_ISSOCK(m)) return '=';
    if (ls.ind == LI_CLASSIFY && e->ok && S_ISREG(m) && (m & 0111)) return '*';
    return 0;
}

static char ls_mode_indicator(mode_t m) {
    if (S_ISDIR(m)) return '/';
    if (ls.ind == LI_SLASH) return 0;
    if (S_ISLNK(m)) return '@';
    if (S_ISFIFO(m)) return '|';
    if (S_ISSOCK(m)) return '=';
    if (ls.ind == LI_CLASSIFY && S_ISREG(m) && (m & 0111)) return '*';
    return 0;
}

static size_t ls_out_bytes;     /* for --dired */

static void ls_emit(const char *s, size_t n) {
    fwrite(s, 1, n, stdout);
    ls_out_bytes += n;
}
static void ls_emits(const char *s) { ls_emit(s, strlen(s)); }
static void ls_emitc(char c) { ls_emit(&c, 1); }

static uint64_t *ls_dired_pos;
static size_t ls_dired_n, ls_dired_cap;
static void ls_dired_mark(void) {
    if (ls_dired_n == ls_dired_cap) {
        ls_dired_cap = ls_dired_cap ? ls_dired_cap * 2 : 64;
        ls_dired_pos = (uint64_t *)realloc(ls_dired_pos, ls_dired_cap * sizeof *ls_dired_pos);
        if (!ls_dired_pos) su_error(2, ENOMEM, "memory exhausted");
    }
    ls_dired_pos[ls_dired_n++] = ls_out_bytes;
}

/* the name with its colour, alignment blank and indicator; its width */
static size_t ls_print_name(const struct ls_ent *e, int with_ind) {
    size_t w = 0;
    if (ls.align_quotes && !e->quoted) { ls_emitc(' '); w++; }
    const char *col = ls.color ? ls_color_of(e) : 0;
    if (col) {
        if (!ls_color_started) { ls_emits("\033[0m"); ls_color_started = 1; }
        ls_emits("\033[");
        ls_emits(col);
        ls_emitc('m');
    }
    if (ls.dired) ls_dired_mark();
    ls_emits(e->q);
    if (ls.dired) ls_dired_mark();
    if (col) ls_emits("\033[0m");
    w += e->qw;
    if (with_ind) {
        char c = ls_indicator(e);
        if (c) { ls_emitc(c); w++; }
    }
    return w;
}

static void ls_human(uint64_t n, unsigned base, char *out, size_t cap) {
    static const char units[] = "KMGTPEZYRQ";
    uint64_t b = 1;
    int e = 0;
    while (e < 10 && n / b >= base) { b *= base; e++; }
    if (!e) { snprintf(out, cap, "%" PRIu64, n); return; }
    uint64_t q = n / b, r = n % b;
    char u = base == 1000 && e == 1 ? 'k' : units[e - 1];
    if (q < 10) {
        uint64_t t = q * 10 + (r * 10 + b - 1) / b;
        if (t < 100) { snprintf(out, cap, "%d.%d%c", (int)(t / 10), (int)(t % 10), u); return; }
        q = 10;
    } else if (r) {
        q++;
        if (q >= base && e < 10) {
            e++;
            snprintf(out, cap, "1.0%c", base == 1000 && e == 1 ? 'k' : units[e - 1]);
            return;
        }
    }
    snprintf(out, cap, "%" PRIu64 "%c", q, u);
}

/* a size in the -l column's units */
static void ls_size_str(uint64_t bytes, char *out, size_t cap) {
    if (ls.human) { ls_human(bytes, (unsigned)ls.human, out, cap); return; }
    if (!ls.block_size) { snprintf(out, cap, "%" PRIu64, bytes); return; }
    uint64_t v = bytes / ls.block_size + (bytes % ls.block_size != 0);
    snprintf(out, cap, "%" PRIu64 "%s", v, ls.block_unit);
}

/* 512-byte blocks in "total" / -s units */
static void ls_blocks_str(uint64_t blocks, char *out, size_t cap) {
    uint64_t bytes = blocks * 512;
    if (ls.human) { ls_human(bytes, (unsigned)ls.human, out, cap); return; }
    uint64_t v = bytes / ls.total_unit + (bytes % ls.total_unit != 0);
    snprintf(out, cap, "%" PRIu64 "%s", v, ls.total_suffix);
}

static void ls_mode_str(mode_t m, char *o) {
    o[0] = S_ISDIR(m) ? 'd' : S_ISLNK(m) ? 'l' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' :
           S_ISFIFO(m) ? 'p' : S_ISSOCK(m) ? 's' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) o[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & S_ISUID) o[3] = (m & 0100) ? 's' : 'S';
    if (m & S_ISGID) o[6] = (m & 0010) ? 's' : 'S';
    if (m & S_ISVTX) o[9] = (m & 0001) ? 't' : 'T';
    o[10] = 0;
}

static unsigned long ls_major(dev_t d) { return (unsigned long)(((uint64_t)d >> 8) & 0xfff) | (unsigned long)(((uint64_t)d >> 32) & ~0xfffULL); }
static unsigned long ls_minor(dev_t d) { return (unsigned long)((uint64_t)d & 0xff) | (unsigned long)(((uint64_t)d >> 12) & ~0xffULL); }

static int64_t ls_now_s = -1;

static void ls_time_str(const struct ls_ent *e, char *out, size_t cap) {
    int64_t sec;
    long nsec;
    ls_time_of(&e->st, &sec, &nsec);
    if (ls_now_s < 0) ls_now_s = (int64_t)time(0);
    int64_t six = 31556952 / 2;
    int recent = sec > ls_now_s - six && sec <= ls_now_s;
    if (sec > ls_now_s) { ls_now_s = (int64_t)time(0); recent = sec > ls_now_s - six && sec <= ls_now_s; }
    const char *fmt = recent ? ls.tfmt_recent : ls.tfmt_old;
    char *s = tx_strftime(fmt, sec, nsec, 0);
    snprintf(out, cap, "%s", s ? s : "");
    free(s);
}

/* widths of a long listing's columns */
struct ls_widths { size_t ino, blk, nlink, owner, group, author, size, major, minor; };

static void ls_long_widths(struct ls_ent **v, size_t n, struct ls_widths *w) {
    memset(w, 0, sizeof *w);
    char b[64];
    for (size_t i = 0; i < n; i++) {
        const struct ls_ent *e = v[i];
        if (!e->ok) continue;
        size_t l;
        snprintf(b, sizeof b, "%lu", (unsigned long)e->st.st_ino);
        if ((l = strlen(b)) > w->ino) w->ino = l;
        ls_blocks_str((uint64_t)e->st.st_blocks, b, sizeof b);
        if ((l = strlen(b)) > w->blk) w->blk = l;
        snprintf(b, sizeof b, "%lu", (unsigned long)e->st.st_nlink);
        if ((l = strlen(b)) > w->nlink) w->nlink = l;
        const char *o = ls.numeric ? 0 : ls_idname("/etc/passwd", (unsigned long)e->st.st_uid, &ls_users);
        if (ls.numeric) { snprintf(b, sizeof b, "%lu", (unsigned long)e->st.st_uid); o = b; }
        if ((l = strlen(o)) > w->owner) w->owner = l;
        if (l > w->author) w->author = l;
        const char *g = ls.numeric ? 0 : ls_idname("/etc/group", (unsigned long)e->st.st_gid, &ls_groups);
        char gb[32];
        if (ls.numeric) { snprintf(gb, sizeof gb, "%lu", (unsigned long)e->st.st_gid); g = gb; }
        if ((l = strlen(g)) > w->group) w->group = l;
        if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode)) {
            snprintf(b, sizeof b, "%lu", ls_major(e->st.st_rdev));
            if ((l = strlen(b)) > w->major) w->major = l;
            snprintf(b, sizeof b, "%lu", ls_minor(e->st.st_rdev));
            if ((l = strlen(b)) > w->minor) w->minor = l;
        } else {
            ls_size_str((uint64_t)e->st.st_size, b, sizeof b);
            if ((l = strlen(b)) > w->size) w->size = l;
        }
    }
    if (w->major && w->major + 2 + w->minor > w->size) w->size = w->major + 2 + w->minor;
}

/* the width a time takes in the long format, for a failed stat's '?':
 * what the style gives now -- 12 for the default, 16 long-iso -- and none
 * for a +FORMAT of the caller's, as GNU pads it */
static int ls_time_width(void) {
    if (ls.tfmt_custom) return 0;
    char *t = tx_strftime(ls.tfmt_recent, (int64_t)time(0), 0, 0);
    int n = t ? (int)strlen(t) : 0;
    free(t);
    return n;
}

/* A FAILED STAT IN THE LONG FORMAT: the type readdir gave, '?' for
 * everything else, each at its column's width and side (GNU's) */
static void ls_print_long_failed(const struct ls_ent *e, const struct ls_widths *w) {
    char line[512];
    size_t n = 0;
    if (ls.dired) n += (size_t)snprintf(line + n, sizeof line - n, "  ");
    if (ls.inode) n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", (int)w->ino, "?");
    if (ls.blocks) n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", (int)w->blk, "?");
    char mode[12];
    ls_mode_str(e->st.st_mode, mode);
    n += (size_t)snprintf(line + n, sizeof line - n, "%c????????? %*s ", mode[0], (int)w->nlink, "?");
    if (!ls.no_owner) n += (size_t)snprintf(line + n, sizeof line - n, "%-*s ", (int)w->owner, "?");
    if (!ls.no_group) n += (size_t)snprintf(line + n, sizeof line - n, "%-*s ", (int)w->group, "?");
    if (ls.author) n += (size_t)snprintf(line + n, sizeof line - n, "%-*s ", (int)w->author, "?");
    if (ls.context) n += (size_t)snprintf(line + n, sizeof line - n, "? ");
    n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", (int)w->size, "?");
    n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", ls_time_width(), "?");
    if (n >= sizeof line) n = sizeof line - 1;
    ls_emit(line, n);
    ls_print_name(e, 1);
    ls_emitc(ls.zero ? 0 : '\n');
}

static void ls_print_long(const struct ls_ent *e, const struct ls_widths *w) {
    if (e->failed) { ls_print_long_failed(e, w); return; }
    char line[512], b[64];
    size_t n = 0;
    if (ls.dired) n += (size_t)snprintf(line + n, sizeof line - n, "  ");
    if (ls.inode) n += (size_t)snprintf(line + n, sizeof line - n, "%*lu ", (int)w->ino, (unsigned long)e->st.st_ino);
    if (ls.blocks) { ls_blocks_str((uint64_t)e->st.st_blocks, b, sizeof b); n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", (int)w->blk, b); }
    char mode[12];
    ls_mode_str(e->st.st_mode, mode);
    n += (size_t)snprintf(line + n, sizeof line - n, "%s %*lu ", mode, (int)w->nlink, (unsigned long)e->st.st_nlink);
    if (!ls.no_owner) {
        if (ls.numeric) snprintf(b, sizeof b, "%lu", (unsigned long)e->st.st_uid);
        else snprintf(b, sizeof b, "%s", ls_idname("/etc/passwd", (unsigned long)e->st.st_uid, &ls_users));
        n += (size_t)snprintf(line + n, sizeof line - n, "%-*s ", (int)w->owner, b);
    }
    if (!ls.no_group) {
        if (ls.numeric) snprintf(b, sizeof b, "%lu", (unsigned long)e->st.st_gid);
        else snprintf(b, sizeof b, "%s", ls_idname("/etc/group", (unsigned long)e->st.st_gid, &ls_groups));
        n += (size_t)snprintf(line + n, sizeof line - n, "%-*s ", (int)w->group, b);
    }
    if (ls.author) {
        if (ls.numeric) snprintf(b, sizeof b, "%lu", (unsigned long)e->st.st_uid);
        else snprintf(b, sizeof b, "%s", ls_idname("/etc/passwd", (unsigned long)e->st.st_uid, &ls_users));
        n += (size_t)snprintf(line + n, sizeof line - n, "%-*s ", (int)w->author, b);
    }
    if (ls.context) n += (size_t)snprintf(line + n, sizeof line - n, "? ");
    if (S_ISCHR(e->st.st_mode) || S_ISBLK(e->st.st_mode)) {
        char mm[48];
        snprintf(mm, sizeof mm, "%*lu, %*lu", (int)w->major, ls_major(e->st.st_rdev),
                 (int)w->minor, ls_minor(e->st.st_rdev));
        n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", (int)w->size, mm);
    } else {
        ls_size_str((uint64_t)e->st.st_size, b, sizeof b);
        n += (size_t)snprintf(line + n, sizeof line - n, "%*s ", (int)w->size, b);
    }
    char t[128];
    ls_time_str(e, t, sizeof t);
    n += (size_t)snprintf(line + n, sizeof line - n, "%s ", t);
    if (n >= sizeof line) n = sizeof line - 1;
    ls_emit(line, n);
    ls_print_name(e, 1);
    if (S_ISLNK(e->st.st_mode) && e->link) {
        ls_emits(" -> ");
        struct ls_ent t2;
        memset(&t2, 0, sizeof t2);
        t2.name = e->link;
        struct ls_sb qb = { 0, 0, 0 };
        size_t qw = ls_quote(e->link, &qb);
        (void)qw;
        /* the target is coloured only when the colours ask for what a
         * target is -- ln=target, or a colour for an orphan (or) or a
         * missing file (mi) -- as GNU looks at it only then */
        const char *col = 0;
        const char *lnc = ls.color ? ls_color_get("ln") : 0;
        if (ls.color && ((lnc && !strcmp(lnc, "target")) || ls_color_get("or") || ls_color_get("mi"))) {
            t2.ok = e->link_ok;
            t2.st = e->lst;
            col = e->link_ok ? ls_color_of(&t2)
                             : ls_color_get("mi") ? ls_color_get("mi") : ls_color_get("or");
        }
        if (col) { ls_emits("\033["); ls_emits(col); ls_emitc('m'); }
        ls_emits(qb.s ? qb.s : "");
        if (col) ls_emits("\033[0m");
        free(qb.s);
        if (ls.ind == LI_CLASSIFY || ls.ind == LI_FILETYPE) {
            if (e->link_ok) {
                char c = ls_mode_indicator(e->lst.st_mode);
                if (c) ls_emitc(c);
            }
        }
    }
    ls_emitc(ls.zero ? 0 : '\n');
}

static size_t ls_ent_width(const struct ls_ent *e, const struct ls_widths *w) {
    size_t n = e->qw + (ls.align_quotes && !e->quoted ? 1 : 0);
    if (ls_indicator(e)) n++;
    if (ls.inode) n += w->ino + 1;
    if (ls.blocks) n += w->blk + 1;
    if (ls.context) n += 2;
    return n;
}

/* the name with the -i / -s prefixes, for columns and commas */
static size_t ls_print_short(const struct ls_ent *e, const struct ls_widths *w) {
    size_t n = 0;
    char b[64];
    if (ls.inode) {
        int k = e->failed ? snprintf(b, sizeof b, "%*s ", (int)w->ino, "?")
                          : snprintf(b, sizeof b, "%*lu ", (int)w->ino, (unsigned long)e->st.st_ino);
        ls_emit(b, (size_t)k);
        n += (size_t)k;
    }
    if (ls.blocks) {
        char bl[48];
        if (e->failed) snprintf(bl, sizeof bl, "?");
        else ls_blocks_str((uint64_t)e->st.st_blocks, bl, sizeof bl);
        int k = snprintf(b, sizeof b, "%*s ", (int)w->blk, bl);
        ls_emit(b, (size_t)k);
        n += (size_t)k;
    }
    if (ls.context) { ls_emits("? "); n += 2; }
    return n + ls_print_name(e, 1);
}

static void ls_indent(size_t from, size_t to) {
    while (from < to) {
        if (ls.tabsize && to / (size_t)ls.tabsize > (from + 1) / (size_t)ls.tabsize) {
            ls_emitc('\t');
            from += (size_t)ls.tabsize - from % (size_t)ls.tabsize;
        } else {
            ls_emitc(' ');
            from++;
        }
    }
}

static void ls_print_columns(struct ls_ent **v, size_t n, const struct ls_widths *w, int across) {
    if (!n) return;
    size_t *wid = (size_t *)malloc(n * sizeof *wid);
    if (!wid) su_error(2, ENOMEM, "memory exhausted");
    for (size_t i = 0; i < n; i++) wid[i] = ls_ent_width(v[i], w);
    /* GNU's fitting: for each number of columns, every column at least 3
     * wide and each its widest entry + 2 -- the last possible column + 0,
     * whether or not entries reach it -- and the most columns whose sum is
     * under the width win */
    size_t maxcols = n;
    if (ls.width) {
        size_t lim = (ls.width + 2) / 3;
        if (lim < maxcols) maxcols = lim;
        if (!maxcols) maxcols = 1;
    }
    size_t best = 1;
    size_t *cw = (size_t *)calloc(maxcols + 1, sizeof *cw);
    if (!cw) su_error(2, ENOMEM, "memory exhausted");
    for (size_t cols = maxcols; cols >= 1; cols--) {
        size_t rows = (n + cols - 1) / cols;
        for (size_t c = 0; c < cols; c++) cw[c] = 3;
        for (size_t i = 0; i < n; i++) {
            size_t c = across ? i % cols : i / rows;
            size_t l = wid[i] + (c == cols - 1 ? 0 : 2);
            if (l > cw[c]) cw[c] = l;
        }
        size_t tot = 0;
        for (size_t c = 0; c < cols; c++) tot += cw[c];
        if (!ls.width || tot < ls.width || cols == 1) { best = cols; break; }
    }
    size_t cols = best, rows = (n + cols - 1) / cols;
    for (size_t c = 0; c < cols; c++) cw[c] = 3;
    for (size_t i = 0; i < n; i++) {
        size_t c = across ? i % cols : i / rows;
        size_t l = wid[i] + (c == cols - 1 ? 0 : 2);
        if (l > cw[c]) cw[c] = l;
    }
    for (size_t r = 0; r < rows; r++) {
        size_t pos = 0;
        for (size_t c = 0; c < cols; c++) {
            size_t i = across ? r * cols + c : c * rows + r;
            if (i >= n) break;
            size_t next = across ? i + 1 : i + rows;
            ls_print_short(v[i], w);
            if (c + 1 < cols && next < n) ls_indent(pos + wid[i], pos + cw[c]);
            pos += cw[c];
        }
        ls_emitc(ls.zero ? 0 : '\n');
    }
    free(cw);
    free(wid);
}

/* -m: SEP (',') and a blank between names, a new line where the next would
 * pass the width; with no width (-w 0), -C and -x come here too, with SEP
 * a blank: two blanks between names, on one line, no tab stops (GNU's) */
static void ls_print_sep(struct ls_ent **v, size_t n, const struct ls_widths *w, char sep) {
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        size_t len = ls_ent_width(v[i], w);
        if (i) {
            if (!ls.width || pos + len + 2 < ls.width) { ls_emitc(sep); ls_emitc(' '); pos += 2; }
            else { ls_emitc(sep); ls_emitc(ls.zero ? 0 : '\n'); pos = 0; }
        }
        ls_print_short(v[i], w);
        pos += len;
    }
    if (n) ls_emitc(ls.zero ? 0 : '\n');
}

/* V's N entries, already quoted (ls_quote_all) and sorted. A long
 * listing's columns are as wide as V's -- and, for the operands, as the
 * directory operands listed after them (EXTRA), as GNU's are: it measures
 * every operand before it sets the directories aside. */
static void ls_print(struct ls_ent **v, size_t n, int is_dir_listing,
                     struct ls_ent **extra, size_t nextra) {
    struct ls_widths w;
    if (nextra) {
        struct ls_ent **all = (struct ls_ent **)malloc((n + nextra) * sizeof *all);
        if (!all) su_error(2, ENOMEM, "memory exhausted");
        memcpy(all, v, n * sizeof *all);
        memcpy(all + n, extra, nextra * sizeof *all);
        ls_long_widths(all, n + nextra, &w);
        free(all);
    } else {
        ls_long_widths(v, n, &w);
    }
    if (is_dir_listing && (ls.fmt == LF_LONG || ls.blocks)) {
        uint64_t tot = 0;
        for (size_t i = 0; i < n; i++) if (v[i]->ok) tot += (uint64_t)v[i]->st.st_blocks;
        char b[48];
        ls_blocks_str(tot, b, sizeof b);
        if (ls.dired) ls_emits("  ");
        ls_emits("total ");
        ls_emits(b);
        ls_emitc(ls.zero ? 0 : '\n');
    }
    switch (ls.fmt) {
    case LF_LONG:
        for (size_t i = 0; i < n; i++) ls_print_long(v[i], &w);
        break;
    case LF_ONE:
        for (size_t i = 0; i < n; i++) { ls_print_short(v[i], &w); ls_emitc(ls.zero ? 0 : '\n'); }
        break;
    case LF_COLS:
        if (!ls.width) ls_print_sep(v, n, &w, ' ');
        else ls_print_columns(v, n, &w, 0);
        break;
    case LF_ACROSS:
        if (!ls.width) ls_print_sep(v, n, &w, ' ');
        else ls_print_columns(v, n, &w, 1);
        break;
    case LF_COMMAS: ls_print_sep(v, n, &w, ','); break;
    }
}

/* ---- directories ---------------------------------------------------------- */

static void ls_dir(const char *path, const char *shown, int top);

static void ls_heading(const char *shown) {
    if (!ls.first_out) ls_emitc(ls.zero ? 0 : '\n');
    ls.first_out = 0;
    if (ls.print_dirname) {
        struct ls_sb b = { 0, 0, 0 };
        int qm = ls.qmark;
        ls_quote(shown, &b);
        ls.qmark = qm;
        if (ls.dired) ls_emits("  ");
        ls_emits(b.s ? b.s : "");
        ls_emitc(':');
        ls_emitc(ls.zero ? 0 : '\n');
        free(b.s);
    }
}

/* an entry NAME of directory PATH, stat'ed, onto *V */
/* WHETHER A LISTED ENTRY IS STATTED, as GNU's ls decides it (measured over
 * a directory it may read but not search, where every stat fails and so
 * shows: logs/b31/lsstat-gnu.out). Everything for -l -i -s -Z and the
 * time and size sorts; otherwise only what readdir's type cannot tell --
 * whether a regular file is executable (-F, colours), a directory's
 * sticky and other-writable bits (colours), where a link leads (-L with
 * indicators, colours or -R; --group-directories-first; colours that
 * look at the target). A plain `ls` stats nothing, so it lists a
 * directory it cannot search without an error, as GNU's does. */
static int ls_needs_stat(unsigned char dt) {
    if (ls.fmt == LF_LONG || ls.inode || ls.blocks || ls.context ||
        ls.sort == LS_SIZE || ls.sort == LS_TIME) return 1;
    int wants_type = ls.ind != LI_NONE || ls.color || ls.group_dirs || ls.recursive;
    if (dt == DT_UNKNOWN) return wants_type;
    if (dt == DT_LNK) {
        if (ls.deref == LD_ALWAYS && wants_type) return 1;
        if (ls.group_dirs) return 1;
        if (ls.color) {
            const char *lnc = ls_color_get("ln");
            if ((lnc && !strcmp(lnc, "target")) || ls_color_get("or") || ls_color_get("mi"))
                return 1;
        }
        return 0;
    }
    if (dt == DT_REG) return ls.ind == LI_CLASSIFY || ls.color;
    if (dt == DT_DIR) return ls.color;
    return 0;
}

static void ls_type_from_dt(struct ls_ent *e, unsigned char dt) {
    if (dt == DT_UNKNOWN) return;
    e->st.st_mode = (mode_t)((unsigned)dt << 12);  /* DTTOIF */
    e->typed = 1;
}

static void ls_add(const char *path, const char *name, unsigned char dt,
                   struct ls_ent ***v, size_t *n, size_t *cap) {
    struct ls_ent *e = (struct ls_ent *)calloc(1, sizeof *e);
    if (!e) su_error(2, ENOMEM, "memory exhausted");
    e->name = strdup(name);
    size_t pl = strlen(path), nl = strlen(name);
    e->path = (char *)malloc(pl + nl + 2);
    if (!e->name || !e->path) su_error(2, ENOMEM, "memory exhausted");
    memcpy(e->path, path, pl);
    size_t k = pl;
    if (k && e->path[k - 1] != '/') e->path[k++] = '/';
    memcpy(e->path + k, name, nl + 1);
    if (!ls_needs_stat(dt)) {
        ls_type_from_dt(e, dt);
    } else if (ls_stat(e, ls.deref == LD_ALWAYS, 0) != 0) {
        su_error(0, errno, "cannot access %s", su_quote(e->path));
        ls.status = ls.status > 1 ? ls.status : 1;
        memset(&e->st, 0, sizeof e->st);
        e->failed = 1;
        ls_type_from_dt(e, dt);
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 64;
        *v = (struct ls_ent **)realloc(*v, *cap * sizeof **v);
        if (!*v) su_error(2, ENOMEM, "memory exhausted");
    }
    (*v)[(*n)++] = e;
}

static void ls_dir(const char *path, const char *shown, int top) {
    DIR *d = opendir(path);
    if (!d) {
        int e = errno;
        su_error(0, e, "cannot open directory %s", su_quote(shown));
        ls.status = top ? 2 : (ls.status > 1 ? ls.status : 1);
        return;
    }
    ls_heading(shown);
    size_t n = 0, cap = 0;
    struct ls_ent **v = 0;
    struct dirent *de;
    int dots = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".")) dots |= 1;
        if (!strcmp(de->d_name, "..")) dots |= 2;
        if (ls_hidden(de->d_name)) continue;
        ls_add(path, de->d_name, de->d_type, &v, &n, &cap);
    }
    /* . AND .. ARE ENTRIES, as Linux's readdir always gives them; tobyOS's
     * gives neither (a finding of its own: native tools would recurse into
     * them), so -a adds them, first, as Linux lists them -- unless the
     * directory has been removed (no links), where Linux's readdir gives
     * nothing at all, dots included (finding 200) */
    struct stat dst;
    int gone = stat(path, &dst) == 0 && S_ISDIR(dst.st_mode) && dst.st_nlink == 0;
    if (ls.all == 2 && dots != 3 && !gone) {
        size_t before = n;
        if (!(dots & 1) && !ls_hidden(".")) ls_add(path, ".", DT_DIR, &v, &n, &cap);
        if (!(dots & 2) && !ls_hidden("..")) ls_add(path, "..", DT_DIR, &v, &n, &cap);
        size_t added = n - before;
        struct ls_ent *t[2];
        for (size_t i = 0; i < added; i++) t[i] = v[before + i];
        memmove(v + added, v, before * sizeof *v);
        for (size_t i = 0; i < added; i++) v[i] = t[i];
    }
    closedir(d);
    ls_quote_all(v, n);             /* before the sort: --sort=width needs it */
    if (ls.sort != LS_NONE || ls.group_dirs) qsort(v, n, sizeof *v, ls_cmp);
    ls_print(v, n, 1, 0, 0);
    if (ls.recursive) {
        for (size_t i = 0; i < n; i++) {
            struct ls_ent *e = v[i];
            if (!(e->ok || e->typed) || !S_ISDIR(e->st.st_mode)) continue;
            if (!strcmp(e->name, ".") || !strcmp(e->name, "..")) continue;
            char *sh = (char *)malloc(strlen(shown) + strlen(e->name) + 2);
            if (!sh) su_error(2, ENOMEM, "memory exhausted");
            size_t sl = strlen(shown);
            memcpy(sh, shown, sl);
            if (sl && sh[sl - 1] != '/') sh[sl++] = '/';
            strcpy(sh + sl, e->name);
            ls_dir(e->path, sh, 0);
            free(sh);
        }
    }
    ls_free(v, n);
}

/* ---- options ---------------------------------------------------------------- */

static void ls_argbad(const char *what, const char *arg, const char *const *valid, int status) {
    fflush(stdout);
    fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n",
            su_short, su_quote(arg), su_quote(what));
    for (int i = 0; valid[i]; i++) fprintf(stderr, "  - %s\n", valid[i]);
    fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
    exit(status);
}

static int ls_pick(const char *arg, const char *const *names, const int *vals, int n) {
    size_t al = strlen(arg);
    int hit = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(arg, names[i])) return vals[i];
        if (!strncmp(arg, names[i], al) && al) {
            if (hit >= 0 && vals[hit] != vals[i]) return -2;      /* ambiguous */
            hit = i;
        }
    }
    return hit >= 0 ? vals[hit] : -1;
}

static void ls_set_quoting(const char *arg, int from_env) {
    static const char *const names[] = { "literal", "shell", "shell-always", "shell-escape",
        "shell-escape-always", "c", "c-maybe", "escape", "locale", "clocale" };
    static const int vals[] = { LQ_LITERAL, LQ_SHELL, LQ_SHELL_ALWAYS, LQ_SHELL_ESC,
        LQ_SHELL_ESC_ALWAYS, LQ_C, LQ_C_MAYBE, LQ_ESCAPE, LQ_LOCALE, LQ_CLOCALE };
    int v = -1;
    for (int i = 0; i < 10; i++) if (!strcmp(arg, names[i])) v = vals[i];
    if (v < 0 && !from_env) v = ls_pick(arg, names, vals, 10);
    if (v < 0) {
        if (from_env) {
            su_error(0, 0, "ignoring invalid value of environment variable QUOTING_STYLE: %s", su_quote(arg));
            return;
        }
        static const char *const valid[] = { "'literal'", "'shell'", "'shell-always'", "'shell-escape'",
            "'shell-escape-always'", "'c'", "'c-maybe'", "'escape'", "'locale'", "'clocale'", 0 };
        ls_argbad("--quoting-style", arg, valid, 1);
    }
    ls.quoting = v;
}

static void ls_set_time_style(const char *style) {
    if (!strncmp(style, "posix-", 6)) style += 6;
    if (style[0] == '+') {
        static char buf[512];
        ls.tfmt_custom = 1;
        snprintf(buf, sizeof buf, "%s", style + 1);
        char *nl = strchr(buf, '\n');
        if (nl) { *nl = 0; ls.tfmt_old = buf; ls.tfmt_recent = nl + 1; }
        else ls.tfmt_old = ls.tfmt_recent = buf;
        return;
    }
    if (!strcmp(style, "full-iso")) { ls.tfmt_old = ls.tfmt_recent = "%Y-%m-%d %H:%M:%S.%N %z"; return; }
    if (!strcmp(style, "long-iso")) { ls.tfmt_old = ls.tfmt_recent = "%Y-%m-%d %H:%M"; return; }
    if (!strcmp(style, "iso")) { ls.tfmt_old = "%Y-%m-%d "; ls.tfmt_recent = "%m-%d %H:%M"; return; }
    if (!strcmp(style, "locale")) { ls.tfmt_old = "%b %e  %Y"; ls.tfmt_recent = "%b %e %H:%M"; return; }
    fflush(stdout);
    fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n"
            "  - [posix-]full-iso\n  - [posix-]long-iso\n  - [posix-]iso\n  - [posix-]locale\n"
            "  - +FORMAT, a 'date' format such as +%%H:%%M\n"
            "Try '%s --help' for more information.\n", su_short, su_quote(style), su_quote("time style"), su_name);
    exit(2);
}

static void ls_add_pat(char ***v, int *n, const char *p) {
    *v = (char **)realloc(*v, (size_t)(*n + 1) * sizeof **v);
    if (!*v) su_error(2, ENOMEM, "memory exhausted");
    (*v)[(*n)++] = (char *)p;
}

static void ls_block_spec(const char *spec, int for_total) {
    const char *s = spec;
    if (*s == '\'') s++;
    if (!strcmp(s, "human-readable")) { ls.human = 1024; return; }
    if (!strcmp(s, "si")) { ls.human = 1000; return; }
    uint64_t v;
    char unit[8];
    int e = du_parse(s, &v, "eEgGkKmMpPtTyYzZ0", unit);
    if (e != SU_NUM_OK || v == 0) su_error(2, 0, "invalid --block-size argument %s", su_quote(spec));
    ls.human = 0;
    if (!for_total) { ls.block_size = v; snprintf(ls.block_unit, sizeof ls.block_unit, "%s", unit); }
    ls.total_unit = v;
    snprintf(ls.total_suffix, sizeof ls.total_suffix, "%s", unit);
}

static int do_ls(int argc, char **argv) {
    enum { O_AUTHOR = 0x100, O_BLOCKSIZE, O_COLOR, O_DEREF_DIR, O_FILETYPE, O_FORMAT,
           O_FULLTIME, O_GROUPDIRS, O_HIDE, O_HYPERLINK, O_INDSTYLE, O_QSTYLE, O_SHOWCTL,
           O_SI, O_SORT, O_TIME, O_TIMESTYLE, O_ZERO, O_CLASSIFY };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "almost-all", SU_NO_ARG, 0, 'A' },
        { "author", SU_NO_ARG, 0, O_AUTHOR },
        { "escape", SU_NO_ARG, 0, 'b' },
        { "block-size", SU_REQ_ARG, 0, O_BLOCKSIZE },
        { "ignore-backups", SU_NO_ARG, 0, 'B' },
        { "color", SU_OPT_ARG, 0, O_COLOR },
        { "colour", SU_OPT_ARG, 0, O_COLOR },
        { "directory", SU_NO_ARG, 0, 'd' },
        { "dired", SU_NO_ARG, 0, 'D' },
        { "classify", SU_OPT_ARG, 0, O_CLASSIFY },
        { "file-type", SU_NO_ARG, 0, O_FILETYPE },
        { "format", SU_REQ_ARG, 0, O_FORMAT },
        { "full-time", SU_NO_ARG, 0, O_FULLTIME },
        { "group-directories-first", SU_NO_ARG, 0, O_GROUPDIRS },
        { "no-group", SU_NO_ARG, 0, 'G' },
        { "human-readable", SU_NO_ARG, 0, 'h' },
        { "si", SU_NO_ARG, 0, O_SI },
        { "dereference-command-line", SU_NO_ARG, 0, 'H' },
        { "dereference-command-line-symlink-to-dir", SU_NO_ARG, 0, O_DEREF_DIR },
        { "hide", SU_REQ_ARG, 0, O_HIDE },
        { "hyperlink", SU_OPT_ARG, 0, O_HYPERLINK },
        { "indicator-style", SU_REQ_ARG, 0, O_INDSTYLE },
        { "inode", SU_NO_ARG, 0, 'i' },
        { "ignore", SU_REQ_ARG, 0, 'I' },
        { "kibibytes", SU_NO_ARG, 0, 'k' },
        { "dereference", SU_NO_ARG, 0, 'L' },
        { "literal", SU_NO_ARG, 0, 'N' },
        { "numeric-uid-gid", SU_NO_ARG, 0, 'n' },
        { "hide-control-chars", SU_NO_ARG, 0, 'q' },
        { "show-control-chars", SU_NO_ARG, 0, O_SHOWCTL },
        { "quote-name", SU_NO_ARG, 0, 'Q' },
        { "quoting-style", SU_REQ_ARG, 0, O_QSTYLE },
        { "reverse", SU_NO_ARG, 0, 'r' },
        { "recursive", SU_NO_ARG, 0, 'R' },
        { "size", SU_NO_ARG, 0, 's' },
        { "sort", SU_REQ_ARG, 0, O_SORT },
        { "time", SU_REQ_ARG, 0, O_TIME },
        { "time-style", SU_REQ_ARG, 0, O_TIMESTYLE },
        { "tabsize", SU_REQ_ARG, 0, 'T' },
        { "width", SU_REQ_ARG, 0, 'w' },
        { "context", SU_NO_ARG, 0, 'Z' },
        { "zero", SU_NO_ARG, 0, O_ZERO },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&ls, 0, sizeof ls);
    int tty = isatty(1);
    ls.fmt = tty ? LF_COLS : LF_ONE;
    ls.quoting = ls_default_escape ? LQ_ESCAPE : tty ? LQ_SHELL_ESC : LQ_LITERAL;
    ls.tabsize = 8;
    ls.width = 80;
    ls.total_unit = 1024;
    ls.first_out = 1;
    int quoting_given = 0;      /* a quoting option was given: no QUOTING_STYLE */
    {
        const char *c = getenv("COLUMNS");
        if (c && *c) {
            char *e;
            unsigned long v = strtoul(c, &e, 10);
            if (!*e && v > 0) ls.width = (size_t)v;
            else su_error(0, 0, "ignoring invalid width in environment variable COLUMNS: %s", su_quote(c));
        }
#ifdef SU_HOST
        struct winsize ws;
        if (tty && ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) ls.width = ws.ws_col;
#else
        struct { unsigned short row, col, x, y; } ws;
        if (tty && ioctl(1, 0x5413, &ws) == 0 && ws.col > 0) ls.width = ws.col;
#endif
        const char *bs = getenv("LS_BLOCK_SIZE");
        if (!bs || !*bs) bs = getenv("BLOCK_SIZE");
        if (bs && *bs) ls_block_spec(bs, 0);
        if (getenv("POSIXLY_CORRECT")) ls.total_unit = 512;
    }
    const char *time_style = getenv("TIME_STYLE");
    int c, fulltime = 0, deref = LD_UNDEF, ind_given = 0, color_mode = 0;
    while ((c = su_getopt_long(argc, argv, "abcdfghiklmnopqrstuvw:xABCDFGHI:LNQRST:UXZ1", longs, 0)) != -1) {
        switch (c) {
        case 'a': ls.all = 2; break;
        case 'A': ls.all = 1; break;
        case 'b': ls.quoting = LQ_ESCAPE; quoting_given = 1; break;
        case 'B': ls.ignore_backups = 1; break;
        case 'c': ls.timetype = LT_C; break;
        case 'u': ls.timetype = LT_A; break;
        case 'C': ls.fmt = LF_COLS; break;
        case 'd': ls.dirs = 1; break;
        case 'D': ls.dired = 1; ls.fmt = LF_LONG; break;
        case 'f': ls.all = 2; ls.sort = LS_NONE; ls.sort_given = 1; break;
        case 'F': ls.ind = LI_CLASSIFY; ind_given = 1; break;
        case O_CLASSIFY:
            if (!su_optarg || !strcmp(su_optarg, "always") || !strcmp(su_optarg, "yes") || !strcmp(su_optarg, "force"))
                ls.ind = LI_CLASSIFY;
            else if (!strcmp(su_optarg, "never") || !strcmp(su_optarg, "no") || !strcmp(su_optarg, "none"))
                ls.ind = LI_NONE;
            else if (!strcmp(su_optarg, "auto") || !strcmp(su_optarg, "tty") || !strcmp(su_optarg, "if-tty"))
                ls.ind = tty ? LI_CLASSIFY : LI_NONE;
            else {
                static const char *const v[] = { "'always', 'yes', 'force'", "'never', 'no', 'none'",
                                                 "'auto', 'tty', 'if-tty'", 0 };
                ls_argbad("--classify", su_optarg, v, 1);
            }
            ind_given = 1;
            break;
        case 'g': ls.fmt = LF_LONG; ls.no_owner = 1; break;
        case 'G': ls.no_group = 1; break;
        case 'o': ls.fmt = LF_LONG; ls.no_group = 1; break;
        case 'h': ls.human = 1024; break;
        case O_SI: ls.human = 1000; break;
        case 'H': deref = LD_CMDLINE; break;
        case O_DEREF_DIR: deref = LD_CMDLINE_DIR; break;
        case 'L': deref = LD_ALWAYS; break;
        case 'i': ls.inode = 1; break;
        case 'I': ls_add_pat(&ls.ignore, &ls.nignore, su_optarg); break;
        case O_HIDE: ls_add_pat(&ls.hide, &ls.nhide, su_optarg); break;
        case 'k': ls.kib = 1; ls.total_unit = 1024; ls.total_suffix[0] = 0; break;
        case 'l': ls.fmt = LF_LONG; break;
        case 'm': ls.fmt = LF_COMMAS; break;
        case 'x': ls.fmt = LF_ACROSS; break;
        case '1': if (ls.fmt != LF_LONG) ls.fmt = LF_ONE; break;
        case 'n': ls.numeric = 1; ls.fmt = LF_LONG; break;
        case 'N': ls.quoting = LQ_LITERAL; quoting_given = 1; break;
        case 'p': ls.ind = LI_SLASH; ind_given = 1; break;
        case O_FILETYPE: ls.ind = LI_FILETYPE; ind_given = 1; break;
        case 'q': ls.qmark = 1; break;
        case O_SHOWCTL: ls.qmark = 0; break;
        case 'Q': ls.quoting = LQ_C; quoting_given = 1; break;
        case O_QSTYLE: ls_set_quoting(su_optarg, 0); quoting_given = 1; break;
        case 'r': ls.reverse = 1; break;
        case 'R': ls.recursive = 1; break;
        case 's': ls.blocks = 1; break;
        case 'S': ls.sort = LS_SIZE; ls.sort_given = 1; break;
        case 't': ls.sort = LS_TIME; ls.sort_given = 1; break;
        case 'U': ls.sort = LS_NONE; ls.sort_given = 1; break;
        case 'v': ls.sort = LS_VERSION; ls.sort_given = 1; break;
        case 'X': ls.sort = LS_EXT; ls.sort_given = 1; break;
        case 'Z': ls.context = 1; break;   /* "?": no security contexts here */
        case 'T': {
            char *e;
            unsigned long v = strtoul(su_optarg, &e, 10);
            if (e == su_optarg || *e || v > 1000000) su_error(2, 0, "invalid tab size: %s", su_quote(su_optarg));
            ls.tabsize = (int)v;
            break;
        }
        case 'w': {
            char *e;
            errno = 0;
            unsigned long long v = strtoull(su_optarg, &e, 10);
            if (e == su_optarg || *e || errno) su_error(2, 0, "invalid line width: %s", su_quote(su_optarg));
            ls.width = (size_t)v;
            break;
        }
        case O_AUTHOR: ls.author = 1; break;
        case O_BLOCKSIZE: ls_block_spec(su_optarg, 0); break;
        case O_COLOR:
            if (!su_optarg || !strcmp(su_optarg, "always") || !strcmp(su_optarg, "yes") || !strcmp(su_optarg, "force"))
                color_mode = 1;
            else if (!strcmp(su_optarg, "never") || !strcmp(su_optarg, "no") || !strcmp(su_optarg, "none"))
                color_mode = 0;
            else if (!strcmp(su_optarg, "auto") || !strcmp(su_optarg, "tty") || !strcmp(su_optarg, "if-tty"))
                color_mode = tty;
            else {
                static const char *const v[] = { "'always', 'yes', 'force'", "'never', 'no', 'none'",
                                                 "'auto', 'tty', 'if-tty'", 0 };
                ls_argbad("--color", su_optarg, v, 1);
            }
            break;
        case O_FORMAT: {
            static const char *const n[] = { "verbose", "long", "commas", "horizontal", "across",
                                              "vertical", "single-column" };
            static const int vv[] = { LF_LONG, LF_LONG, LF_COMMAS, LF_ACROSS, LF_ACROSS, LF_COLS, LF_ONE };
            int f = ls_pick(su_optarg, n, vv, 7);
            if (f < 0) {
                static const char *const v[] = { "'verbose', 'long'", "'commas'", "'horizontal', 'across'",
                                                 "'vertical'", "'single-column'", 0 };
                ls_argbad("--format", su_optarg, v, 1);
            }
            ls.fmt = f;
            break;
        }
        case O_FULLTIME: ls.fmt = LF_LONG; fulltime = 1; time_style = "full-iso"; break;
        case O_GROUPDIRS: ls.group_dirs = 1; break;
        case O_HYPERLINK: break;    /* no hyperlinks into a pipe */
        case O_INDSTYLE: {
            static const char *const n[] = { "none", "slash", "file-type", "classify" };
            static const int vv[] = { LI_NONE, LI_SLASH, LI_FILETYPE, LI_CLASSIFY };
            int f = ls_pick(su_optarg, n, vv, 4);
            if (f < 0) {
                static const char *const v[] = { "'none'", "'slash'", "'file-type'", "'classify'", 0 };
                ls_argbad("--indicator-style", su_optarg, v, 1);
            }
            ls.ind = f;
            ind_given = 1;
            break;
        }
        case O_SORT: {
            static const char *const n[] = { "none", "size", "time", "version", "extension", "name", "width" };
            static const int vv[] = { LS_NONE, LS_SIZE, LS_TIME, LS_VERSION, LS_EXT, LS_NAME, LS_WIDTH };
            int f = ls_pick(su_optarg, n, vv, 7);
            if (f < 0) {
                static const char *const v[] = { "'none'", "'size'", "'time'", "'version'", "'extension'",
                                                 "'name'", "'width'", 0 };
                ls_argbad("--sort", su_optarg, v, 1);
            }
            ls.sort = f;
            ls.sort_given = 1;
            break;
        }
        case O_TIME: {
            static const char *const n[] = { "atime", "access", "use", "ctime", "status",
                                              "mtime", "modification", "birth", "creation" };
            static const int vv[] = { LT_A, LT_A, LT_A, LT_C, LT_C, LT_M, LT_M, LT_B, LT_B };
            int f = ls_pick(su_optarg, n, vv, 9);
            if (f < 0) {
                static const char *const v[] = { "'atime', 'access', 'use'", "'ctime', 'status'",
                                                 "'mtime', 'modification'", "'birth', 'creation'", 0 };
                ls_argbad("--time", su_optarg, v, 1);
            }
            ls.timetype = f;
            break;
        }
        case O_TIMESTYLE: time_style = su_optarg; break;
        case O_ZERO:
            /* where it stands, as GNU's: one per line unless long, literal
             * names, control characters shown -- what came before it gives
             * way, what comes after it does not (finding 215: applied once
             * every option was read, so `--zero -Q` lost its -Q) */
            ls.zero = 1;
            if (ls.fmt != LF_LONG) ls.fmt = LF_ONE;
            ls.quoting = LQ_LITERAL;
            ls.qmark = 0;
            quoting_given = 1;
            break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "List the FILEs (the current directory if there are none), each directory's\n"
                   "entries in order of name unless an option says otherwise.\n"
                   "\n"
                   "  -a, --all                  entries starting with . too\n"
                   "  -A, --almost-all           the same, but not . and ..\n"
                   "      --author               with -l, each file's author\n"
                   "  -b, --escape               C-style escapes for unprintable bytes\n"
                   "      --block-size=SIZE      sizes in units of SIZE (K, M, ...; human-readable, si)\n"
                   "  -B, --ignore-backups       leave out entries ending in ~\n"
                   "  -c                         ctime: with -lt sort by it and show it; with -l\n"
                   "                             show it; otherwise sort by it\n"
                   "  -C                         in columns, down the page\n"
                   "      --color[=WHEN]         colour names: always, auto or never\n"
                   "  -d, --directory            a directory itself, not what it holds\n"
                   "  -D, --dired                Emacs dired's output\n"
                   "  -f                         every entry, in the order read (-aU)\n"
                   "  -F, --classify[=WHEN]      a mark after each name: * / @ | =\n"
                   "      --file-type            the same without *\n"
                   "      --format=WORD          across -x, commas -m, horizontal -x, long -l,\n"
                   "                             single-column -1, verbose -l, vertical -C\n"
                   "      --full-time            -l --time-style=full-iso\n"
                   "  -g                         -l without the owner\n"
                   "      --group-directories-first  directories before the rest\n"
                   "  -G, --no-group             no group in a long listing\n"
                   "  -h, --human-readable       sizes like 1K 234M 2G\n"
                   "      --si                   the same in powers of 1000\n"
                   "  -H, --dereference-command-line  follow links named as operands\n"
                   "      --dereference-command-line-symlink-to-dir  follow such a link when it\n"
                   "                             names a directory\n"
                   "      --hide=PATTERN         leave out entries matching PATTERN (not with -a)\n"
                   "      --hyperlink[=WHEN]     hyperlink names (not done here)\n"
                   "      --indicator-style=WORD none, slash (-p), file-type, classify (-F)\n"
                   "  -i, --inode                each file's inode number\n"
                   "  -I, --ignore=PATTERN       leave out entries matching PATTERN\n"
                   "  -k, --kibibytes            1024-byte blocks for -s and total\n"
                   "  -l                         a long listing\n"
                   "  -L, --dereference          a link's target's information\n"
                   "  -m                         across, separated by commas\n"
                   "  -n, --numeric-uid-gid      -l with numbers for owner and group\n"
                   "  -N, --literal              names as they are\n"
                   "  -o                         -l without the group\n"
                   "  -p                         / after a directory's name\n"
                   "  -q, --hide-control-chars   ? for an unprintable byte\n"
                   "      --show-control-chars   unprintable bytes as they are\n"
                   "  -Q, --quote-name           names in double quotes\n"
                   "      --quoting-style=WORD   literal, locale, shell, shell-always,\n"
                   "                             shell-escape, shell-escape-always, c, escape\n"
                   "  -r, --reverse              the order reversed\n"
                   "  -R, --recursive            subdirectories too\n"
                   "  -s, --size                 each file's allocated blocks\n"
                   "  -S                         largest first\n"
                   "      --sort=WORD            none (-U), size (-S), time (-t), version (-v),\n"
                   "                             extension (-X), name, width\n"
                   "      --time=WORD            which time to show and sort by: atime, ctime,\n"
                   "                             mtime, birth\n"
                   "      --time-style=STYLE     full-iso, long-iso, iso, locale, or +FORMAT\n"
                   "  -t                         newest first\n"
                   "  -T, --tabsize=COLS         tab stops every COLS, not 8\n"
                   "  -u                         atime, as -c is ctime\n"
                   "  -U                         in the order read\n"
                   "  -v                         by version number in names\n"
                   "  -w, --width=COLS           the line width (0: no limit)\n"
                   "  -x                         in columns, across the page\n"
                   "  -X                         by extension\n"
                   "  -Z, --context              security contexts (there are none here)\n"
                   "      --zero                 NUL after each entry, not newline\n"
                   "  -1                         one per line\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "Status: 0 if all went well, 1 for a minor problem (a subdirectory that\n"
                   "could not be read), 2 for a serious one (an operand).\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default:
            fflush(stdout);
            fprintf(stderr, "Try '%s --help' for more information.\n", su_name);
            return 2;
        }
    }
    (void)fulltime;
    if (ls.timetype != LT_M && !ls.sort_given && ls.fmt != LF_LONG) ls.sort = LS_TIME;
    if (ls.zero && ls.dired) {
        su_error(0, 0, "--dired and --zero are incompatible");
        return 2;
    }
    /* QUOTING_STYLE only when no option chose a quoting -- an empty one is
     * warned about as any other bad one (finding 215: read before the
     * options, it warned under -Q, and an empty one said nothing) */
    if (!quoting_given) {
        const char *q = getenv("QUOTING_STYLE");
        if (q) ls_set_quoting(q, 1);
    }
    ls.color = color_mode;
    if (ls.color) {
        ls_color_init();
        ls.tabsize = 0;     /* GNU pads coloured columns with blanks alone */
    }
    if (!time_style || !*time_style) time_style = "locale";
    if (ls.fmt == LF_LONG) ls_set_time_style(time_style);
    if (ls.kib && !ls.human) { ls.total_unit = 1024; ls.total_suffix[0] = 0; }
    if (deref == LD_UNDEF)
        deref = (ls.dirs || ls.ind == LI_CLASSIFY || ls.fmt == LF_LONG) ? LD_NEVER : LD_CMDLINE_DIR;
    ls.deref = deref;
    (void)ind_given;
    if (ls.dired && ls.fmt != LF_LONG) ls.dired = 0;

    int nop = argc - su_optind;
    char *dot = (char *)".";
    char **ops = nop ? argv + su_optind : &dot;
    if (!nop) nop = 1;
    struct ls_ent **files = (struct ls_ent **)calloc((size_t)nop, sizeof *files);
    struct ls_ent **dirs = (struct ls_ent **)calloc((size_t)nop, sizeof *dirs);
    if (!files || !dirs) su_error(2, ENOMEM, "memory exhausted");
    size_t nf = 0, nd = 0;
    for (int i = 0; i < nop; i++) {
        struct ls_ent *e = (struct ls_ent *)calloc(1, sizeof *e);
        if (!e) su_error(2, ENOMEM, "memory exhausted");
        e->name = strdup(ops[i]);
        e->path = strdup(ops[i]);
        /* THE "." OF NO OPERANDS IS NOT STATTED: GNU lists it as the
         * directory it is, so in a directory one may not search, `ls`
         * says "cannot open directory '.'" -- where `ls .` and `ls -d`,
         * which stat it, say "cannot access '.'" */
        if (!(argc - su_optind) && !ls.dirs) {
            e->st.st_mode = S_IFDIR;
            e->typed = 1;
            dirs[nd++] = e;
            continue;
        }
        int follow = ls.deref == LD_ALWAYS || ls.deref == LD_CMDLINE;
        int r = ls_stat(e, follow, 1);
        if (r == 0 && ls.deref == LD_CMDLINE_DIR && S_ISLNK(e->st.st_mode) && e->link_ok &&
            S_ISDIR(e->lst.st_mode)) {
            e->st = e->lst;
        }
        if (r != 0) {
            su_error(0, errno, "cannot access %s", su_quote(ops[i]));
            ls.status = 2;
            free(e->name);
            free(e->path);
            free(e->link);
            free(e);
            continue;
        }
        if (!ls.dirs && S_ISDIR(e->st.st_mode)) dirs[nd++] = e;
        else files[nf++] = e;
    }
    ls.print_dirname = ls.recursive || (argc - su_optind) > 1;
    if (nf) {
        ls_quote_all(files, nf);
        if (ls.sort != LS_NONE || ls.group_dirs) qsort(files, nf, sizeof *files, ls_cmp);
        ls_print(files, nf, 0, ls.fmt == LF_LONG ? dirs : 0, ls.fmt == LF_LONG ? nd : 0);
        ls.first_out = 0;
    }
    if (nd) {
        ls_quote_all(dirs, nd);
        if (ls.sort != LS_NONE) qsort(dirs, nd, sizeof *dirs, ls_cmp);
        for (size_t i = 0; i < nd; i++) ls_dir(dirs[i]->path, dirs[i]->name, 1);
    }
    if (ls.dired) {
        fputs("//DIRED//", stdout);
        for (size_t i = 0; i < ls_dired_n; i++) printf(" %" PRIu64, ls_dired_pos[i]);
        putchar('\n');
        printf("//DIRED-OPTIONS// --quoting-style=%s\n",
               (const char *[]){ "literal", "shell", "shell-always", "shell-escape",
                   "shell-escape-always", "c", "c-maybe", "escape", "locale", "clocale" }[ls.quoting]);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(2, errno, "write error");
    ls_free(files, nf);
    ls_free(dirs, nd);
    return ls.status;
}
