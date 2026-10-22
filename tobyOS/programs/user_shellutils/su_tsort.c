/* su_tsort.c -- tsort, as GNU coreutils 9.7's (logs/utilgate/tsort.sh):
 * the pairs "A B" of FILE (or standard input, or -) read as "A comes
 * before B", and every token written once, in an order that keeps every
 * pair -- or, where the pairs make a loop, the loop reported and broken.
 *
 * What was measured, and what this file therefore does:
 *
 *   - A token is a run of bytes other than space, TAB and newline (\v \f
 *     \r and NUL belong to tokens); a token is compared as a C string, so
 *     "a\0b" is the token "a". An odd count is refused before anything is
 *     written ("F: input contains an odd number of tokens", status 1).
 *     "A A" names A and orders nothing; a pair given twice counts twice.
 *
 *   - The order is a queue. It starts with every token that nothing must
 *     precede, in strcmp order; writing a token releases the tokens it
 *     precedes, in the REVERSE of the order their pairs were read (each
 *     new pair goes to the front of its token's list), and a token whose
 *     last predecessor was written joins the end of the queue.
 *
 *   - When the queue runs dry with tokens left, there is a loop, found by
 *     passes over the tokens in strcmp order: the first one still waiting
 *     starts a chain; a later waiting token with the chain's head among
 *     the tokens it precedes (the first such pair on its list) becomes
 *     the new head; when a token already in the chain is found to precede
 *     the head, the chain from the head to that token is the loop. It is
 *     reported to stderr ("F: input contains a loop:", then "tsort: NAME"
 *     for each member, as written), that one pair is dropped, and the
 *     queue goes on. The exit status is then 1, though everything is
 *     still written. Every probe of this was a differential run against
 *     gnutsort on random graphs.
 *
 * The old tsort (programs/user_coreutil) used a different order. */

struct fm_ts_link { int to; struct fm_ts_link *next; };

struct fm_ts_item {
    const char *name;
    long count;                 /* predecessors not yet written */
    int done;                   /* written */
    int chain;                  /* the next item in the loop chain, or -1 */
    struct fm_ts_link *succ;    /* newest pair first */
};

static struct fm_ts_item *fm_ts_items;
static size_t fm_ts_n, fm_ts_cap;
static int *fm_ts_hash;         /* open addressing; -1 = empty */
static size_t fm_ts_hcap;

static void fm_ts_oom(void) { su_error(1, 0, "memory exhausted"); }

