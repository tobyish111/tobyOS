/* shell_htab.inc -- two small containers several builtins share: a growable
 * list of strings, and a string-keyed table that iterates in bash's
 * hash-table order (hashlib.c), for the tables whose LISTING order is part of
 * bash's output -- `complete -p`, `hash`. Included by shell.c after
 * sh_bash_hash; everything is static. */

/* strchr and strrchr: the kernel build has neither. */
static const char *comp_strchr(const char *s, char c) {
    for (; *s; s++) if (*s == c) return s;
    return c ? 0 : s;
}

static const char *comp_strrchr(const char *s, char c) {
    const char *r = 0;
    for (; *s; s++) if (*s == c) r = s;
    return c ? r : s;
}

/* ---- a growable list of strings ---------------------------------------- */

struct sh_strv { char **v; int n, cap; };

static bool sv_add_n(struct sh_strv *s, const char *str, size_t len) {
    if (s->n + 1 >= s->cap) {
        int ncap = s->cap ? s->cap * 2 : 16;
        char **nv = (char **)kmalloc((size_t)ncap * sizeof *nv);
        if (!nv) return false;
        for (int i = 0; i < s->n; i++) nv[i] = s->v[i];
        kfree(s->v);
        s->v = nv;
        s->cap = ncap;
    }
    char *c = (char *)kmalloc(len + 1);
    if (!c) return false;
    memcpy(c, str, len);
    c[len] = '\0';
    s->v[s->n++] = c;
    s->v[s->n] = 0;
    return true;
}

static bool sv_add(struct sh_strv *s, const char *str) {
    return sv_add_n(s, str, strlen(str));
}

static void sv_free(struct sh_strv *s) {
    for (int i = 0; i < s->n; i++) kfree(s->v[i]);
    kfree(s->v);
    s->v = 0;
    s->n = s->cap = 0;
}

static bool sv_has(const struct sh_strv *s, const char *str) {
    for (int i = 0; i < s->n; i++) if (strcmp(s->v[i], str) == 0) return true;
    return false;
}

/* Merge sort, by strcmp: bash's qsort with its name comparators. Stable,
 * and O(n log n) -- a completion script can define a thousand functions. */
static void sv_sort(struct sh_strv *s) {
    if (s->n < 2) return;
    char **tmp = (char **)kmalloc((size_t)s->n * sizeof *tmp);
    if (!tmp) return;
    for (int w = 1; w < s->n; w *= 2) {
        for (int lo = 0; lo < s->n; lo += 2 * w) {
            int mid = lo + w < s->n ? lo + w : s->n;
            int hi = lo + 2 * w < s->n ? lo + 2 * w : s->n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = strcmp(s->v[i], s->v[j]) <= 0 ? s->v[i++] : s->v[j++];
            while (i < mid) tmp[k++] = s->v[i++];
            while (j < hi) tmp[k++] = s->v[j++];
        }
        for (int i = 0; i < s->n; i++) s->v[i] = tmp[i];
    }
    kfree(tmp);
}

/* ---- a table walked in bash's hash order (hashlib.c) --------------------
 * FNV-1 over the key, `nb` buckets (a power of two), each bucket newest
 * first; it grows x4 when an insert finds n >= 2 * nb, re-linking the old
 * buckets in order, each head-first. The entries are KEPT in walk order,
 * as an associative array's are (sh_array_set_key). */
struct sh_hent { unsigned hv; char *key; void *data; int hits; int flags; };
struct sh_htab { struct sh_hent *e; int n, cap; unsigned nb, nb0; };

static int sh_htab_find(const struct sh_htab *t, const char *key) {
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->e[i].key, key) == 0) return i;
    return -1;
}

static void sh_htab_rehash(struct sh_htab *t, unsigned nb) {
    struct sh_hent *ne = (struct sh_hent *)kmalloc((size_t)(t->cap > 0 ? t->cap : 1) * sizeof *ne);
    int *cnt = (int *)kmalloc((size_t)nb * sizeof(int));
    if (!ne || !cnt) { kfree(ne); kfree(cnt); return; }
    for (unsigned b = 0; b < nb; b++) cnt[b] = 0;
    for (int i = 0; i < t->n; i++) cnt[t->e[i].hv & (nb - 1)]++;
    int pos = 0;
    for (unsigned b = 0; b < nb; b++) { int c = cnt[b]; cnt[b] = pos; pos += c; }
    for (int i = t->n - 1; i >= 0; i--) ne[cnt[t->e[i].hv & (nb - 1)]++] = t->e[i];
    kfree(t->e);
    kfree(cnt);
    t->e = ne;
    t->nb = nb;
}

/* The entry for `key`, made if it is new (*created says which); -1 out of
 * memory. An index lasts until the next insert or remove. */
