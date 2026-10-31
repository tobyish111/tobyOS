/* shell_histexp.inc -- history expansion: `!!`, `!n`, `!-n`, `!str`,
 * `!?str?`, `!#`, `^old^new^`, word designators and modifiers (batch 13).
 *
 * tsh had none: `set -H` was an option that did nothing and `!!` was two
 * characters. This follows bash 5.2 (its readline history library and
 * bashhist.c), measured case by case (logs/b13 q29): a line is expanded as
 * it is READ, before it is parsed, when history is being recorded and
 * histexpand is on -- at a terminal both are, in a script neither unless it
 * asks (`set -o history -H`). The expanded line is echoed on standard error
 * and is what runs and what the history records; a failed expansion is
 * reported and the line dropped; `:p` echoes and records it without running
 * it. A here-document body is never expanded.
 *
 * Included by shell.c after the history list (g_hist). */

#define HX_WDELIM " \t\n;&()|<>"     /* what ends a word (history_word_delimiters) */
#define HX_NOEXP  " \t\n\r="          /* a `!` before one of these is itself */
#define HX_SDELIM ";&()|<>"           /* ends a !string search */
#define HX_EDELIM "^$*%-"             /* ...and so do these, but for a leading `-` */

static char *g_hx_lhs, *g_hx_rhs;     /* the last :s/LHS/RHS/ */
static char *g_hx_search;             /* the last !?string? */
static char *g_hx_match;              /* the word it was found in, for `%` */

struct hx_cfg { char bang, hat, cmt; };

/* $histchars: the expansion character, the quick-substitution character and
 * the comment character -- `!`, `^` and `#` when it is unset. */
static void hx_cfg_get(struct hx_cfg *c) {
    const char *h = env_get("histchars");
    c->bang = '!';
    c->hat = '^';
    c->cmt = '#';
    if (h) {
        c->bang = h[0];
        if (h[0] && h[1]) {
            c->hat = h[1];
            if (h[2]) c->cmt = h[2];
        }
    }
}

static bool hx_in(char c, const char *set);

/* strchr and strrchr: the kernel's string library has neither */
static const char *hx_chr(const char *s, char c) {
    for (; *s; s++) if (*s == c) return s;
    return 0;
}
static char *hx_rchr(char *s, char c) {
    char *r = 0;
    for (; *s; s++) if (*s == c) r = s;
    return r;
}

static bool hx_in(char c, const char *set) { return c && hx_chr(set, c) != 0; }
static bool hx_digit(char c) { return c >= '0' && c <= '9'; }

static size_t hx_clen(const char *s, size_t i) {
    if ((unsigned char)s[i] < 0x80 || !shell_utf8()) return 1;
    return sh_cl(s + i, s + i + strlen(s + i), true);
}

/* ---- words, as the history library cuts them (history_tokenize) ------- */

/* The end of the word that starts at S[I]. */
static size_t hx_tokword(const char *s, size_t i) {
    char delim = 0, dopen = 0;
    int nest = 0;
    if (hx_in(s[i], "()\n")) return i + 1;
    if (hx_digit(s[i])) {
        size_t j = i;
        while (hx_digit(s[j])) j++;
        if (!s[j]) return j;
        i = j;
        if (s[j] != '<' && s[j] != '>') goto word;     /* digits in a word */
    }
    if (hx_in(s[i], "<>;&|")) {
        char pk = s[i + 1];
        if (pk == s[i]) {
            if (pk == '<' && (s[i + 2] == '-' || s[i + 2] == '<')) i++;
            return i + 2;
        }
        if (pk == '&' && (s[i] == '>' || s[i] == '<')) {
            size_t j = i + 2;
            while (hx_digit(s[j])) j++;
            if (s[j] == '-') j++;
            return j;
        }
        if ((pk == '>' && s[i] == '&') || (pk == '|' && s[i] == '>')) return i + 2;
        if (pk == '(' && (s[i] == '>' || s[i] == '<')) {
            i += 2;
            dopen = '(';
            delim = ')';
            nest = 1;
            goto word;
        }
        return i + 1;
    }
word:
    if (!delim && hx_in(s[i], "\"'`")) delim = s[i++];
    for (; s[i]; i++) {
        if (s[i] == '\\' && s[i + 1] == '\n') { i++; continue; }
        if (s[i] == '\\' && delim != '\'' &&
            (delim != '"' || hx_in(s[i + 1], "\\`\"$"))) {
            i++;
            continue;
        }
        if (nest && s[i] == dopen) { nest++; continue; }
        if (nest && s[i] == delim) {
            if (--nest == 0) delim = 0;
            continue;
        }
        if (delim && s[i] == delim) { delim = 0; continue; }
        /* $( <( >( and the extglob groups: one word to the `)` -- stepping
         * over the character after the `(` as bash's loop does */
        if (!nest && !delim && hx_in(s[i], "<>$!@?+*") && s[i + 1] == '(') {
            i += 2;
            dopen = '(';
            delim = ')';
            nest = 1;
            continue;
        }
        if (!delim && hx_in(s[i], HX_WDELIM)) break;
        if (!delim && hx_in(s[i], "\"'`")) delim = s[i];
    }
    return i;
}

