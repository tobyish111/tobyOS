/* shell_xtrace.inc -- `set -x`, as bash 5.2 prints it (batch 13).
 *
 * tsh printed "+ " and a pipeline's words as they stood, joined by "| ".
 * bash's trace, measured line by line (logs/b13 q30 q34, guest bash):
 *
 *   - Every line starts with $PS4 -- expanded, with -x off while it is --
 *     and its first CHARACTER once more for each level of nesting: `+ ` in
 *     a script, at the prompt or under -c, `++ ` in an eval, a sourced
 *     file, a command or process substitution or a trap action, `+++ `
 *     one deeper (bash's indirection_level_string). PS4 empty or unset is
 *     no prefix at all.
 *   - A simple command prints its assignments one to a line, then its
 *     words. A word is quoted the way input would need it: '' when empty,
 *     '...' when it holds a shell metacharacter, $'...' when it holds a
 *     character the locale cannot print (sh_contains_shell_metas, then
 *     ansic_shouldquote). An array literal prints as written, re-spaced.
 *     Each stage of a pipeline is a line of its own.
 *   - for, select and case print their heads as written; `for (( ))` and
 *     `(( ))` print the expression; `[[ ]]` prints each test it evaluates.
 *   - The lines go to $BASH_XTRACEFD when it names an open descriptor, and
 *     to standard error otherwise.
 *
 * Included by shell.c after the quoting helpers. */

/* sh_single_quote: a lone quote is \', anything else '...' with each quote
 * written '\'' */
static int shell_xt_single_quote(struct shell_sb *o, const char *t) {
    if (t[0] == '\'' && !t[1]) return sb_puts(o, "\\'");
    if (sb_putc(o, '\'') < 0) return -1;
    for (const char *p = t; *p; p++) {
        if (sb_putc(o, *p) < 0) return -1;
        if (*p == '\'' && sb_puts(o, "\\''") < 0) return -1;
    }
    return sb_putc(o, '\'');
}

/* A word as the trace shows it. */
static int shell_xt_word(struct shell_sb *o, const char *t) {
    if (!t || !*t) return sb_puts(o, "''");
    if (shell_has_shell_metas(t)) return shell_xt_single_quote(o, t);
    if (shell_q_needs_ansi(t)) return shell_quote_ansic(t, o);
    return sb_puts(o, t);
}

/* $PS4 expanded, its first character repeated for the level. */
static void shell_xt_prefix(struct shell_sb *out) {
    const char *ps4 = env_get("PS4");
    if (!ps4 || !*ps4) return;
    SH_SB(d);
    bool sx = g_opt_xtrace;
    g_opt_xtrace = false;
    int rc = shell_expand_prompt(ps4, &d);
    g_opt_xtrace = sx;
    if (rc < 0 || !d.p || !*d.p) return;
    shell_strip_nosplit_inplace(d.p);
    size_t dl = strlen(d.p);
    if (!dl) return;
    size_t fl = shell_utf8() ? sh_cl(d.p, d.p + dl, true) : 1;
    int lvl = g_xtrace_level > 0 ? g_xtrace_level : 1;
    for (int i = 0; i < lvl; i++)
        if (sb_putmem(out, d.p, fl) < 0) return;
    (void)sb_puts(out, d.p + fl);
}

/* Write a finished line where bash's trace goes. */
static void shell_xt_emit(const struct shell_sb *line) {
    if (!line->p || !line->len) return;
    struct file *f = 0;
    const char *fdv = env_get("BASH_XTRACEFD");
    if (fdv && *fdv) {
        long fd = 0;
        const char *q = fdv;
        while (*q >= '0' && *q <= '9' && fd < SHELL_FD_LIMIT) fd = fd * 10 + (*q++ - '0');
        if (!*q && fd < SHELL_FD_LIMIT) f = shell_builtin_fd((int)fd);
    }
#ifdef SHELL_HOSTED
    if (!f) {
        extern struct file *shell_current_diag_file(void);
        f = shell_current_diag_file();
    }
#endif
    if (f) {
        (void)file_write(f, line->p, line->len);
        return;
    }
    /* the console: in pieces, as kprintf formats into a bounded buffer */
    for (size_t off = 0; off < line->len; off += 512) {
        size_t k = line->len - off < 512 ? line->len - off : 512;
        kprintf_raw("%.*s", (int)k, line->p + off);
    }
}

