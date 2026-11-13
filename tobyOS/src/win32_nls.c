/* src/win32_nls.c -- W7s: the Win32 layer's national language support.
 *
 * Code pages (one-byte tables and their best fit, UTF-8), Unicode case
 * mapping and character classes, named locales' data, collation, and the
 * number, date and time formats. The tables are what Windows 11 answers
 * (src/win32_nls_data.h, generated on Windows by tools/w32nlsdump.c);
 * the rules were measured there (programs/win-locale50, the probes
 * logs/w32/w7s-probe*.c). Collation is tobyOS's own, three levels and the
 * word sort's ignorables: the base letter (a Latin letter's accents and
 * case off; ß as "ss", æ as "ae"; Swedish's å ä ö after z), then the
 * accents, then the case (small first), then a hyphen's or apostrophe's
 * place -- the order Windows gives the cases measured, not its full tables.
 */
#include <tobyos/types.h>
#include <tobyos/klibc.h>
#include <tobyos/w32nls.h>
#include "win32_nls_data.h"

/* ---- code pages ---------------------------------------------------------- */
struct w32nls_cpt {
    uint32_t cp;
    const uint8_t (*rev)[3];
    int nrev;
    const uint8_t (*exact)[2];
    int nexact;
};
#define W32NLS_CPT(c) { c, w32nls_rev_##c, (int)(sizeof w32nls_rev_##c / 3) - 1, \
                        w32nls_exact_##c, (int)(sizeof w32nls_exact_##c / 2) - 1 }
static const struct w32nls_cpt g_cpt[] = {
    W32NLS_CPT(1252), W32NLS_CPT(1250), W32NLS_CPT(1251), W32NLS_CPT(1254), W32NLS_CPT(437),
    W32NLS_CPT(850), W32NLS_CPT(852), W32NLS_CPT(857), W32NLS_CPT(866), W32NLS_CPT(28591), W32NLS_CPT(20127),
};
static int cp_ix(uint32_t cp) {
    for (int i = 0; i < W32NLS_NCP; i++)
        if (w32nls_cp_id[i] == cp) return i;
    return -1;
}
static const struct w32nls_cpt *cp_t(uint32_t cp) {
    for (unsigned i = 0; i < sizeof g_cpt / sizeof g_cpt[0]; i++)
        if (g_cpt[i].cp == cp) return &g_cpt[i];
    return 0;
}
uint32_t w32nls_cp(uint32_t cp) {
    if (cp == 0 || cp == 3) return W32NLS_ACP;          /* CP_ACP, CP_THREAD_ACP */
    if (cp == 1) return W32NLS_OEMCP;                   /* CP_OEMCP */
    if (cp == W32NLS_UTF8) return cp;
    return cp_ix(cp) >= 0 ? cp : 0;
}
uint16_t w32nls_b2u(uint32_t cp, uint8_t b, bool strict) {
    if (b < 0x80) return b;
    int i = cp_ix(cp);
    if (i < 0) return 0xFFFF;
    return strict ? w32nls_cp_hi[i][b - 0x80] : w32nls_cp_plain[i][b - 0x80];
}
static int find3(const uint8_t (*t)[3], int n, uint16_t u) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        uint16_t k = (uint16_t)(t[m][0] | (t[m][1] << 8));
        if (k == u) return m;
        if (k < u) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