struct hx_word { size_t s, e; };

/* The words of S, into *OUT (kfree it); the index of the one holding byte
 * AT in *ATW (-1: none). A word that starts with the comment character
 * ends the list. */
static int hx_tokenize(const char *s, char cmt, struct hx_word **out,
                       size_t at, int *atw) {
    int cap = 0, n = 0;
    struct hx_word *w = 0;
    if (atw) *atw = -1;
    size_t i = 0;
    while (s[i]) {
        while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') i++;
        if (!s[i] || (cmt && s[i] == cmt)) break;
        size_t st = i;
        i = hx_tokword(s, st);
        if (i == st) {
            i++;
            while (s[i] && hx_in(s[i], HX_WDELIM)) i++;
        }
        if (atw && at >= st && at < i) *atw = n;
        if (n == cap) {
            int nc = cap ? cap * 2 : 16;
            struct hx_word *nw = (struct hx_word *)kmalloc((size_t)nc * sizeof *nw);
            if (!nw) { kfree(w); return -1; }
            if (n) memcpy(nw, w, (size_t)n * sizeof *nw);
            kfree(w);
            w = nw;
            cap = nc;
        }
        w[n].s = st;
        w[n].e = i;
        n++;
    }
    *out = w;
    return n;
}

/* Words FIRST..LAST of LINE, a blank between each (history_arg_extract):
 * a negative bound counts from the end, '$' -- the CHARACTER, 36, as in
 * bash -- is the last word. -1: there are no such words. */
static int hx_args(const char *line, char cmt, long first, long last,
                   struct shell_sb *out) {
    struct hx_word *w = 0;
    int len = hx_tokenize(line, cmt, &w, 0, 0);
    if (len <= 0) { kfree(w); return -1; }
    if (last < 0) last = len + last - 1;
    if (first < 0) first = len + first - 1;
    if (last == '$') last = len - 1;
    if (first == '$') first = len - 1;
    last++;
    int rc = 0;
    if (first >= len || last > len || first < 0 || last < 0 || first > last) {
        rc = -1;
    } else {
        for (long k = first; k < last && rc == 0; k++) {
            if (k > first && sb_putc(out, ' ') < 0) rc = -1;
            else if (sb_putmem(out, line + w[k].s, w[k].e - w[k].s) < 0) rc = -1;
        }
        if (rc == 0 && sb_puts(out, "") < 0) rc = -1;
    }
    kfree(w);
    return rc;
}

/* ---- events ----------------------------------------------------------- */

static const char *hx_entry(long idx) {
    return (idx >= 0 && idx < g_hist_count) ? g_hist[idx] : 0;
}

/* The line the event at S[*PI] (a `!`) names, *PI moved past it; 0 when
 * there is none (get_history_event). QC is the quote the `!` sits in. */
