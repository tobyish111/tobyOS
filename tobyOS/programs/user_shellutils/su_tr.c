/* su_tr.c -- tr, as GNU coreutils 9.7's (logs/utilgate/staging/tr.sh):
 * standard input to standard output, bytes translated, deleted and/or
 * squeezed as STRING1 and STRING2 say.
 *
 * WHAT WAS MEASURED, in the C locale (a character is a byte):
 *   - In a STRING: \\ \a \b \f \n \r \t \v, \NNN (1-3 octal digits; a
 *     3-digit one past \377 is the 2-digit one and a digit, with "warning:
 *     the ambiguous octal escape \400 is being\n\tinterpreted as the
 *     2-byte sequence \040, 0"), \X for any other X is X, and a lone
 *     trailing \ is itself ("warning: an unescaped backslash at end of
 *     string is not portable"). An escaped character is never special.
 *     C1-C2 is a range ("range-endpoints of 'z-a' are in reverse
 *     collating sequence order"; an endpoint that is not printable is
 *     shown \NNN); a '-' first or last is itself. [:CLASS:] (the twelve,
 *     each in byte order: "invalid character class 'foo'", "missing
 *     character class name '[::]'"); [=C=] ("missing equivalence class
 *     character '[==]'", "ab: equivalence class operand must be a single
 *     character"); [C*N] (N octal when it begins with 0, blanks and a '+'
 *     allowed before it: "invalid repeat count '08' in [c*n] construct";
 *     a total too big is "too many characters in set") and [C*] or
 *     [C*0], which in STRING2 fills it to STRING1's length. A '[' that
 *     begins none of them is itself.
 *   - Then, in this order: [C*] in STRING1 ("the [c*] repeat construct
 *     may not appear in string1"; [C*N] may); in STRING2 when not
 *     translating ("the [c*] construct may appear in string2 only when
 *     translating"; two are refused when parsing); when translating,
 *     [=C=] in STRING2, a class there other than upper and lower, an
 *     upper/lower one not at the position of an upper/lower one in
 *     STRING1 ("misaligned", unless -c), an empty STRING2 for a STRING1
 *     that is not ("when not truncating set1, string2 must be
 *     non-empty", unless -t), a shorter STRING2 ending in a class, and
 *     -c of a STRING1 with a class unless STRING2 maps everything to one
 *     character (never with -t).
 *   - ARRAY1 is STRING1's characters in order (with -c every other byte,
 *     ascending; with -t cut to ARRAY2's length); ARRAY2 is padded with
 *     its last character; a character listed twice maps as its last
 *     place says. -s squeezes runs of the characters of the last string
 *     given (all of ARRAY2's, when translating), after translating or
 *     deleting.
 *   - Options stop at the first operand (`tr a -b` maps a to -). A
 *     failed write stops tr ("write error: Broken pipe"); a failed read
 *     is "read error: REASON".
 *
 * The old tr (sbase's) had no [=C=], no [C*N], no -t, no warnings and
 * its own messages. */

#include "su_txio.c"

enum { TXR_CHAR, TXR_RANGE, TXR_CLASS, TXR_EQUIV, TXR_REPEAT };

struct txr_el {
    int type;
    unsigned char c, c2;    /* the character; a range's end */
    int cls;                /* a class's number */
    uintmax_t count;        /* a repeat's count */
    int fill;               /* [C*]: as many as needed */
};

struct txr_spec {
    struct txr_el *e;
    size_t n, cap;
    uintmax_t len;          /* characters, not counting a [C*] */
    int nfill, has_class, has_equiv;
};

static const char *const txr_cls_names[] = {
    "alnum", "alpha", "blank", "cntrl", "digit", "graph",
    "lower", "print", "punct", "space", "upper", "xdigit", 0
};
enum { TXR_LOWER = 6, TXR_UPPER = 10 };