/* + NAME=VALUE: bash's xtrace_print_assignment. NAME carries its subscript
 * and a `+` for `+=`; LIST says VALUE is an array literal's inside. */
static int shell_xt_assign_fmt(struct shell_sb *l, const char *name, size_t nlen,
                               const char *value, bool list) {
    shell_xt_prefix(l);
    if (sb_putmem(l, name, nlen) < 0 || sb_putc(l, '=') < 0) return -1;
    int rc;
    if (list)
        rc = (sb_putc(l, '(') < 0 || sb_puts(l, value) < 0 ||
              sb_putc(l, ')') < 0) ? -1 : 0;
    else if (!*value)
        rc = 0;
    else if (shell_has_shell_metas(value))
        rc = shell_xt_single_quote(l, value);
    else if (shell_q_needs_ansi(value))
        rc = shell_quote_ansic(value, l);
    else
        rc = sb_puts(l, value);
    if (rc < 0 || sb_putc(l, '\n') < 0) return -1;
    return 0;
}

static void shell_xt_assign(const char *name, size_t nlen, const char *value,
                            bool list) {
    SH_SB(l);
    if (shell_xt_assign_fmt(&l, name, nlen, value, list) == 0) shell_xt_emit(&l);
}

/* The words of TEXT as written, one blank between each: what the parser
 * keeps of a for list or an array literal. Quotes and substitutions stay
 * whole; blanks, newlines and `;` separate; a `#` starting a word starts
 * a comment that runs to the end of the line. */
static int shell_xt_rawwords(const char *s, const char *e, struct shell_sb *out) {
    bool first = true;
    while (s < e) {
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\n' || *s == ';')) s++;
        if (s >= e) break;
        if (*s == '#') {
            while (s < e && *s != '\n') s++;
            continue;
        }
        const char *w = s;
        int depth = 0;
        while (s < e) {
            char c = *s;
            if (depth == 0 && (c == ' ' || c == '\t' || c == '\n' || c == ';')) break;
            if (c == '\\' && s + 1 < e) { s += 2; continue; }
            if (c == '\'') {
                bool ansi = s > w && s[-1] == '$';
                s++;
                while (s < e && *s != '\'') {
                    if (ansi && *s == '\\' && s + 1 < e) s++;
                    s++;
                }
                if (s < e) s++;
                continue;
            }
            if (c == '"') {
                s++;
                while (s < e && *s != '"') {
                    if (*s == '\\' && s + 1 < e) s++;
                    s++;
                }
                if (s < e) s++;
                continue;
            }
            if (c == '`') {
                s++;
                while (s < e && *s != '`') {
                    if (*s == '\\' && s + 1 < e) s++;
                    s++;
                }
                if (s < e) s++;
                continue;
            }
            if (c == '(' || c == '{' || c == '[') depth++;
            else if ((c == ')' || c == '}' || c == ']') && depth > 0) depth--;
            s++;
        }
        if (!first && sb_putc(out, ' ') < 0) return -1;
        if (sb_putmem(out, w, (size_t)(s - w)) < 0) return -1;
        first = false;
    }
    return 0;
}

/* + HEAD TEXT: a compound command's head, its words as written -- `for i
 * in $l`, `case $v in`, `select x in a b`. */
static void shell_xt_head(const char *head, const char *ws, const char *we,
                          const char *tail) {
    SH_SB(l);
    shell_xt_prefix(&l);
    if (sb_puts(&l, head) < 0) return;
    if (ws && shell_xt_rawwords(ws, we, &l) < 0) return;
    if (tail && sb_puts(&l, tail) < 0) return;
    if (sb_putc(&l, '\n') < 0) return;
    shell_xt_emit(&l);
}

/* + (( EXPR )): `(( ))` and the three parts of `for (( ))`, expanded and
 * printed as they are (xtrace_print_arith_cmd). */