static const char *hx_event(const char *s, size_t *pi, char qc, char bang,
                            char cmt) {
    size_t i = *pi + 1;
    long hlen = g_hist_count;
    if (s[i] == bang) {
        *pi = i + 1;
        return hx_entry(hlen - 1);
    }
    int sign = 1;
    if (s[i] == '-' && hx_digit(s[i + 1])) { sign = -1; i++; }
    if (hx_digit(s[i])) {
        long which = 0;
        while (hx_digit(s[i])) {
            if (which < 100000000L) which = which * 10 + (s[i] - '0');
            i++;
        }
        *pi = i;
        long base = (long)g_hist_base + 1;
        if (sign < 0) which = (hlen + base) - which;
        return hx_entry(which - base);
    }
    bool sub = false;
    if (s[i] == '?') { sub = true; i++; }
    size_t li = i;
    for (; s[i]; i++) {
        size_t cl = hx_clen(s, i);
        if (cl > 1) { i += cl - 1; continue; }
        char c = s[i];
        if ((!sub && (c == ' ' || c == '\t' || c == ':' ||
                      (i > li && c == '-') ||
                      (c != '-' && hx_in(c, HX_EDELIM)) ||
                      hx_in(c, HX_SDELIM) || (qc && c == qc))) ||
            c == '\n' || (sub && c == '?'))
            break;
    }
    SH_SB(want);
    if (sb_putmem(&want, s + li, i - li) < 0 || sb_puts(&want, "") < 0) return 0;
    if (sub && s[i] == '?') i++;
    *pi = i;
    if (!*want.p && sub) {
        if (!g_hx_search) return 0;
        if (sb_set(&want, g_hx_search) < 0) return 0;
    }
    size_t wl = strlen(want.p);
    for (long k = hlen - 1; k >= 0; k--) {
        const char *h = g_hist[k];
        if (!sub) {
            if (strncmp(h, want.p, wl) == 0) return h;
            continue;
        }
        /* a substring: the LAST place it is on the line, and the word that
         * holds it is what `%` means from now on */
        size_t hl = strlen(h);
        if (wl > hl) continue;
        for (size_t at = hl - wl + 1; at-- > 0; ) {
            if (strncmp(h + at, want.p, wl) != 0) continue;
            char *ns = shell_strdup(want.p);
            if (ns) { kfree(g_hx_search); g_hx_search = ns; }
            kfree(g_hx_match);
            g_hx_match = 0;
            struct hx_word *w = 0;
            int ai = -1;
            int n = hx_tokenize(h, cmt, &w, at, &ai);
            if (n > 0 && ai >= 0) {
                g_hx_match = (char *)kmalloc(w[ai].e - w[ai].s + 1);
                if (g_hx_match) {
                    memcpy(g_hx_match, h + w[ai].s, w[ai].e - w[ai].s);
                    g_hx_match[w[ai].e - w[ai].s] = 0;
                }
            }
            kfree(w);
            return h;
        }
    }
    return 0;
}

/* A word designator at S[*PI] (get_history_word_specifier): 1 with the words
 * in OUT, 0 when there is none -- the event is meant whole -- and -1 when
 * the words it names do not exist. */
static int hx_wordspec(const char *s, size_t *pi, const char *ev, char cmt,
                       struct shell_sb *out) {
    size_t i = *pi;
    bool expecting = false;
    if (s[i] == ':') { i++; expecting = true; }
    if (s[i] == '%') {
        *pi = i + 1;
        return sb_puts(out, g_hx_match ? g_hx_match : "") < 0 ? -1 : 1;
    }
    if (s[i] == '*') {
        *pi = i + 1;
        if (hx_args(ev, cmt, 1, '$', out) < 0) sb_truncate(out, 0);
        return sb_puts(out, "") < 0 ? -1 : 1;
    }
    if (s[i] == '$') {
        *pi = i + 1;
        return hx_args(ev, cmt, '$', '$', out) < 0 ? 0 : 1;
    }
    long first, last;
    if (s[i] == '-') {
        first = 0;
    } else if (s[i] == '^') {
        first = 1;
        i++;
    } else if (hx_digit(s[i]) && expecting) {
        first = 0;
        while (hx_digit(s[i])) {
            if (first < 100000000L) first = first * 10 + (s[i] - '0');
            i++;
        }
    } else {
        return 0;
    }
    if (s[i] == '^' || s[i] == '*') {
        last = s[i] == '^' ? 1 : '$';
        i++;
    } else if (s[i] != '-') {
        last = first;
    } else {
        i++;
        if (hx_digit(s[i])) {
            last = 0;
            while (hx_digit(s[i])) {
                if (last < 100000000L) last = last * 10 + (s[i] - '0');
                i++;
            }
        } else if (s[i] == '$') {
            i++;
            last = '$';
        } else if (s[i] == '^') {
            i++;
            last = 1;
        } else {
            last = -1;                  /* x- : x to the next-to-last */
        }
    }
    *pi = i;
    if ((last >= first || last == '$' || last < 0) &&
        hx_args(ev, cmt, first, last, out) == 0)
        return 1;
    return -1;
}

/* ---- modifiers -------------------------------------------------------- */

/* The LHS or RHS of :s, from S[*PI] to DELIM -- a backslash quotes the
 * delimiter -- *PI left after the delimiter. An empty LHS is 0. */
static char *hx_pattern(const char *s, size_t *pi, char delim, bool rhs) {
    size_t i = *pi, si = i;
    for (; s[si] && s[si] != delim; si++) {
        size_t cl = hx_clen(s, si);
        if (cl > 1) si += cl - 1;
        else if (s[si] == '\\' && s[si + 1] == delim) si++;
    }
    char *r = 0;
    if (si > i || rhs) {
        r = (char *)kmalloc(si - i + 1);
        if (r) {
            size_t j = 0;
            for (size_t k = i; k < si; k++) {
                if (s[k] == '\\' && s[k + 1] == delim) k++;
                r[j++] = s[k];
            }
            r[j] = 0;
        }
    }
    *pi = s[si] ? si + 1 : si;
    return r;
}