/* membership in the C locale's classes */
static int txr_in_class(int cls, int c) {
    int lower = c >= 'a' && c <= 'z', upper = c >= 'A' && c <= 'Z';
    int digit = c >= '0' && c <= '9', graph = c > 0x20 && c < 0x7f;
    switch (cls) {
    case 0: return lower || upper || digit;
    case 1: return lower || upper;
    case 2: return c == ' ' || c == '\t';
    case 3: return c < 0x20 || c == 0x7f;
    case 4: return digit;
    case 5: return graph;
    case 6: return lower;
    case 7: return graph || c == ' ';
    case 8: return graph && !(lower || upper || digit);
    case 9: return c == ' ' || (c >= '\t' && c <= '\r');
    case 10: return upper;
    case 11: return digit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    return 0;
}

static uintmax_t txr_el_len(const struct txr_el *e) {
    switch (e->type) {
    case TXR_RANGE: return (uintmax_t)(e->c2 - e->c) + 1;
    case TXR_CLASS: {
        uintmax_t k = 0;
        for (int c = 0; c < 256; c++) k += (uintmax_t)txr_in_class(e->cls, c);
        return k;
    }
    case TXR_REPEAT: return e->fill ? 0 : e->count;
    }
    return 1;
}

static void txr_push(struct txr_spec *s, struct txr_el el) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->e = (struct txr_el *)realloc(s->e, s->cap * sizeof *s->e);
        if (!s->e) tx_error(1, ENOMEM, "memory exhausted");
    }
    uintmax_t add = txr_el_len(&el);
    if (add >= UINTMAX_MAX - s->len) tx_error(1, 0, "too many characters in set");
    s->len += add;
    if (el.type == TXR_CLASS) s->has_class = 1;
    if (el.type == TXR_EQUIV) s->has_equiv = 1;
    if (el.type == TXR_REPEAT && el.fill) s->nfill++;
    s->e[s->n++] = el;
}

/* a range's endpoint in a message: itself, or \NNN */
static const char *txr_printable(unsigned char c, char *buf) {
    if (c >= 0x20 && c < 0x7f) { buf[0] = (char)c; buf[1] = 0; }
    else snprintf(buf, 8, "\\%03o", c);
    return buf;
}

/* STRING into elements. ESC marks the characters a backslash made. */
static void txr_parse(const char *str, struct txr_spec *s, int is2) {
    size_t sl = strlen(str);
    unsigned char *ch = (unsigned char *)malloc(sl + 1);
    unsigned char *esc = (unsigned char *)malloc(sl + 1);
    if (!ch || !esc) tx_error(1, ENOMEM, "memory exhausted");
    size_t n = 0;
    for (size_t i = 0; i < sl; i++) {
        unsigned char c = (unsigned char)str[i];
        if (c != '\\') { ch[n] = c; esc[n++] = 0; continue; }
        if (i + 1 == sl) {
            tx_error(0, 0, "warning: an unescaped backslash at end of string is not portable");
            ch[n] = '\\';
            esc[n++] = 1;
            continue;
        }
        c = (unsigned char)str[++i];
        unsigned char v;
        switch (c) {
        case 'a': v = '\a'; break;
        case 'b': v = '\b'; break;
        case 'f': v = '\f'; break;
        case 'n': v = '\n'; break;
        case 'r': v = '\r'; break;
        case 't': v = '\t'; break;
        case 'v': v = '\v'; break;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            unsigned val = c - '0';
            int nd = 1;
            while (nd < 3 && i + 1 < sl && str[i + 1] >= '0' && str[i + 1] <= '7') {
                unsigned nv = val * 8 + (unsigned)(str[i + 1] - '0');
                if (nv > 255) {
                    tx_error(0, 0, "warning: the ambiguous octal escape \\%c%c%c is being\n"
                             "\tinterpreted as the 2-byte sequence \\0%c%c, %c",
                             str[i - 1], str[i], str[i + 1], str[i - 1], str[i], str[i + 1]);
                    break;
                }
                val = nv;
                i++;
                nd++;
            }
            v = (unsigned char)val;
            break;
        }
        default: v = c; break;
        }
        ch[n] = v;
        esc[n++] = 1;
    }