static void shell_xt_arith(const char *expr) {
    SH_SB(l);
    shell_xt_prefix(&l);
    if (sb_puts(&l, "(( ") < 0 || sb_puts(&l, expr) < 0 ||
        sb_puts(&l, " ))\n") < 0)
        return;
    shell_xt_emit(&l);
}

/* + [[ ... ]]: one test of a conditional, as it is evaluated
 * (xtrace_print_cond_term). OP alone with A1 is a unary test; A2 makes it
 * binary. An empty operand is ''. Operands are not quoted -- a pattern
 * arrives with its quoted characters backslashed (shell_dbr_val). */
static void shell_xt_cond(bool inv, const char *op, const char *a1, const char *a2,
                          bool binary) {
    SH_SB(l);
    shell_xt_prefix(&l);
    if (sb_puts(&l, "[[ ") < 0) return;
    if (inv && sb_puts(&l, "! ") < 0) return;
    if (!binary) {
        if (sb_puts(&l, op) < 0 || sb_putc(&l, ' ') < 0 ||
            sb_puts(&l, a1 && *a1 ? a1 : "''") < 0)
            return;
    } else {
        if (sb_puts(&l, a1 && *a1 ? a1 : "''") < 0 || sb_putc(&l, ' ') < 0 ||
            sb_puts(&l, op) < 0 || sb_putc(&l, ' ') < 0 ||
            sb_puts(&l, a2 && *a2 ? a2 : "''") < 0)
            return;
    }
    if (sb_puts(&l, " ]]\n") < 0) return;
    shell_xt_emit(&l);
}

/* + W1 W2 ...: ARGV's words from FROM on, quoted. */
static void shell_xt_words(char **argv, int from, int argc) {
    if (from >= argc) return;
    SH_SB(l);
    shell_xt_prefix(&l);
    for (int a = from; a < argc; a++) {
        if (a > from && sb_putc(&l, ' ') < 0) return;
        if (shell_xt_word(&l, argv[a]) < 0) return;
    }
    if (sb_putc(&l, '\n') < 0) return;
    shell_xt_emit(&l);
}

static bool shell_simple_noassign(const struct shell_simple *s, int i);
static size_t shell_word_assign_eq(const char *w);
static bool shell_is_assignment_word(const char *s);

/* DECLARATION BUILTINS (declare typeset local export readonly) trace as
 * bash's do, which is not as written (logs/b13 q34):
 *
 *     local -a la=(1 2)       + la=('1' '2')       the literal assigned on
 *                             + local -a la        its own, elements quoted
 *     export e=3 f            + export e=3 f       ...and export/readonly
 *                             + e=3                 echo what they assign
 *
 * bash assigns a compound argument while it expands the command
 * (expand_declaration_argument) and runs the builtin with the name alone;
 * export and readonly assign through do_assignment_no_expand, which
 * traces. tsh's builtin does the assigning, so the command's line WAITS
 * (g_xtd) while it runs -- the literal's line is made as its elements
 * are expanded (sh_array_assign_literal) -- and goes out after, in bash's
 * order. */
static bool shell_xt_is_decl(const char *w) {
    return w && (strcmp(w, "declare") == 0 || strcmp(w, "typeset") == 0 ||
                 strcmp(w, "local") == 0 || strcmp(w, "export") == 0 ||
                 strcmp(w, "readonly") == 0);
}

/* Word K of CMD as NAME=(...) or NAME+=(...): NAME's length, else 0. */
static size_t shell_xt_decl_compound(const struct shell_simple *cmd, int k) {
    const char *w = cmd->argv[k];
    size_t eq = shell_word_assign_eq(w);
    if (eq == (size_t)-1) return 0;
    const char *v = w + eq + 1;
    size_t vl = strlen(v);
    if (vl < 2 || v[0] != '(' || v[vl - 1] != ')') return 0;
    for (int i = 0; i < cmd->nscalar_paren; i++)
        if (cmd->scalar_paren[i] == w) return 0;
    size_t n = eq;
    if (n && w[n - 1] == '+') n--;
    return n;
}