/* :q -- bash's sh_single_quote */
static int hx_sq(struct shell_sb *o, const char *t) {
    if (t[0] == '\'' && !t[1]) return sb_puts(o, "\\'");
    if (sb_putc(o, '\'') < 0) return -1;
    for (const char *p = t; *p; p++) {
        if (sb_putc(o, *p) < 0) return -1;
        if (*p == '\'' && sb_puts(o, "\\''") < 0) return -1;
    }
    return sb_putc(o, '\'');
}

/* :x -- the same, every blank and newline a quoted word break of its own */
static int hx_sqx(struct shell_sb *o, const char *t) {
    if (sb_putc(o, '\'') < 0) return -1;
    for (const char *p = t; *p; p++) {
        if (*p == '\'') {
            if (sb_puts(o, "'\\''") < 0) return -1;
        } else if (*p == ' ' || *p == '\t' || *p == '\n') {
            if (sb_putc(o, '\'') < 0 || sb_putc(o, *p) < 0 || sb_putc(o, '\'') < 0)
                return -1;
        } else if (sb_putc(o, *p) < 0) {
            return -1;
        }
    }
    return sb_putc(o, '\'');
}

/* The text of an error: S[A, B) and bash's words. */
static void hx_err(struct shell_sb *err, const char *s, size_t a, size_t b,
                   const char *what) {
    sb_truncate(err, 0);
    if (s[a] && b > a) (void)sb_putmem(err, s + a, b - a);
    (void)(sb_puts(err, ": ") < 0 || sb_puts(err, what) < 0);
}

/* ONE expansion, at S[START] (a `!`): its text into OUT, *END at its last
 * byte (history_expand_internal). -1 on an error (in ERR), 1 when `:p`
 * asked for print-only, 0 otherwise. CUR is the line expanded so far, for
 * `!#`. */