static int sh_htab_insert(struct sh_htab *t, const char *key, bool *created) {
    if (created) *created = false;
    int f = sh_htab_find(t, key);
    if (f >= 0) return f;
    if (!t->nb) t->nb = t->nb0;
    if (t->n + 1 > t->cap) {
        int ncap = t->cap ? t->cap * 2 : 16;
        struct sh_hent *ne = (struct sh_hent *)kmalloc((size_t)ncap * sizeof *ne);
        if (!ne) return -1;
        for (int i = 0; i < t->n; i++) ne[i] = t->e[i];
        kfree(t->e);
        t->e = ne;
        t->cap = ncap;
    }
    char *kc = shell_strdup(key);
    if (!kc) return -1;
    if ((unsigned)t->n >= t->nb * 2u) sh_htab_rehash(t, t->nb * 4u);
    unsigned hv = sh_bash_hash(key), b = hv & (t->nb - 1);
    int lo = 0, hi = t->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if ((t->e[mid].hv & (t->nb - 1)) < b) lo = mid + 1;
        else hi = mid;
    }
    for (int j = t->n; j > lo; j--) t->e[j] = t->e[j - 1];
    t->e[lo].hv = hv;
    t->e[lo].key = kc;
    t->e[lo].data = 0;
    t->e[lo].hits = 0;
    t->e[lo].flags = 0;
    t->n++;
    if (created) *created = true;
    return lo;
}

static void sh_htab_remove_at(struct sh_htab *t, int i) {
    kfree(t->e[i].key);
    for (int j = i + 1; j < t->n; j++) t->e[j - 1] = t->e[j];
    t->n--;
}

/* bash's sh_contains_shell_metas: would this word need quoting to be read
 * back as itself? A `~` only where it would expand (first, or after `=` or
 * `:`), a `#` only where it would start a comment. */
static bool sh_contains_shell_metas(const char *s) {
    for (const char *p = s; p && *p; p++) {
        switch (*p) {
        case ' ': case '\t': case '\n': case '\'': case '"': case '\\':
        case '|': case '&': case ';': case '(': case ')': case '<': case '>':
        case '!': case '{': case '}': case '*': case '[': case '?': case ']':
        case '^': case '$': case '`':
            return true;
        case '~':
            if (p == s || p[-1] == '=' || p[-1] == ':') return true;
            break;
        case '#':
            if (p == s) return true;
            break;
        default:
            break;
        }
    }
    return false;
}

/* ---- bash's internal_getopt (builtins/bashgetopt.c) ---------------------
 * The rules every option-taking builtin here shares: clustered letters,
 * an argument attached or in the next word (`;` argument optional, `#`
 * numeric), `--` ends the options, a lone `-` is an operand, `--help` asks
 * for help, a leading `+` in OPTS admits +x forms. Errors are printed as
 * bash prints them, "NAME: -x: invalid option" / "option requires an
 * argument"; the caller adds the usage line. */
struct sh_igo {
    int argc; char **argv;
    int ind;                    /* the word being read */
    int sp;                     /* the letter within it */
    const char *optarg;
    char opttype;               /* '-' or '+' */
    const char *name;           /* the builtin, for messages */
};

#define SH_IGO_HELP (-2)

static void sh_igo_init(struct sh_igo *g, const char *name, int argc, char **argv) {
    g->argc = argc; g->argv = argv; g->ind = 1; g->sp = 1;
    g->optarg = 0; g->opttype = '-'; g->name = name;
}

static int sh_igo_next(struct sh_igo *g, const char *opts) {
    bool plus = *opts == '+';
    if (plus) opts++;
    g->optarg = 0;
    if (g->ind >= g->argc) return -1;
    const char *w = g->argv[g->ind];
    if (g->sp == 1) {
        bool isopt = (w[0] == '-' || (plus && w[0] == '+')) && w[1];
        if (!isopt) return -1;
        if (strcmp(w, "--help") == 0) return SH_IGO_HELP;
        if (strcmp(w, "--") == 0) { g->ind++; return -1; }
        g->opttype = w[0];
    }
    char c = w[g->sp];
    const char *cp = c && c != ':' ? comp_strchr(opts, c) : 0;
    if (!cp) {
        kprintf("%s: %c%c: invalid option\n", g->name, g->opttype, c);
        if (w[++g->sp] == '\0') { g->ind++; g->sp = 1; }
        return '?';
    }
    if (cp[1] == ':' || cp[1] == ';') {
        if (w[g->sp + 1]) {
            g->optarg = w + g->sp + 1;
            g->ind++;
        } else if (g->ind + 1 < g->argc &&
                   (cp[1] == ':' ||
                    !((g->argv[g->ind + 1][0] == '-' ||
                       (plus && g->argv[g->ind + 1][0] == '+')) &&
                      g->argv[g->ind + 1][1]))) {
            g->optarg = g->argv[g->ind + 1];
            g->ind += 2;
        } else if (cp[1] == ';') {
            g->ind++;
        } else {
            kprintf("%s: %c%c: option requires an argument\n", g->name, g->opttype, c);
            g->sp = 1;
            return '?';
        }
        g->sp = 1;
        return c;
    }
    if (w[++g->sp] == '\0') { g->sp = 1; g->ind++; }
    return c;
}