#define TXR_IS(k, x) ((k) < n && !esc[k] && ch[k] == (x))
    for (size_t i = 0; i < n;) {
        struct txr_el el;
        memset(&el, 0, sizeof el);
        if (TXR_IS(i, '[') && i + 1 < n) {
            if (TXR_IS(i + 1, ':') || TXR_IS(i + 1, '=')) {
                unsigned char kind = ch[i + 1];
                size_t j = i + 2;
                while (j + 1 < n && !(TXR_IS(j, kind) && TXR_IS(j + 1, ']'))) j++;
                if (j + 1 < n) {
                    size_t len = j - (i + 2);
                    char *name = (char *)malloc(len + 1);
                    if (!name) tx_error(1, ENOMEM, "memory exhausted");
                    memcpy(name, ch + i + 2, len);
                    name[len] = 0;
                    if (kind == ':') {
                        if (!len) tx_error(1, 0, "missing character class name %s", tx_quote("[::]"));
                        int k = 0;
                        while (txr_cls_names[k] && strcmp(txr_cls_names[k], name)) k++;
                        if (!txr_cls_names[k]) tx_error(1, 0, "invalid character class %s", tx_quote(name));
                        el.type = TXR_CLASS;
                        el.cls = k;
                    } else {
                        if (!len) tx_error(1, 0, "missing equivalence class character %s", tx_quote("[==]"));
                        if (len > 1)
                            tx_error(1, 0, "%s: equivalence class operand must be a single character", name);
                        el.type = TXR_EQUIV;
                        el.c = ch[i + 2];
                    }
                    free(name);
                    txr_push(s, el);
                    i = j + 2;
                    continue;
                }
            }
            if (TXR_IS(i + 2, '*')) {
                size_t j = i + 3;
                while (j < n && !TXR_IS(j, ']')) j++;
                if (j < n) {
                    size_t len = j - (i + 3);
                    char *cnt = (char *)malloc(len + 1);
                    if (!cnt) tx_error(1, ENOMEM, "memory exhausted");
                    memcpy(cnt, ch + i + 3, len);
                    cnt[len] = 0;
                    el.type = TXR_REPEAT;
                    el.c = ch[i + 1];
                    if (!len) {
                        el.fill = 1;
                    } else {
                        /* blanks and a '+' may lead; octal after a leading 0 */
                        const char *p = cnt;
                        while (isspace((unsigned char)*p)) p++;
                        if (*p == '+') p++;
                        int base = cnt[0] == '0' ? 8 : 10, bad = !isdigit((unsigned char)*p);
                        uintmax_t v = 0;
                        for (; *p && !bad; p++) {
                            unsigned d = (unsigned)(*p - '0');
                            if (*p < '0' || *p > '9' || d >= (unsigned)base) { bad = 1; break; }
                            if (v > (UINTMAX_MAX - d) / (unsigned)base) { bad = 1; break; }
                            v = v * (unsigned)base + d;
                        }
                        if (bad || v == UINTMAX_MAX)
                            tx_error(1, 0, "invalid repeat count %s in [c*n] construct", tx_quote(cnt));
                        if (v == 0) el.fill = 1;
                        el.count = v;
                    }
                    free(cnt);
                    if (el.fill && is2 && s->nfill)
                        tx_error(1, 0, "only one [c*] repeat construct may appear in string2");
                    txr_push(s, el);
                    i = j + 1;
                    continue;
                }
            }
        }
        if (i + 2 < n && TXR_IS(i + 1, '-')) {
            if (ch[i] > ch[i + 2]) {
                char a[8], b[8];
                txr_printable(ch[i], a);
                txr_printable(ch[i + 2], b);
                tx_error(1, 0, "range-endpoints of '%s-%s' are in reverse collating sequence order",
                         a, b);
            }
            el.type = TXR_RANGE;
            el.c = ch[i];
            el.c2 = ch[i + 2];
            txr_push(s, el);
            i += 3;
            continue;
        }
        el.type = TXR_CHAR;
        el.c = ch[i];
        txr_push(s, el);
        i++;
    }
#undef TXR_IS
    free(ch);
    free(esc);
}