static int hx_one(const char *s, size_t start, char qc, size_t *end,
                  struct shell_sb *out, struct shell_sb *err, const char *cur,
                  const struct hx_cfg *c) {
    size_t i = start;
    const char *ev;
    if (hx_in(s[i + 1], ":$*%^")) {          /* !:1 and the like: !! implied */
        i++;
        ev = hx_entry(g_hist_count - 1);
    } else if (s[i + 1] == '#') {
        i += 2;
        ev = cur;
    } else {
        ev = hx_event(s, &i, qc, c->bang, c->cmt);
    }
    if (!ev) {
        hx_err(err, s, start, i, "event not found");
        return -1;
    }
    SH_SB(t);
    size_t wstart = i;
    int ws = hx_wordspec(s, &i, ev, c->cmt, &t);
    if (ws < 0) {
        hx_err(err, s, wstart, i, "bad word specifier");
        return -1;
    }
    if (ws == 0 && (sb_set(&t, ev) < 0)) return -1;
    if (sb_puts(&t, "") < 0) return -1;

    bool global = false, bywords = false, print_only = false;
    char quotes = 0;
    size_t mstart = i;
    while (s[i] == ':') {
        char m = s[i + 1];
        if (m == 'g' || m == 'a') {
            global = true;
            i++;
            m = s[i + 1];
        } else if (m == 'G') {
            bywords = true;
            i++;
            m = s[i + 1];
        }
        switch (m) {
        case 'q': case 'x':
            quotes = m;
            break;
        case 'p':
            print_only = true;
            break;
        case 't': {
            char *sl = hx_rchr(t.p, '/');
            if (sl) {
                SH_SB(tail);
                if (sb_puts(&tail, sl + 1) < 0 || sb_set(&t, tail.p) < 0) return -1;
            }
            break;
        }
        case 'h': {
            char *sl = hx_rchr(t.p, '/');
            if (sl) sb_truncate(&t, (size_t)(sl - t.p));
            break;
        }
        case 'r': {
            char *dot = hx_rchr(t.p, '.');
            if (dot) sb_truncate(&t, (size_t)(dot - t.p));
            break;
        }
        case 'e': {
            char *dot = hx_rchr(t.p, '.');
            if (dot) {
                SH_SB(suf);
                if (sb_puts(&suf, dot) < 0 || sb_set(&t, suf.p) < 0) return -1;
            }
            break;
        }
        case '&': case 's': {
            if (m == 's') {
                size_t sl = strlen(s);
                if (i + 2 >= sl) break;         /* no delimiter: ignored */
                char delim = hx_clen(s, i + 2) > 1 ? 0 : s[i + 2];
                i += 3;
                char *l = hx_pattern(s, &i, delim, false);
                if (l) {
                    kfree(g_hx_lhs);
                    g_hx_lhs = l;
                } else if (!g_hx_lhs && g_hx_search && *g_hx_search) {
                    g_hx_lhs = shell_strdup(g_hx_search);
                }
                char *r = hx_pattern(s, &i, delim, true);
                kfree(g_hx_rhs);
                g_hx_rhs = 0;
                if (r && hx_chr(r, '&')) {
                    /* `&` in the replacement is the pattern; `\&` is `&` */
                    SH_SB(nr);
                    for (const char *q = r; *q; q++) {
                        if (*q == '&') {
                            if (sb_puts(&nr, g_hx_lhs ? g_hx_lhs : "") < 0) break;
                            continue;
                        }
                        if (*q == '\\' && q[1] == '&') q++;
                        if (sb_putc(&nr, *q) < 0) break;
                    }
                    g_hx_rhs = shell_strdup(nr.p ? nr.p : "");
                    kfree(r);
                } else {
                    g_hx_rhs = r;
                }
            } else {
                i += 2;
            }
            if (!g_hx_lhs || !*g_hx_lhs) {
                hx_err(err, s, mstart, i, "no previous substitution");
                return -1;
            }
            size_t ll = strlen(g_hx_lhs);
            const char *rhs = g_hx_rhs ? g_hx_rhs : "";
            size_t rl = strlen(rhs);
            if (ll > t.len) {
                hx_err(err, s, mstart, i, "substitution failed");
                return -1;
            }
            bool failed = true;
            size_t si = 0, we = 0;
            int gl = global ? 1 : 0;
            while (si + ll <= t.len) {
                if (bywords && si > we) {
                    while (t.p[si] == ' ' || t.p[si] == '\t' || t.p[si] == '\n') si++;
                    we = hx_tokword(t.p, si);
                }
                if (strncmp(t.p + si, g_hx_lhs, ll) == 0) {
                    SH_SB(nt);
                    if (sb_putmem(&nt, t.p, si) < 0 || sb_puts(&nt, rhs) < 0 ||
                        sb_puts(&nt, t.p + si + ll) < 0 || sb_set(&t, nt.p) < 0)
                        return -1;
                    failed = false;
                    if (gl) {
                        si += rl;
                        gl++;
                        continue;
                    }
                    if (bywords) {
                        si = we + 1;
                        continue;
                    }
                    break;
                }
                si++;
            }
            if (gl > 1) {
                global = false;
                continue;               /* past the pattern already */
            }
            if (!failed) continue;
            hx_err(err, s, mstart, i, "substitution failed");
            return -1;
        }
        default:
            hx_err(err, s, i + 1, i + 2, "unrecognized history modifier");
            return -1;
        }
        i += 2;
    }
    *end = i ? i - 1 : 0;
    int rc;
    if (quotes == 'q') rc = hx_sq(out, t.p);
    else if (quotes == 'x') rc = hx_sqx(out, t.p);
    else rc = sb_puts(out, t.p);
    if (rc < 0) return -1;
    return print_only ? 1 : 0;
}

/* ---- when a `!` is not one ---------------------------------------------- */

/* Past a '...' from S[I] (after the opening quote): the index after the
 * closing one, or the end. */
static size_t hx_past_sq(const char *s, size_t n, size_t i) {
    while (i < n && s[i] != '\'') i++;
    return i < n ? i + 1 : n;
}

static size_t hx_past_dq(const char *s, size_t n, size_t i) {
    while (i < n && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < n) i++;
        i++;
    }
    return i < n ? i + 1 : n;
}

/* bash's skip_to_histexp: the first `!` from START that no quoting hides
 * -- inside $( ) a quote counts again -- or the end. */
static size_t hx_next_bang(const char *s, size_t start, char bang) {
    size_t n = strlen(s), i = start;
    bool pass = false, backq = false;
    int dq = 0, old_dq = 0, comsub = 0;
    while (i < n) {
        char ch = s[i];
        if (pass) { pass = false; i += hx_clen(s, i); continue; }
        if (ch == '\\') { pass = true; i++; continue; }
        if (backq && ch == '`') { backq = false; dq = old_dq; i++; continue; }
        if (ch == '`') { backq = true; old_dq = dq; dq = 0; i++; continue; }
        if (dq && ch == bang && s[i + 1] == '"') { i++; continue; }
        if (ch == bang) return i;
        if (dq && ch == '\'') { i++; continue; }
        if (ch == '\'') { i = hx_past_sq(s, n, i + 1); continue; }
        if (!g_opt_posix && ch == '"') { dq = !dq; i++; continue; }
        if (ch == '"') { i = hx_past_dq(s, n, i + 1); continue; }
        if ((ch == '$' || ch == '<' || ch == '>') && s[i + 1] == '(' && s[i + 2] != '(') {
            if (!s[i + 2]) return i + 2;
            i += 2;
            comsub++;
            old_dq = dq;
            dq = 0;
            continue;
        }
        if (comsub && ch == ')') { comsub--; dq = old_dq; i++; continue; }
        i += hx_clen(s, i);
    }
    return i;
}