static int find2(const uint8_t (*t)[2], int n, uint16_t u) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        uint16_t k = (uint16_t)(t[m][0] | (t[m][1] << 8));
        if (k == u) return m;
        if (k < u) lo = m + 1; else hi = m - 1;
    }
    return -1;
}
int w32nls_u2b(uint32_t cp, uint16_t u, bool nobf) {
    if (u < 0x80) return u;
    const struct w32nls_cpt *t = cp_t(cp);
    if (!t) return -1;
    int k = find3(t->rev, t->nrev, u);
    if (k < 0) return -1;
    if (nobf && find2(t->exact, t->nexact, u) < 0) return -1;
    return t->rev[k][2];
}
int w32nls_utf8_dec(const uint8_t *b, int n, uint32_t *cp, bool *bad) {
    *bad = false;
    if (n <= 0) return 0;
    uint8_t b0 = b[0];
    if (b0 < 0x80) { *cp = b0; return 1; }
    int need, lo = 0x80, hi = 0xBF;
    uint32_t v;
    if (b0 >= 0xC2 && b0 <= 0xDF) { need = 1; v = b0 & 0x1Fu; }
    else if (b0 >= 0xE0 && b0 <= 0xEF) {
        need = 2; v = b0 & 0x0Fu;
        if (b0 == 0xE0) lo = 0xA0;
        if (b0 == 0xED) hi = 0x9F;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
        need = 3; v = b0 & 0x07u;
        if (b0 == 0xF0) lo = 0x90;
        if (b0 == 0xF4) hi = 0x8F;
    } else { *cp = 0xFFFD; *bad = true; return 1; }      /* 80..C1, F5..FF */
    for (int k = 0; k < need; k++) {
        if (1 + k >= n) return -1;                        /* the end cuts it short */
        uint8_t c = b[1 + k];
        if (c < 0x80 || c > 0xBF) { *cp = 0xFFFD; *bad = true; return 1 + k; }
        if (k == 0 && (c < lo || c > hi)) { *cp = 0xFFFD; *bad = true; return 2; }
        v = (v << 6) | (c & 0x3Fu);
    }
    *cp = v;
    return need + 1;
}

/* ---- Unicode -------------------------------------------------------------- */
static uint16_t case_map(const int32_t (*t)[4], int n, uint16_t u) {
    int lo = 0, hi = n - 1, best = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (t[m][0] <= (int32_t)u) { best = m; lo = m + 1; } else hi = m - 1;
    }
    if (best < 0 || (int32_t)u > t[best][1]) return u;
    if (((int32_t)u - t[best][0]) % t[best][3]) return u;
    return (uint16_t)((int32_t)u + t[best][2]);
}
uint16_t w32nls_uupper(uint16_t u) {
    return case_map(w32nls_upper, (int)(sizeof w32nls_upper / sizeof w32nls_upper[0]) - 1, u);
}
uint16_t w32nls_ulower(uint16_t u) {
    return case_map(w32nls_lower, (int)(sizeof w32nls_lower / sizeof w32nls_lower[0]) - 1, u);
}
uint16_t w32nls_uclass(uint16_t u) {
    int n = (int)(sizeof w32nls_ctype1 / sizeof w32nls_ctype1[0]) - 1;
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (u < w32nls_ctype1[m][0]) hi = m - 1;
        else if (u > w32nls_ctype1[m][1]) lo = m + 1;
        else return w32nls_ctype1[m][2];
    }
    return 0;
}

/* ---- the C runtime's tables ------------------------------------------------ */
const uint16_t *w32nls_pctype(uint32_t cp) {
    switch (cp) {
    case 1252: return w32nls_pctype_1252;
    case 1250: return w32nls_pctype_1250;
    case 1251: return w32nls_pctype_1251;
    case 1254: return w32nls_pctype_1254;
    default:   return w32nls_pctype_c;
    }
}
const uint8_t *w32nls_toupper_tab(uint32_t cp) {
    switch (cp) {
    case 1252: return w32nls_toupper_1252;
    case 1250: return w32nls_toupper_1250;
    case 1251: return w32nls_toupper_1251;
    case 1254: return w32nls_toupper_1254;
    default:   return w32nls_toupper_c;
    }
}
const uint8_t *w32nls_tolower_tab(uint32_t cp) {
    switch (cp) {
    case 1252: return w32nls_tolower_1252;
    case 1250: return w32nls_tolower_1250;
    case 1251: return w32nls_tolower_1251;
    case 1254: return w32nls_tolower_1254;
    default:   return w32nls_tolower_c;
    }
}