/* the set a spec names */
static void txr_set(const struct txr_spec *s, unsigned char set[256], uintmax_t fill) {
    memset(set, 0, 256);
    for (size_t k = 0; k < s->n; k++) {
        const struct txr_el *e = &s->e[k];
        switch (e->type) {
        case TXR_RANGE: for (int c = e->c; c <= e->c2; c++) set[c] = 1; break;
        case TXR_CLASS: for (int c = 0; c < 256; c++) if (txr_in_class(e->cls, c)) set[c] = 1; break;
        case TXR_REPEAT: if (e->fill ? fill > 0 : e->count > 0) set[e->c] = 1; break;
        default: set[e->c] = 1; break;
        }
    }
}

/* the character at POS of a spec whose [C*] fills FILL places; past the
 * end, the last one */
static unsigned char txr_at(const struct txr_spec *s, uintmax_t fill, uintmax_t pos) {
    unsigned char last = 0;
    for (size_t k = 0; k < s->n; k++) {
        const struct txr_el *e = &s->e[k];
        uintmax_t len = (e->type == TXR_REPEAT && e->fill) ? fill : txr_el_len(e);
        if (!len) continue;
        if (pos < len) {
            switch (e->type) {
            case TXR_RANGE: return (unsigned char)(e->c + pos);
            case TXR_CLASS:
                for (int c = 0; c < 256; c++)
                    if (txr_in_class(e->cls, c) && pos-- == 0) return (unsigned char)c;
                return 0;
            default: return e->c;
            }
        }
        pos -= len;
        switch (e->type) {
        case TXR_RANGE: last = e->c2; break;
        case TXR_CLASS:
            for (int c = 0; c < 256; c++) if (txr_in_class(e->cls, c)) last = (unsigned char)c;
            break;
        default: last = e->c; break;
        }
    }
    return last;
}

static void txr_help(void) {
    tx_printf("Usage: %s [OPTION]... STRING1 [STRING2]\n"
              "Copy standard input to standard output, translating, squeezing and/or\n"
              "deleting characters. STRING1 and STRING2 name the arrays of characters\n"
              "ARRAY1 and ARRAY2 that say what is done.\n"
              "\n"
              "  -c, -C, --complement    use every character not in ARRAY1 instead\n"
              "  -d, --delete            delete the characters of ARRAY1\n"
              "  -s, --squeeze-repeats   replace each run of one character that is in\n"
              "                            the last ARRAY given with one of it\n"
              "  -t, --truncate-set1     cut ARRAY1 to the length of ARRAY2 first\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "Most characters in a STRING stand for themselves. These do not:\n"
              "\n"
              "  \\NNN            the character with octal value NNN (1 to 3 digits)\n"
              "  \\\\              a backslash\n"
              "  \\a              BEL (audible bell)\n"
              "  \\b              backspace\n"
              "  \\f              form feed\n"
              "  \\n              newline\n"
              "  \\r              carriage return\n"
              "  \\t              horizontal tab\n"
              "  \\v              vertical tab\n"
              "  CHAR1-CHAR2     every character from CHAR1 to CHAR2, in order\n"
              "  [CHAR*]         in ARRAY2, CHAR as many times as ARRAY1 needs\n"
              "  [CHAR*REPEAT]   CHAR REPEAT times; REPEAT is octal if it starts with 0\n"
              "  [:alnum:]       the letters and digits\n"
              "  [:alpha:]       the letters\n"
              "  [:blank:]       the horizontal white space\n"
              "  [:cntrl:]       the control characters\n"
              "  [:digit:]       the digits\n"
              "  [:graph:]       the printable characters but space\n"
              "  [:lower:]       the lower-case letters\n"
              "  [:print:]       the printable characters and space\n"
              "  [:punct:]       the punctuation\n"
              "  [:space:]       the horizontal and vertical white space\n"
              "  [:upper:]       the upper-case letters\n"
              "  [:xdigit:]      the hexadecimal digits\n"
              "  [=CHAR=]        the characters equivalent to CHAR\n"
              "\n"
              "Characters are translated when -d is not given and both STRINGs are.\n"
              "-t matters only when translating. ARRAY2 is made as long as ARRAY1 by\n"
              "repeating its last character; what is left of it over is not used.\n"
              "When translating, [:lower:] and [:upper:] in the same place of the two\n"
              "STRINGs change letter case. Squeezing is done after translating or\n"
              "deleting.\n", su_name);
}