/* bash_history_inhibit_expansion: is the `!` at S[I] the shell's own -- a
 * bracket's negation, ${!name}, $!, an extglob !( ) -- or quoted? QS is the
 * quote a continued line starts in. */
static bool hx_inhibited(const char *s, size_t i, char bang, char qs) {
    if (i > 0 && s[i - 1] == '[' && hx_chr(s + i + 1, ']')) return true;
    if (i > 1 && s[i - 1] == '{' && s[i - 2] == '$' && hx_chr(s + i + 1, '}'))
        return true;
    if (i > 1 && s[i - 1] == '$' && s[i] == '!') return true;
    if (g_shopt_extglob && i > 1 && s[i + 1] == '(' && hx_chr(s + i + 2, ')'))
        return true;
    size_t si = 0;
    if (qs == '\'') {
        const char *q = hx_chr(s, '\'');
        if (!q || (size_t)(q - s) >= i) return true;
        si = (size_t)(q - s) + 1;
    }
    size_t t = hx_next_bang(s, si, bang);
    if (t == 0) return false;
    while (t < i) {
        t = hx_next_bang(s, t + 1, bang);
        if (t == 0) return false;
    }
    return t > i;
}

/* ---- the line ------------------------------------------------------------ */

/* Expand LINE (history_expand). QS is the quote an earlier line left open
 * (`'`, `"` or 0). 0: nothing to expand; 1: expanded, into OUT; 2: the
 * same, but `:p` -- show it, record it, do not run it; -1: an error,
 * described in ERR. */