/* ---- locales ---------------------------------------------------------------- */
#define LT_SNAME 0x5Cu
static int lt_ix(uint32_t lt) {
    lt &= 0x07FFFFFFu;               /* NOUSEROVERRIDE, USE_CP_ACP, RETURN_NUMBER, GENITIVE, ALLOW_NEUTRAL */
    for (int j = 0; j < W32NLS_NLT; j++)
        if (w32nls_lctype[j] == lt) return j;
    return -1;
}
int w32nls_nloc(void) { return W32NLS_NLOC; }
static int lc_ascii(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int w32nls_find(const uint16_t *name) {
    if (!name) return w32nls_user_default();
    static const char sysdef[] = "!x-sys-default-locale";
    int i = 0;
    for (; sysdef[i] && name[i] == (uint16_t)sysdef[i]; i++) { }
    if (!sysdef[i] && !name[i]) return w32nls_user_default();
    int j = lt_ix(LT_SNAME);
    for (int k = 0; k < W32NLS_NLOC; k++) {
        const uint16_t *s = w32nls_locales[k].s[j];
        int p = 0;
        while (s[p] && name[p] && lc_ascii(s[p]) == lc_ascii(name[p])) p++;
        if (!s[p] && !name[p]) return k;
    }
    return -1;
}
int w32nls_user_default(void) { return 0; }         /* en-US, the table's first */
int w32nls_invariant(void) { return W32NLS_NLOC - 1; }
int w32nls_find_lcid(uint32_t lcid) {
    lcid &= 0xFFFFu;
    if (lcid == 0x400 || lcid == 0x800 || lcid == 0xC00 || lcid == 0 || lcid == 0x1000)
        return w32nls_user_default();               /* the user's, the system's, the custom default */
    if (lcid == 0x7F) return w32nls_invariant();
    for (int k = 0; k < W32NLS_NLOC; k++)
        if (w32nls_locales[k].lcid == lcid) return k;
    return -1;
}
uint32_t w32nls_lcid(int loc) { return loc >= 0 && loc < W32NLS_NLOC ? w32nls_locales[loc].lcid : 0; }
const uint16_t *w32nls_info(int loc, uint32_t lctype) {
    if (loc < 0 || loc >= W32NLS_NLOC) return 0;
    int j = lt_ix(lctype);
    return j < 0 ? 0 : (const uint16_t *)w32nls_locales[loc].s[j];
}
const uint16_t *w32nls_genitive(int loc, int month) {
    if (loc < 0 || loc >= W32NLS_NLOC || month < 0 || month >= W32NLS_NGEN) return 0;
    return (const uint16_t *)w32nls_locales[loc].gen[month];
}
static const uint16_t *genitive_abbrev(int loc, int month) {
    if (loc < 0 || loc >= W32NLS_NLOC || month < 0 || month >= W32NLS_NGEN) return 0;
    return (const uint16_t *)w32nls_locales[loc].gena[month];
}
bool w32nls_info_is_number(uint32_t lt) {
    switch (lt & 0x07FFFFFFu) {
    case 0x01: case 0x05: case 0x0B: case 0x0D: case 0x11: case 0x12: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x100C: case 0x100D: case 0x1004: case 0x1010: case 0x1011:
        return true;
    }
    return false;
}
uint32_t w32nls_info_number(int loc, uint32_t lt) {
    const uint16_t *s = w32nls_info(loc, lt);
    if (!s) return 0;
    bool hex = (lt & 0x07FFFFFFu) == 0x01;         /* LOCALE_ILANGUAGE: a LANGID in hex */
    uint32_t v = 0;
    for (; *s; s++) {
        int d = -1;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (hex && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (hex && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        if (d < 0) break;
        v = v * (hex ? 16u : 10u) + (uint32_t)d;
    }
    return v;
}
static bool loc_is(int loc, const char *lang) {
    const uint16_t *s = w32nls_info(loc, 0x59);    /* LOCALE_SISO639LANGNAME */
    if (!s) return false;
    int i = 0;
    for (; lang[i] && s[i] == (uint16_t)lang[i]; i++) { }
    return !lang[i] && !s[i];
}

/* ---- case mapping ------------------------------------------------------------ */
void w32nls_case_map(int loc, uint32_t flags, uint16_t *s, int n) {
    bool up = (flags & 0x200u) != 0, low = (flags & 0x100u) != 0;
    bool turk = (flags & 0x01000000u) && (loc_is(loc, "tr") || loc_is(loc, "az"));
    for (int i = 0; i < n; i++) {
        uint16_t c = s[i];
        if (up) {
            if (turk && c == 'i') c = 0x0130;
            else if (turk && c == 0x0131) c = 'I';
            else c = w32nls_uupper(c);
        } else if (low) {
            if (turk && c == 'I') c = 0x0131;
            else if (turk && c == 0x0130) c = 'i';
            else c = w32nls_ulower(c);
        }
        s[i] = c;
    }
}

/* ---- collation ---------------------------------------------------------------- */
/* A Latin letter's base, U+00C0..U+017F: a letter, or 1 "ae", 2 "th", 3 "ss",
 * 4 "ij", 5 "oe", 0 not a letter */
static const char g_lat[] =
    "aaaaaa\1ceeeeiiiidnooooo\0ouuuuy\2\3"                   /* C0..DF */
    "aaaaaa\1ceeeeiiiidnooooo\0ouuuuy\2y"                    /* E0..FF */
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiii\4\4jjkkkllllllllll"
    "nnnnnnnnnoooooo\5\5rrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs"; /* 100..17F */
struct w32nls_ce { uint32_t p; uint16_t s, t; };
#define CE_SPECIAL 0xFFFFFFFFu
#define P_SYM   0x000100u      /* + the symbol's rank in Windows' string sort */
#define P_SYMX  0x001000u      /* + a symbol's unit when it has no rank here */
#define P_DIGIT 0x020000u
#define P_LATIN 0x030000u
#define P_GREEK 0x040000u
#define P_CYR   0x050000u
#define P_OTHER 0x100000u
/* a symbol's rank (Windows' string sort), 0 for none here; rank 1 is the
 * ignorable ones (soft hyphen, joiners, direction marks) */
static int sym_rank(uint16_t c) {
    for (int i = 0; w32nls_symrank[i][0]; i++)
        if (w32nls_symrank[i][0] == c) return w32nls_symrank[i][1];
    return 0;
}
/* the elements of character c: their count (0..3), or -1 for a word-sort
 * ignorable (a hyphen, an apostrophe, a soft hyphen) */
static bool is_hy(uint16_t c) { return c == '-' || c == '\'' || c == 0x2010 || c == 0x2011; }
static int coll_elems(bool sv, uint32_t flags, uint16_t c, bool trailing, struct w32nls_ce *e) {
    uint16_t cls = w32nls_uclass(c);
    uint16_t t = (cls & 0x01) ? 1 : 0;                     /* C1_UPPER: capitals after the small */
    int rank = (cls & (0x100 | 0x04)) ? 0 : sym_rank(c);
    if (rank == 1) return 0;                               /* ignorable everywhere */
    if (is_hy(c)) {
        if (flags & 0x4u) return 0;                        /* NORM_IGNORESYMBOLS */
        /* the word sort sets hyphens and apostrophes aside for its last
         * level; the string sort only those that end the string, the others
         * symbols (measured: "a-b" < "ab" but "a-" < "A" and "a-" > "a") */
        if (!(flags & 0x1000u) || trailing) return -1;
        e[0].p = P_SYM + (uint32_t)rank; e[0].s = 0; e[0].t = 0;
        return 1;
    }
    if (c >= '0' && c <= '9') { e[0].p = P_DIGIT + (uint32_t)(c - '0'); e[0].s = 0; e[0].t = 0; return 1; }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        e[0].p = P_LATIN + (uint32_t)(lc_ascii(c) - 'a') * 4; e[0].s = 0; e[0].t = t; return 1;
    }
    if (c >= 0xC0 && c <= 0x17F) {
        char b = g_lat[c - 0xC0];
        if (sv) {
            uint16_t lc = w32nls_ulower(c);
            int k = lc == 0xE5 ? 1 : (lc == 0xE4 || lc == 0xE6) ? 2 : (lc == 0xF6 || lc == 0xF8) ? 3 : 0;
            if (k) { e[0].p = P_LATIN + 26 * 4 + (uint32_t)k; e[0].s = (lc == 0xE6 || lc == 0xF8) ? 1 : 0; e[0].t = t; return 1; }
        }
        if (b >= 'a' && b <= 'z') {            /* the accent: the small letter's, whatever the case */
            e[0].p = P_LATIN + (uint32_t)(b - 'a') * 4; e[0].s = w32nls_ulower(c); e[0].t = t; return 1;
        }
        static const char *const exp[6] = { 0, "ae", "th", "ss", "ij", "oe" };
        if (b >= 1 && b <= 5) {
            const char *x = exp[(int)b];
            for (int k = 0; k < 2; k++) {
                e[k].p = P_LATIN + (uint32_t)(x[k] - 'a') * 4;
                e[k].s = (b == 3 || b == 4) ? 0 : (k == 0 ? w32nls_ulower(c) : 0);
                e[k].t = t;
            }
            return 2;
        }
    }
    if (cls & 0x100) {                                     /* C1_ALPHA */
        uint16_t lc = w32nls_ulower(c);
        if (c >= 0x370 && c < 0x400) { e[0].p = P_GREEK + lc; e[0].s = 0; e[0].t = t; return 1; }
        if (c >= 0x400 && c < 0x530) {
            if (lc == 0x451) { e[0].p = P_CYR + 0x435; e[0].s = 1; e[0].t = t; return 1; }    /* ё: е and its mark */
            e[0].p = P_CYR + lc; e[0].s = 0; e[0].t = t; return 1;
        }
        e[0].p = P_OTHER + lc; e[0].s = 0; e[0].t = t; return 1;
    }
    if (cls & 0x04) { e[0].p = P_DIGIT + 0xF0u; e[0].s = c; e[0].t = 0; return 1; }   /* another digit */
    if (flags & 0x4u) return 0;                            /* NORM_IGNORESYMBOLS */
    e[0].p = rank ? P_SYM + (uint32_t)rank : P_SYMX + c; e[0].s = 0; e[0].t = 0;
    return 1;
}
struct w32nls_ci {
    const uint16_t *s; int n, i;
    struct w32nls_ce pend[3]; int np, ip;
    bool sv; uint32_t flags;
    int pos;                                   /* elements given */
    int trail;                                 /* from here the string is hyphens and apostrophes */
    uint16_t spc; int spos;                    /* the last ignorable met, and its place */
};
/* the next element: 1, 0 at the end; an ignorable is given as CE_SPECIAL */
static int ci_next(struct w32nls_ci *it, struct w32nls_ce *out) {
    for (;;) {
        if (it->ip < it->np) { *out = it->pend[it->ip++]; it->pos++; return 1; }
        if (it->i >= it->n) return 0;
        bool trailing = it->i >= it->trail;
        uint16_t c = it->s[it->i++];
        int k = coll_elems(it->sv, it->flags, c, trailing, it->pend);
        if (k < 0) { out->p = CE_SPECIAL; out->s = c; out->t = (uint16_t)it->pos; return 1; }
        it->np = k; it->ip = 0;
    }
}
static void ci_init(struct w32nls_ci *it, const uint16_t *s, int n, bool sv, uint32_t flags) {
    it->s = s; it->n = n; it->i = 0; it->np = it->ip = 0; it->sv = sv; it->flags = flags; it->pos = 0;
    it->trail = n;
    while (it->trail > 0 && (is_hy(s[it->trail - 1]) || sym_rank(s[it->trail - 1]) == 1)) it->trail--;
    if (it->trail == 0) it->trail = n;       /* nothing else: they are the string, symbols */
}
/* a run of ASCII digits from the element stream: its value's digits (no
 * leading zeros) in d, their count */
static int digit_run(struct w32nls_ci *it, uint32_t first, char *d, int cap) {
    int n = 0;
    d[n++] = (char)('0' + (first - P_DIGIT));
    while (it->i < it->n && it->ip >= it->np && it->s[it->i] >= '0' && it->s[it->i] <= '9') {
        if (n < cap) d[n++] = (char)it->s[it->i];
        it->i++;
        it->pos++;
    }
    int z = 0;
    while (z < n - 1 && d[z] == '0') z++;
    for (int k = z; k < n; k++) d[k - z] = d[k];
    return n - z;
}
static int cmp_level(int level, const uint16_t *a, int na, const uint16_t *b, int nb, bool sv, uint32_t flags) {
    struct w32nls_ci x, y;
    ci_init(&x, a, na, sv, flags);
    ci_init(&y, b, nb, sv, flags);
    for (;;) {
        struct w32nls_ce ea, eb;
        int ra, rb;
        do { ra = ci_next(&x, &ea); } while (ra && ea.p == CE_SPECIAL && level < 4);
        do { rb = ci_next(&y, &eb); } while (rb && eb.p == CE_SPECIAL && level < 4);
        if (level == 5) {                                  /* string sort: the whole, them as symbols */
            if (ra && ea.p == CE_SPECIAL) ea.p = P_SYM + (uint32_t)sym_rank(ea.s);
            if (rb && eb.p == CE_SPECIAL) eb.p = P_SYM + (uint32_t)sym_rank(eb.s);
            if (!ra || !rb) return ra ? 1 : rb ? -1 : 0;
            if (ea.p != eb.p) return ea.p < eb.p ? -1 : 1;
            continue;
        }
        if (level == 4) {                                  /* word sort: the set-aside ones' places */
            while (ra && ea.p != CE_SPECIAL) ra = ci_next(&x, &ea);
            while (rb && eb.p != CE_SPECIAL) rb = ci_next(&y, &eb);
            if (!ra && !rb) return 0;
            if (!ra) return -1;
            if (!rb) return 1;
            if (ea.t != eb.t) return ea.t < eb.t ? 1 : -1;
            if (ea.s != eb.s) return ea.s < eb.s ? -1 : 1;
            continue;
        }
        if (!ra || !rb) return ra ? 1 : rb ? -1 : 0;
        if (level == 1 && (flags & 0x8u) && ea.p >= P_DIGIT && ea.p <= P_DIGIT + 9 &&
            eb.p >= P_DIGIT && eb.p <= P_DIGIT + 9) {      /* SORT_DIGITSASNUMBERS */
            char da[64], db[64];
            int la = digit_run(&x, ea.p, da, 64), lb = digit_run(&y, eb.p, db, 64);
            if (la != lb) return la < lb ? -1 : 1;
            for (int k = 0; k < la; k++) if (da[k] != db[k]) return da[k] < db[k] ? -1 : 1;
            continue;
        }
        uint32_t va = level == 1 ? ea.p : level == 2 ? ea.s : ea.t;
        uint32_t vb = level == 1 ? eb.p : level == 2 ? eb.s : eb.t;
        if (va != vb) return va < vb ? -1 : 1;
    }
}
int w32nls_compare(int loc, uint32_t flags, const uint16_t *a, int na, const uint16_t *b, int nb) {
    bool sv = loc_is(loc, "sv") || loc_is(loc, "fi");
    int r = cmp_level(1, a, na, b, nb, sv, flags);
    if (!r && !(flags & (0x2u | 0x20u))) r = cmp_level(2, a, na, b, nb, sv, flags);   /* IGNORENONSPACE */
    if (!r && !(flags & (0x1u | 0x10u))) r = cmp_level(3, a, na, b, nb, sv, flags);   /* IGNORECASE */
    if (!r) r = cmp_level((flags & 0x1000u) ? 5 : 4, a, na, b, nb, sv, flags);   /* SORT_STRINGSORT */
    return r < 0 ? 1 : r > 0 ? 3 : 2;
}

/* ---- formats ---------------------------------------------------------------- */
static int put_s(uint16_t *out, int cap, int o, const uint16_t *s) {
    for (; s && *s; s++) {
        if (o >= cap) return -1;
        out[o++] = *s;
    }
    return o;
}
static int put_num(uint16_t *out, int cap, int o, int v, int width) {
    char d[12];
    int n = 0;
    if (v < 0) v = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < width) d[n++] = '0';
    while (n) {
        if (o >= cap) return -1;
        out[o++] = (uint16_t)d[--n];
    }
    return o;
}
/* Sunday 0 */
static int weekday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[(m - 1) % 12] + d) % 7;
}
int w32nls_fmt_picture(int loc, const uint16_t *pic, const struct w32nls_time *t, uint16_t *out, int cap) {
    bool genitive = false;                     /* a day's number in the picture: MMMM genitive */
    {
        bool q = false;
        for (int i = 0; pic[i]; ) {
            if (pic[i] == '\'') { q = !q; i++; continue; }
            int r = 1;
            while (pic[i + r] == pic[i]) r++;
            if (!q && pic[i] == 'd' && r <= 2) genitive = true;
            i += r;
        }
    }
    int o = 0, after_field = 0;                /* the end of what the last field wrote */
    for (int i = 0; pic[i] && o >= 0; ) {
        uint16_t c = pic[i];
        if (c == '\'') {                       /* quoted text; '' a quote */
            i++;
            while (pic[i]) {
                if (pic[i] == '\'') {
                    if (pic[i + 1] == '\'') { if (o >= cap) return -1; out[o++] = '\''; i += 2; continue; }
                    i++;
                    break;
                }
                if (o >= cap) return -1;
                out[o++] = pic[i++];
            }
            continue;
        }
        int r = 1;
        while (pic[i + r] == c) r++;
        int wd = weekday(t->year, t->month, t->day), h12 = t->hour % 12 ? t->hour % 12 : 12;
        switch (c) {
        case 'd':
            if (r <= 2) o = put_num(out, cap, o, t->day, r);
            else o = put_s(out, cap, o, w32nls_info(loc, (r == 3 ? 0x31u : 0x2Au) + (uint32_t)((wd + 6) % 7)));
            break;
        case 'M':
            if (r <= 2) o = put_num(out, cap, o, t->month, r);
            else if (r == 3) o = put_s(out, cap, o, genitive ? genitive_abbrev(loc, t->month - 1)
                                                         : w32nls_info(loc, 0x44u + (uint32_t)(t->month - 1)));
            else o = put_s(out, cap, o, genitive ? w32nls_genitive(loc, t->month - 1)
                                                 : w32nls_info(loc, 0x38u + (uint32_t)(t->month - 1)));
            break;
        case 'y':
            if (r <= 2) o = put_num(out, cap, o, t->year % 100, r);
            else o = put_num(out, cap, o, t->year, r == 3 ? 4 : r);
            break;
        case 'h': o = put_num(out, cap, o, h12, r > 2 ? 2 : r); break;
        case 'H': o = put_num(out, cap, o, t->hour, r > 2 ? 2 : r); break;
        case 'm': o = put_num(out, cap, o, t->min, r > 2 ? 2 : r); break;
        case 's': o = put_num(out, cap, o, t->sec, r > 2 ? 2 : r); break;
        case 't': {
            const uint16_t *m = w32nls_info(loc, t->hour < 12 ? 0x28u : 0x29u);
            if (!m || !m[0]) { o = after_field; break; }   /* none: the text before it goes too */
            if (r == 1) { if (o >= cap) return -1; out[o++] = m[0]; }
            else o = put_s(out, cap, o, m);
            break;
        }
        case 'g': break;                       /* an era: none kept */
        default:
            for (int k = 0; k < r && o >= 0; k++) { if (o >= cap) return -1; out[o++] = c; }
            i += r;
            continue;
        }
        if (o < 0) return -1;
        after_field = o;
        i += r;
    }
    return o;
}
void w32nls_time_picture(const uint16_t *pic, uint32_t flags, uint16_t *p2, int cap) {
    int o = 0, cut = -1;                       /* where the text after the last kept field starts */
    bool q = false;
    for (int i = 0; pic[i] && o < cap - 1; ) {
        uint16_t c = pic[i];
        if (c == '\'') { q = !q; p2[o++] = c; i++; continue; }
        int r = 1;
        while (pic[i + r] == c) r++;
        bool field = !q && (c == 'h' || c == 'H' || c == 'm' || c == 's' || c == 't');
        bool drop = field && (((flags & 2u) && c == 's') || ((flags & 1u) && (c == 'm' || c == 's')) ||
                              ((flags & 4u) && c == 't'));
        if (drop) {
            if (cut >= 0) o = cut;             /* the separator before it goes too */
            i += r;
            if (c == 't') while (pic[i] == ' ') i++;
            continue;
        }
        for (int k = 0; k < r && o < cap - 1; k++)
            p2[o++] = (field && c == 'h' && (flags & 8u)) ? 'H' : c;
        if (field) cut = o;
        i += r;
    }
    while (o > 0 && p2[o - 1] == ' ') o--;
    p2[o] = 0;
}
static void copy16(uint16_t *d, int cap, const uint16_t *s) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
void w32nls_numfmt_of(int loc, struct w32nls_numfmt *nf) {
    nf->digits = (int)w32nls_info_number(loc, 0x11);       /* LOCALE_IDIGITS */
    nf->lzero = (int)w32nls_info_number(loc, 0x12);        /* LOCALE_ILZERO */
    nf->negorder = (int)w32nls_info_number(loc, 0x1010);   /* LOCALE_INEGNUMBER */
    copy16(nf->dec, 8, w32nls_info(loc, 0x0E));
    copy16(nf->thou, 8, w32nls_info(loc, 0x0F));
    copy16(nf->neg, 8, w32nls_info(loc, 0x51));
    const uint16_t *g = w32nls_info(loc, 0x10);            /* LOCALE_SGROUPING "3;0" */
    nf->ngroup = 0;
    nf->repeat = false;
    int v = 0;
    bool have = false;
    for (int i = 0; g; i++) {
        uint16_t c = g[i];
        if (c >= '0' && c <= '9') { v = v * 10 + (c - '0'); have = true; continue; }
        if (have) {
            if (v == 0) nf->repeat = true;
            else if (nf->ngroup < 4) nf->group[nf->ngroup++] = v;
        }
        v = 0;
        have = false;
        if (!c) break;
    }
}
int w32nls_fmt_number(const struct w32nls_numfmt *nf, const char *val, uint16_t *out, int cap) {
    const char *p = val;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    char ip[64], fp[64];
    int ni = 0, nfr = 0;
    while (*p >= '0' && *p <= '9') { if (ni < 63) ip[ni++] = *p; p++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { if (nfr < 63) fp[nfr++] = *p; p++; }
    }
    if (*p || (!ni && !nfr)) return -2;
    /* round to nf->digits fraction digits, half up */
    int nd = nf->digits < 0 ? 0 : nf->digits > 9 ? 9 : nf->digits;
    bool carry = nfr > nd && fp[nd] >= '5';
    while (nfr < nd) fp[nfr++] = '0';
    nfr = nd;
    for (int k = nfr - 1; carry && k >= 0; k--) {
        if (fp[k] == '9') fp[k] = '0'; else { fp[k]++; carry = false; }
    }
    for (int k = ni - 1; carry && k >= 0; k--) {
        if (ip[k] == '9') ip[k] = '0'; else { ip[k]++; carry = false; }
    }
    if (carry) {
        if (ni >= 63) ni = 62;
        for (int k = ni; k > 0; k--) ip[k] = ip[k - 1];
        ip[0] = '1';
        ni++;
    }
    int z = 0;
    while (z < ni - 1 && ip[z] == '0') z++;
    if (z) { for (int k = z; k < ni; k++) ip[k - z] = ip[k]; ni -= z; }
    if (!ni) { ip[0] = '0'; ni = 1; }
    bool zero = ni == 1 && ip[0] == '0';
    for (int k = 0; k < nfr; k++) if (fp[k] != '0') zero = false;
    if (zero) neg = false;                     /* -0.004 at two digits: 0.00, no sign */
    if (ni == 1 && ip[0] == '0' && !nf->lzero && nfr) ni = 0;
    /* the integer part, grouped from the right */
    uint16_t body[200];
    int nb = 0;
    {
        int sep_at[64], ns = 0, pos = ni, gi = 0;
        while (nf->ngroup > 0 && ns < 64) {    /* "3;0": threes; "3;2;0": three, then twos */
            int g;
            if (gi < nf->ngroup) g = nf->group[gi];
            else if (nf->repeat) g = nf->group[nf->ngroup - 1];
            else break;
            if (g <= 0 || pos - g <= 0) break;
            pos -= g;
            sep_at[ns++] = pos;
            gi++;
        }
        for (int k = 0; k < ni; k++) {
            for (int s = 0; s < ns; s++)
                if (sep_at[s] == k)
                    for (int m = 0; nf->thou[m] && nb < 190; m++) body[nb++] = nf->thou[m];
            if (nb < 190) body[nb++] = (uint16_t)ip[k];
        }
        if (nfr) {
            for (int m = 0; nf->dec[m] && nb < 190; m++) body[nb++] = nf->dec[m];
            for (int k = 0; k < nfr && nb < 190; k++) body[nb++] = (uint16_t)fp[k];
        }
        body[nb] = 0;
    }
    int o = 0;
    if (!neg) return put_s(out, cap, 0, body);
    switch (nf->negorder) {
    case 0: o = put_s(out, cap, o, (const uint16_t *)u"("); if (o >= 0) o = put_s(out, cap, o, body);
            if (o >= 0) o = put_s(out, cap, o, (const uint16_t *)u")"); break;
    case 2: o = put_s(out, cap, o, nf->neg); if (o >= 0) o = put_s(out, cap, o, (const uint16_t *)u" ");
            if (o >= 0) o = put_s(out, cap, o, body); break;
    case 3: o = put_s(out, cap, o, body); if (o >= 0) o = put_s(out, cap, o, nf->neg); break;
    case 4: o = put_s(out, cap, o, body); if (o >= 0) o = put_s(out, cap, o, (const uint16_t *)u" ");
            if (o >= 0) o = put_s(out, cap, o, nf->neg); break;
    default: o = put_s(out, cap, o, nf->neg); if (o >= 0) o = put_s(out, cap, o, body); break;
    }
    return o;
}