static size_t fm_ts_hashstr(const char *s) {
    size_t h = 5381;
    for (; *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}

static void fm_ts_rehash(void) {
    size_t nc = fm_ts_hcap ? fm_ts_hcap * 2 : 1024;
    int *nh = (int *)malloc(nc * sizeof *nh);
    if (!nh) fm_ts_oom();
    for (size_t i = 0; i < nc; i++) nh[i] = -1;
    for (size_t i = 0; i < fm_ts_n; i++) {
        size_t h = fm_ts_hashstr(fm_ts_items[i].name) & (nc - 1);
        while (nh[h] >= 0) h = (h + 1) & (nc - 1);
        nh[h] = (int)i;
    }
    free(fm_ts_hash);
    fm_ts_hash = nh;
    fm_ts_hcap = nc;
}

/* the item named S (S is kept, not copied) */
static int fm_ts_get(const char *s) {
    if ((fm_ts_n + 1) * 2 > fm_ts_hcap) fm_ts_rehash();
    size_t h = fm_ts_hashstr(s) & (fm_ts_hcap - 1);
    while (fm_ts_hash[h] >= 0) {
        if (!strcmp(fm_ts_items[fm_ts_hash[h]].name, s)) return fm_ts_hash[h];
        h = (h + 1) & (fm_ts_hcap - 1);
    }
    if (fm_ts_n == fm_ts_cap) {
        fm_ts_cap = fm_ts_cap ? fm_ts_cap * 2 : 256;
        fm_ts_items = (struct fm_ts_item *)realloc(fm_ts_items, fm_ts_cap * sizeof *fm_ts_items);
        if (!fm_ts_items) fm_ts_oom();
    }
    struct fm_ts_item *it = &fm_ts_items[fm_ts_n];
    it->name = s;
    it->count = 0;
    it->done = 0;
    it->chain = -1;
    it->succ = 0;
    fm_ts_hash[h] = (int)fm_ts_n;
    return (int)fm_ts_n++;
}

static int fm_ts_cmp(const void *a, const void *b) {
    return strcmp(fm_ts_items[*(const int *)a].name, fm_ts_items[*(const int *)b].name);
}

/* one pass of the loop search over the items in ORDER; 1 when it found
 * and broke a loop */
static int fm_ts_pass(const int *order, int *head, const char *fname) {
    for (size_t i = 0; i < fm_ts_n; i++) {
        int k = order[i];
        struct fm_ts_item *it = &fm_ts_items[k];
        if (it->done || it->count <= 0) continue;
        if (*head < 0) {
            *head = k;
            continue;
        }
        for (struct fm_ts_link **p = &it->succ; *p; p = &(*p)->next) {
            if ((*p)->to != *head) continue;
            if (it->chain >= 0) {
                fflush(stdout);
                fprintf(stderr, "%s: %s: input contains a loop:\n", su_short, su_quotef(fname));
                int m = *head;
                for (;;) {
                    fprintf(stderr, "%s: %s\n", su_short, fm_ts_items[m].name);
                    if (m == k) break;
                    m = fm_ts_items[m].chain;
                }
                struct fm_ts_link *dead = *p;
                *p = dead->next;
                free(dead);
                fm_ts_items[*head].count--;
                /* the chain is over */
                for (size_t j = 0; j < fm_ts_n; j++) fm_ts_items[j].chain = -1;
                *head = -1;
                return 1;
            }
            it->chain = *head;
            *head = k;
            break;
        }
    }
    return 0;
}

/* --help and --version end as the output does: a write error is one */
static int fm_ts_done(void) {
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}

static int do_tsort(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        switch (c) {
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION] [FILE]\n"
                   "Read FILE (or standard input) as pairs of words, each pair saying that\n"
                   "its first word comes before its second, and write every word once, in an\n"
                   "order that keeps every pair.\n"
                   "\n"
                   "With no FILE, or when FILE is -, read standard input.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name);
            return fm_ts_done();
        case SU_VERSION_OPT: su_version(); return fm_ts_done();
        default: su_try_help(1);
        }
    }
    if (argc - su_optind > 1) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind + 1]));
        su_try_help(1);
    }
    const char *fname = su_optind < argc ? argv[su_optind] : "-";
    int fd = 0;
    if (strcmp(fname, "-")) {
        fd = su_open_read(fname, 0);
        if (fd < 0) su_error(1, errno, "%s", su_quotef(fname));
    }
    /* the whole input: tokens point into it */
    char *buf = 0;
    size_t len = 0, cap = 0;
    for (;;) {
        if (cap - len < 65536) {
            cap = cap ? cap * 2 : 131072;
            buf = (char *)realloc(buf, cap + 1);
            if (!buf) fm_ts_oom();
        }
        ssize_t r = read(fd, buf + len, cap - len);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) su_error(1, errno, "%s: read error", su_quotef(fname));
        if (r == 0) break;
        len += (size_t)r;
    }
    if (fd > 0) close(fd);
    if (!buf) { buf = (char *)malloc(1); if (!buf) fm_ts_oom(); }
    buf[len] = 0;

    int pending = -1;
    size_t ntok = 0;
    for (size_t i = 0; i < len;) {
        while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n')) i++;
        if (i >= len) break;
        size_t s = i;
        while (i < len && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\n') i++;
        buf[i < len ? i : len] = 0;     /* a NUL inside ends the name early */
        if (i < len) i++;
        int k = fm_ts_get(buf + s);
        ntok++;
        if (pending < 0) { pending = k; continue; }
        if (pending != k) {
            struct fm_ts_link *l = (struct fm_ts_link *)malloc(sizeof *l);
            if (!l) fm_ts_oom();
            l->to = k;
            l->next = fm_ts_items[pending].succ;
            fm_ts_items[pending].succ = l;
            fm_ts_items[k].count++;
        }
        pending = -1;
    }
    if (ntok % 2) su_error(1, 0, "%s: input contains an odd number of tokens", su_quotef(fname));

    int *order = (int *)malloc((fm_ts_n + 1) * sizeof *order);
    int *queue = (int *)malloc((fm_ts_n + 1) * sizeof *queue);
    if (!order || !queue) fm_ts_oom();
    for (size_t i = 0; i < fm_ts_n; i++) order[i] = (int)i;
    qsort(order, fm_ts_n, sizeof *order, fm_ts_cmp);

    size_t left = fm_ts_n, qh = 0, qt = 0;
    int rc = 0, head = -1;
    for (size_t i = 0; i < fm_ts_n; i++)
        if (fm_ts_items[order[i]].count == 0) queue[qt++] = order[i];
    for (;;) {
        while (qh < qt) {
            int k = queue[qh++];
            struct fm_ts_item *it = &fm_ts_items[k];
            fputs(it->name, stdout);
            putchar('\n');
            it->done = 1;
            left--;
            for (struct fm_ts_link *l = it->succ; l; l = l->next)
                if (--fm_ts_items[l->to].count == 0) queue[qt++] = l->to;
        }
        if (!left) break;
        rc = 1;
        while (!fm_ts_pass(order, &head, fname)) {}
        qh = qt = 0;
        for (size_t i = 0; i < fm_ts_n; i++) {
            struct fm_ts_item *it = &fm_ts_items[order[i]];
            if (!it->done && it->count == 0) queue[qt++] = order[i];
        }
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}