static int shell_hist_expand(const char *line, char qs, struct shell_sb *out,
                             struct shell_sb *err) {
    struct hx_cfg c;
    hx_cfg_get(&c);
    if (!c.bang) return 0;
    SH_SB(qline);
    const char *s = line;
    if (c.hat && line[0] == c.hat) {     /* ^old^new^ is !!:s^old^new^ */
        char pre[5] = { c.bang, c.bang, ':', 's', 0 };
        if (sb_puts(&qline, pre) < 0 || sb_puts(&qline, line) < 0) return -1;
        s = qline.p;
    } else {
        /* Is there a `!` to expand at all? Asked of the line as written,
         * which is what lets `[!a]` and `${!v}` through untouched. */
        bool dq = qs == '"';
        size_t i = 0, n = strlen(s);
        if (qs == '\'') {
            while (i < n && s[i] != '\'') i++;
            if (i < n) i++;
        }
        bool found = false;
        for (; i < n; i++) {
            size_t cl = hx_clen(s, i);
            if (cl > 1) { i += cl - 1; continue; }
            char cc = s[i + 1];
            if (c.cmt && s[i] == c.cmt && !dq &&
                (i == 0 || hx_in(s[i - 1], HX_WDELIM))) {
                break;
            } else if (s[i] == c.bang) {
                if (!cc || hx_in(cc, HX_NOEXP)) continue;
                if (dq && cc == '"') continue;
                if (hx_inhibited(s, i, c.bang, qs)) continue;
                found = true;
                break;
            } else if (dq && s[i] == '\\' && cc == '"') {
                i++;
            } else if (s[i] == '"') {
                dq = !dq;
            } else if (!dq && s[i] == '\'') {
                bool ansi = i > 0 && s[i - 1] == '$';
                i++;
                while (i < n && s[i] != '\'') {
                    if (ansi && s[i] == '\\' && i + 1 < n) i++;
                    i++;
                }
            } else if (s[i] == '\\') {
                if (cc == '\'' || cc == c.bang) i++;
            }
        }
        if (!found) return 0;
    }

    SH_SB(res);
    bool dq = qs == '"';
    size_t n = strlen(s), i = 0;
    int modified = 0, only_print = 0;
    if (qs == '\'') {
        while (i < n && s[i] != '\'') i++;
        if (sb_putmem(&res, s, i < n ? i + 1 : i) < 0) return -1;
        if (i < n) i++;
    }
    bool passc = false;
    for (; i < n; i++) {
        char ch = s[i];
        if (passc) {
            passc = false;
            if (sb_putc(&res, ch) < 0) return -1;
            continue;
        }
        size_t cl = hx_clen(s, i);
        if (cl > 1) {
            if (sb_putmem(&res, s + i, cl) < 0) return -1;
            i += cl - 1;
            continue;
        }
        if (ch == c.bang) {
            char cc = s[i + 1];
            if (!cc || hx_in(cc, HX_NOEXP) || (dq && cc == '"')) {
                if (sb_putc(&res, ch) < 0) return -1;
                continue;
            }
            /* asked of what is expanded so far with this `!` after it, as
             * bash does */
            size_t save = res.len;
            if (sb_putc(&res, ch) < 0 || sb_putc(&res, cc) < 0) return -1;
            bool inh = hx_inhibited(res.p, save, c.bang, qs);
            sb_truncate(&res, save);
            if (inh) {
                if (sb_putc(&res, ch) < 0) return -1;
                continue;
            }
            size_t e = i;
            SH_SB(one);
            int r = hx_one(s, i, dq ? '"' : 0, &e, &one, err,
                           res.p ? res.p : "", &c);
            if (r < 0) return -1;
            modified++;
            if (one.len && sb_putmem(&res, one.p, one.len) < 0) return -1;
            only_print += r == 1;
            i = e;
            continue;
        }
        if (c.cmt && ch == c.cmt) {
            if (!dq && (i == 0 || hx_in(s[i - 1], HX_WDELIM))) {
                if (sb_puts(&res, s + i) < 0) return -1;
                break;
            }
            if (sb_putc(&res, ch) < 0) return -1;
            continue;
        }
        switch (ch) {
        case '\\':
            passc = true;
            if (sb_putc(&res, ch) < 0) return -1;
            break;
        case '"':
            dq = !dq;
            if (sb_putc(&res, ch) < 0) return -1;
            break;
        case '\'':
            if (!dq) {
                bool ansi = i > 0 && s[i - 1] == '$';
                size_t q = i++;
                while (i < n && s[i] != '\'') {
                    if (ansi && s[i] == '\\' && i + 1 < n) i++;
                    i++;
                }
                if (sb_putmem(&res, s + q, (i < n ? i + 1 : n) - q) < 0) return -1;
            } else if (sb_putc(&res, ch) < 0) {
                return -1;
            }
            break;
        default:
            if (sb_putc(&res, ch) < 0) return -1;
            break;
        }
    }
    if (sb_set(out, res.p ? res.p : "") < 0) return -1;
    if (only_print) return 2;
    return modified ? 1 : 0;
}

/* The quote left open at the end of TEXT -- the line a continuation
 * follows: `'`, `"` or 0 (bash's history_quoting_state). */
static char shell_hist_open_quote(const char *text) {
    bool sq = false, dq = false, ansi = false;
    for (const char *p = text; *p; p++) {
        if (sq) {
            if (ansi && *p == '\\' && p[1]) { p++; continue; }
            if (*p == '\'') sq = false;
            continue;
        }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (dq) {
            if (*p == '"') dq = false;
            continue;
        }
        if (*p == '\'') { sq = true; ansi = p > text && p[-1] == '$'; continue; }
        if (*p == '"') dq = true;
    }
    return sq ? '\'' : dq ? '"' : 0;
}

/* Is history expansion on for the next line read: history being recorded
 * and histexpand set. */
static bool shell_hist_expanding(void) {
    return g_opt_history && g_opt_histexpand;
}

/* Write S and a newline, all of it, to the shell's standard error -- the
 * expanded line bash echoes. */
static void shell_hist_echo(const char *s) {
    size_t n = strlen(s);
    for (size_t off = 0; off < n; off += 512) {
        size_t k = n - off < 512 ? n - off : 512;
        kprintf_raw("%.*s", (int)k, s + off);
    }
    kprintf_raw("\n");
}

/* ONE LINE AS bash's pre_process_line TAKES IT: expanded into OUT and the
 * expansion echoed; QS as shell_hist_expand. 1: OUT holds the line to use;
 * 0: use LINE as it is; -1: drop the line -- an error, reported, or `:p`,
 * echoed and recorded. */
static int shell_hist_preprocess(const char *line, char qs, struct shell_sb *out) {
    SH_SB(err);
    int r = shell_hist_expand(line, qs, out, &err);
    if (r == 0) return 0;
    if (r < 0) {
        kprintf("%s\n", err.p ? err.p : "history expansion failed");
        return -1;
    }
    shell_hist_echo(out->p ? out->p : "");
    if (r == 2) {
        if (out->p && *out->p) shell_history_add(out->p);
        return -1;
    }
    return 1;
}