static void shell_xt_decl_flush(void) {
    if (!g_xtd.pending) return;
    g_xtd.pending = false;
    shell_xt_emit(&g_xtd.pre);
    shell_xt_emit(&g_xtd.cmd);
    shell_xt_emit(&g_xtd.post);
    sb_free(&g_xtd.pre);
    sb_free(&g_xtd.cmd);
    sb_free(&g_xtd.post);
}

/* A compound argument, assigned: + NAME=('e1' ['k']='v') -- ELEMS is the
 * elements, quoted (sh_xt_lit_note). */
static void shell_xt_decl_literal(const char *name, bool append, const char *elems) {
    shell_xt_prefix(&g_xtd.pre);
    (void)(sb_puts(&g_xtd.pre, name) < 0 ||
           (append && sb_putc(&g_xtd.pre, '+') < 0) ||
           sb_puts(&g_xtd.pre, "=(") < 0 || sb_puts(&g_xtd.pre, elems) < 0 ||
           sb_puts(&g_xtd.pre, ")\n") < 0);
}

/* export's and readonly's NAME=VALUE (WORD, its `=` at KLEN), as they
 * assign it. A compound one is traced as its literal is assigned. */
static void shell_xt_decl_assign(const char *word, size_t klen) {
    if (!g_opt_xtrace || word[klen] != '=') return;
    const char *v = word + klen + 1;
    size_t vl = strlen(v);
    if (vl >= 2 && v[0] == '(' && v[vl - 1] == ')' && !shell_kv_scalar_paren(word, v))
        return;
    if (g_xtd.pending) {
        (void)shell_xt_assign_fmt(&g_xtd.post, word, klen, v, false);
        return;
    }
    shell_xt_assign(word, klen, v, false);
}

/* A simple command: its assignments a line each -- an array literal as
 * written, re-spaced -- then its words. */
static void shell_xt_simple(const struct shell_simple *cmd) {
    int assignc = 0;
    while (assignc < cmd->argc && !shell_simple_noassign(cmd, assignc) &&
           shell_is_assignment_word(cmd->argv[assignc]))
        assignc++;
    for (int k = 0; k < assignc; k++) {
        const char *w = cmd->argv[k];
        size_t eq = shell_word_assign_eq(w);
        if (eq == (size_t)-1) continue;
        const char *v = w + eq + 1;
        size_t vl = strlen(v);
        bool list = vl >= 2 && v[0] == '(' && v[vl - 1] == ')';
        for (int i = 0; list && i < cmd->nscalar_paren; i++)
            if (cmd->scalar_paren[i] == w) list = false;
        if (list) {
            SH_SB(inner);
            if (shell_xt_rawwords(v + 1, v + vl - 1, &inner) < 0) continue;
            shell_xt_assign(w, eq, inner.p ? inner.p : "", true);
        } else {
            shell_xt_assign(w, eq, v, false);
        }
    }
    if (assignc < cmd->argc && shell_xt_is_decl(cmd->argv[assignc])) {
        bool any = false;
        for (int k = assignc + 1; k < cmd->argc && !any; k++)
            any = shell_xt_decl_compound(cmd, k) > 0;
        if (any) {
            sb_free(&g_xtd.pre);
            sb_free(&g_xtd.cmd);
            sb_free(&g_xtd.post);
            shell_xt_prefix(&g_xtd.cmd);
            for (int k = assignc; k < cmd->argc; k++) {
                if (k > assignc && sb_putc(&g_xtd.cmd, ' ') < 0) return;
                size_t n = k > assignc ? shell_xt_decl_compound(cmd, k) : 0;
                SH_SB(nm);
                if (n && sb_putmem(&nm, cmd->argv[k], n) < 0) return;
                if (shell_xt_word(&g_xtd.cmd, n ? nm.p : cmd->argv[k]) < 0) return;
            }
            if (sb_putc(&g_xtd.cmd, '\n') < 0) return;
            g_xtd.pending = true;
            g_xtd.level = g_xtrace_level;
            return;
        }
    }
    shell_xt_words(cmd->argv, assignc, cmd->argc);
}