static int do_tr(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "complement", SU_NO_ARG, 0, 'c' },
        { "delete", SU_NO_ARG, 0, 'd' },
        { "squeeze-repeats", SU_NO_ARG, 0, 's' },
        { "truncate-set1", SU_NO_ARG, 0, 't' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    tx_begin(1);
    int comp = 0, del = 0, sq = 0, trunc = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "+cCdst", longs, 0)) != -1) {
        switch (c) {
        case 'c': case 'C': comp = 1; break;
        case 'd': del = 1; break;
        case 's': sq = 1; break;
        case 't': trunc = 1; break;
        case SU_HELP_OPT: txr_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default: tx_try_help(1);
        }
    }
    int nops = argc - su_optind;
    char **ops = argv + su_optind;
    if (nops == 0) {
        tx_error(0, 0, "missing operand");
        tx_try_help(1);
    }
    int need = (del == sq) ? 2 : (del ? 1 : 0);    /* 0: one or two */
    if (need == 2 && nops == 1) {
        tx_error(0, 0, "missing operand after %s", tx_quote(ops[0]));
        fprintf(stderr, "%s\n", del ? "Two strings must be given when both deleting and squeezing repeats."
                                    : "Two strings must be given when translating.");
        tx_try_help(1);
    }
    if (need == 1 && nops > 1) {
        tx_error(0, 0, "extra operand %s", tx_quote(ops[1]));
        fprintf(stderr, "Only one string may be given when deleting without squeezing repeats.\n");
        tx_try_help(1);
    }
    if (nops > 2) {
        tx_error(0, 0, "extra operand %s", tx_quote(ops[2]));
        tx_try_help(1);
    }
    int translating = !del && nops == 2;

    struct txr_spec s1, s2;
    memset(&s1, 0, sizeof s1);
    memset(&s2, 0, sizeof s2);
    txr_parse(ops[0], &s1, 0);
    if (nops == 2) txr_parse(ops[1], &s2, 1);
    if (s1.nfill) tx_error(1, 0, "the [c*] repeat construct may not appear in string1");
    if (nops == 2 && !translating && s2.nfill)
        tx_error(1, 0, "the [c*] construct may appear in string2 only when translating");

    unsigned char set1[256];
    txr_set(&s1, set1, 1);
    /* ARRAY1: its characters and places (a repeat's last place only) */
    unsigned char a1c[512];
    
    size_t na1 = 0;
    uintmax_t len1;
    if (comp) {
        for (int k = 0; k < 256; k++)
            if (!set1[k]) { a1c[na1] = (unsigned char)k; na1++; }
        len1 = na1;
    } else {
        len1 = s1.len;
    }

    unsigned char map[256];
    for (int k = 0; k < 256; k++) map[k] = (unsigned char)k;
    unsigned char sqset[256];
    memset(sqset, 0, sizeof sqset);

    if (translating) {
        for (size_t k = 0; k < s2.n; k++)
            if (s2.e[k].type == TXR_EQUIV)
                tx_error(1, 0, "[=c=] expressions may not appear in string2 when translating");
        for (size_t k = 0; k < s2.n; k++)
            if (s2.e[k].type == TXR_CLASS && s2.e[k].cls != TXR_LOWER && s2.e[k].cls != TXR_UPPER)
                tx_error(1, 0, "when translating, the only character classes that may appear in\n"
                         "string2 are 'upper' and 'lower'");
        uintmax_t fill = (s2.nfill && len1 > s2.len) ? len1 - s2.len : 0;
        uintmax_t len2 = s2.len + fill;
        if (!comp) {
            /* each upper/lower of STRING2 where STRING1 has one */
            uintmax_t p2 = 0;
            for (size_t k = 0; k < s2.n; k++) {
                const struct txr_el *e = &s2.e[k];
                if (e->type == TXR_CLASS) {
                    uintmax_t p1 = 0;
                    int ok = 0;
                    for (size_t m = 0; m < s1.n; m++) {
                        const struct txr_el *f = &s1.e[m];
                        if (p1 == p2 && f->type == TXR_CLASS &&
                            (f->cls == TXR_LOWER || f->cls == TXR_UPPER)) { ok = 1; break; }
                        p1 += txr_el_len(f);
                        if (p1 > p2) break;
                    }
                    if (!ok) tx_error(1, 0, "misaligned [:upper:] and/or [:lower:] construct");
                }
                p2 += (e->type == TXR_REPEAT && e->fill) ? fill : txr_el_len(e);
            }
        }
        if (!trunc && len2 == 0 && len1 > 0)
            tx_error(1, 0, "when not truncating set1, string2 must be non-empty");
        if (!trunc && len1 > len2 && s2.n && s2.e[s2.n - 1].type == TXR_CLASS)
            tx_error(1, 0, "when translating with string1 longer than string2,\n"
                     "the latter string must not end with a character class");
        if (comp && s1.has_class) {
            /* the complement of a class has no order to map by: every
             * character of STRING2 must be the same one */
            int one = !trunc && len2 > 0, have = 0;
            unsigned char x = 0;
            for (size_t k = 0; k < s2.n && one; k++) {
                const struct txr_el *e = &s2.e[k];
                uintmax_t len = (e->type == TXR_REPEAT && e->fill) ? fill : txr_el_len(e);
                if (!len) continue;
                if (e->type == TXR_CLASS || (e->type == TXR_RANGE && e->c != e->c2)) { one = 0; break; }
                if (have && e->c != x) one = 0;
                x = e->c;
                have = 1;
            }
            if (!one)
                tx_error(1, 0, "when translating with complemented character classes,\n"
                         "string2 must map all characters in the domain to one");
        }
        if (trunc && len1 > len2) len1 = len2;
        if (comp) {
            for (size_t k = 0; k < na1 && k < len1; k++) map[a1c[k]] = txr_at(&s2, fill, k);
        } else {
            uintmax_t p = 0;
            for (size_t k = 0; k < s1.n && p < len1; k++) {
                const struct txr_el *e = &s1.e[k];
                uintmax_t len = txr_el_len(e);
                switch (e->type) {
                case TXR_RANGE:
                    for (int ch = e->c; ch <= e->c2 && p < len1; ch++, p++)
                        map[ch] = txr_at(&s2, fill, p);
                    break;
                case TXR_CLASS:
                    for (int ch = 0; ch < 256 && p < len1; ch++)
                        if (txr_in_class(e->cls, ch)) map[ch] = txr_at(&s2, fill, p++);
                    break;
                case TXR_REPEAT:
                    if (len) {
                        uintmax_t lastp = p + len - 1;
                        if (lastp >= len1) lastp = len1 - 1;
                        map[e->c] = txr_at(&s2, fill, lastp);
                        p += len;
                    }
                    break;
                default:
                    map[e->c] = txr_at(&s2, fill, p++);
                    break;
                }
            }
        }
        if (sq) {
            /* every character of ARRAY2 */
            txr_set(&s2, sqset, fill);
        }
    } else if (sq) {
        if (nops == 2) txr_set(&s2, sqset, 1);
        else for (int k = 0; k < 256; k++) sqset[k] = comp ? !set1[k] : set1[k];
    }
    unsigned char delset[256];
    for (int k = 0; k < 256; k++) delset[k] = del ? (comp ? !set1[k] : set1[k]) : 0;

    unsigned char in[8192], out[8192];
    int last = -1;
    for (;;) {
        ssize_t r = tx_read(0, in, sizeof in);
        if (r < 0) tx_error(1, errno, "read error");
        if (r == 0) break;
        size_t o = 0;
        for (ssize_t i = 0; i < r; i++) {
            unsigned char ch = in[i];
            if (delset[ch]) continue;
            ch = map[ch];
            if (sq && sqset[ch] && last == ch) continue;
            last = ch;
            out[o++] = ch;
        }
        if (o && tx_fwrite(out, o) < o) {
            int e = errno;
            tx_oerr = 0;
            tx_error(1, e, "write error");
        }
    }
    return 0;
}