int shell_line_incomplete_text(const char *s);

/* A SCRIPT's command, read as bash reads it with expansion on: its lines,
 * from SRC, each expanded in the quote the one before left open, and
 * echoed, into OUT until one completes the command (the REPL's test).
 * *USED is the bytes of SRC they were, their newlines included. A line
 * whose expansion fails is reported and DROPPED, newline and all: bash
 * reads the next one under the same line number, so it is not counted and
 * the lines after it are numbered one less (logs/b13 q29 case 11). An
 * empty OUT is a command that was all dropped lines. A HERE-DOCUMENT'S
 * BODY IS NEVER EXPANDED: the lines after the command -- or, for one that
 * a line of a command still open starts, `{ cat <<EOF` or `$( cat <<EOF`,
 * the unit's next lines through its delimiter's, which tsh expanded, and
 * a `!!` there pulled in the whole last command (bash's histexp7.sub;
 * logs/b42 hx1 hx2). CODE is the unit without those, for the quote a
 * line starts in. */
static void shell_hist_unit(const char *src, struct shell_sb *out, size_t *used) {
    size_t pos = 0;
    sb_truncate(out, 0);
    SH_SB(code);
    struct shell_heredoc_spec hd[8];
    int nhd = 0, hdi = 0;
    unsigned long ln0 = g_shell_lineno;
    g_shell_lineno++;                   /* the line being read, for a report */
    for (;;) {
        size_t ln = 0;
        while (src[pos + ln] && src[pos + ln] != '\n') ln++;
        bool nl = src[pos + ln] == '\n';
        SH_SB(phys);
        if (sb_putmem(&phys, src + pos, ln) < 0 || sb_puts(&phys, "") < 0) break;
        if (hdi < nhd) {
            /* a body line, as it is */
            pos += ln + (nl ? 1 : 0);
            if (sb_puts(out, phys.p) < 0) break;
            if (shell_heredoc_line_matches(phys.p, hd[hdi].delim, hd[hdi].strip_tabs) &&
                ++hdi == nhd) {
                shell_heredoc_specs_free(hd, nhd);
                nhd = hdi = 0;
            }
            g_shell_lineno++;
            bool more = nl && src[pos] &&
                        (hdi < nhd || shell_line_incomplete_text(out->p));
            if (nl && sb_putc(out, '\n') < 0) break;
            if (!more) break;
            continue;
        }
        char qs = code.len ? shell_hist_open_quote(code.p) : 0;
        SH_SB(xl);
        int r = shell_hist_preprocess(phys.p, qs, &xl);
        pos += ln + (nl ? 1 : 0);
        if (r < 0) {
            if (!out->len || !nl || !src[pos]) break;
            continue;                   /* the command goes on without it */
        }
        const char *use = r > 0 ? (xl.p ? xl.p : "") : phys.p;
        if (sb_puts(out, use) < 0 || sb_puts(&code, use) < 0) break;
        g_shell_lineno++;
        bool more = nl && src[pos] && shell_line_incomplete_text(out->p);
        if (more && !qs) {
            nhd = shell_line_find_heredocs(use, hd, 8);
            /* ...and one in a `$(` the line leaves open, which that
             * finder reads only when the `)` comes */
            const char *open = 0;
            for (const char *s = use; nhd == 0 && *s; s++)
                if (s[0] == '$' && s[1] == '(' && s[2] != '(') open = s + 2;
            if (nhd == 0 && open) nhd = shell_line_find_heredocs(open, hd, 8);
        }
        if (nl && (sb_putc(out, '\n') < 0 || sb_putc(&code, '\n') < 0)) break;
        if (!more) break;
    }
    shell_heredoc_specs_free(hd, nhd);
    g_shell_lineno = ln0;
    *used = pos;
}

#ifdef SHELL_HOSTED
/* /bin/tsh's REPL reads a line at a time, as bash does, and expands each as
 * it comes (shell_hist_preprocess). ACCUM is the command read so far, for
 * the quote this line continues. *RC: 0 use LINE, 1 use the answer, -1 the
 * line is dropped. The answer lives until the next call. */
bool shell_hist_active_hosted(void) { return shell_hist_expanding(); }

const char *shell_hist_line_hosted(const char *line, const char *accum, int *rc) {
    static struct shell_sb keep;
    static bool keep_init;
    if (!keep_init) { sb_init(&keep); keep_init = true; }
    char qs = accum && *accum ? shell_hist_open_quote(accum) : 0;
    *rc = shell_hist_preprocess(line, qs, &keep);
    return keep.p ? keep.p : "";
}
#endif
