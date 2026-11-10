/* shell_help.inc -- `help`, and every builtin's `--help`, as bash 5.2.15's.
 *
 * Included from shell.c after shell_complete.inc, whose g_bash_topics
 * (generated, shell_bashhelp.inc) holds bash's table: each topic's name,
 * synopsis and long doc, in bash's order. The text is bash's own (the
 * user's decision for Oils 0644); the code here is tsh's, written from
 * bash's builtins/help.def, common.h and bashgetopt.c as the spec.
 *
 * What bash does, measured on the guest's bash (logs/b27):
 *   - `help` alone: "GNU bash, version V (MACHTYPE)", its paragraph, then
 *     every topic's synopsis in two columns of half the screen width each
 *     ($COLUMNS, else the terminal's when checkwinsize is on, else 80),
 *     cut with a `>`; a `*` marks a disabled builtin. How a column is cut
 *     depends on whether the locale is multibyte: bash has two routines
 *     (dispcolumn, wdispcolumn) that cut differently at the edges.
 *   - `help PATTERN...`: each topic whose name the pattern matches, or,
 *     when none does, each it is a prefix of; "NAME: SYNOPSIS" and the long
 *     doc after four spaces. -s the synopsis only, -d "NAME - " and the
 *     doc's first line, -m a manual page. A pattern that is a glob first
 *     says "Shell commands matching keyword `...'". Only when nothing at
 *     all matched: "no help topics match", naming the LAST pattern.
 *   - `NAME --help`: "NAME: SYNOPSIS" and the long doc, status 2 -- where
 *     the builtin's option reading meets `--help` (shell_builtin_wants_help).
 *
 * The header keeps tsh's MACHTYPE, x86_64-pc-tobyos, as $MACHTYPE does (the
 * user's decision), so `help` alone differs from bash's in its first line. */

#ifdef SHELL_HOSTED
extern int host_winsize(int fd, int *rows, int *cols);     /* host.c */
extern int host_file_fd(struct file *f);
#endif

/* bash's atoi: blanks, a sign, the digits */
static long help_atoi(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    bool neg = *s == '-';
    if (*s == '-' || *s == '+') s++;
    long v = 0;
    for (; *s >= '0' && *s <= '9' && v < 1000000; s++) v = v * 10 + (*s - '0');
    return neg ? -v : v;
}

/* default_columns (general.c): $COLUMNS if it is a positive number, else
 * -- when checkwinsize is on -- the terminal's width, which also sets LINES
 * and COLUMNS (sh_set_lines_and_columns), else 80. The terminal is bash's
 * input_tty: the shell's terminal when it has one (an interactive shell),
 * otherwise whatever fd 2 is now. */
static int shell_default_columns(void) {
    const char *v = env_get("COLUMNS");
    if (v && *v) {
        long c = help_atoi(v);
        if (c > 0) return c > 100000 ? 100000 : (int)c;
    }
#ifdef SHELL_HOSTED
    if (g_shopt_checkwinsize) {
        int rows = 0, cols = 0;
        int fd = g_interactive ? -2 : host_file_fd(g_shell_fd[2]);
        if (host_winsize(fd, &rows, &cols) == 0 && rows > 0 && cols > 0) {
            char b[16];
            ksnprintf(b, sizeof b, "%d", rows);
            (void)env_set("LINES", b);
            ksnprintf(b, sizeof b, "%d", cols);
            (void)env_set("COLUMNS", b);
            return cols;
        }
    }
#endif
    return 80;
}

/* MB_CUR_MAX > 1, as the guest's C library decides it: the locale is the
 * first of LC_ALL, LC_CTYPE and LANG that is set and not empty; "C" and
 * "POSIX" are single-byte, any other name is UTF-8, and with none of them
 * musl's default is C.UTF-8. An assignment in the script counts, as bash
 * calls setlocale for it (measured: logs/b27/locale-hw.out). */
static bool shell_locale_multibyte(void) {
    static const char *const vars[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (int i = 0; i < 3; i++) {
        const char *v = env_get(vars[i]);
        if (v && *v) return strcmp(v, "C") != 0 && strcmp(v, "POSIX") != 0;
    }
    return true;
}

/* glob_pattern_p (lib/glob/glob_loop.c): `*` or `?`, a `[` with a `]`
 * after it, or `+(`, `@(`, `!(`; a backslash hides the next character. */
static bool help_glob_pattern_p(const char *p) {
    int bopen = 0;
    for (char c; (c = *p++) != '\0'; ) {
        switch (c) {
        case '?': case '*': return true;
        case '[': bopen++; break;
        case ']': if (bopen) return true; break;
        case '+': case '@': case '!': if (*p == '(') return true; break;
        case '\\': if (*p == '\0') return false; p++; break;
        default: break;
        }
    }
    return false;
}

/* strmatch(PATTERN, NAME, FNMATCH_EXTFLAG): an extglob group counts only
 * with extglob on -- off, `@(echo|true)` is those twelve characters. tsh's
 * matcher reads groups always (its parser is what extglob gates), so with
 * it off every `(` after + @ ! * ? is marked literal first. */
static bool help_match(const char *pat, const char *name) {
    if (g_shopt_extglob) return shell_glob_match(pat, name);
    SH_SB(b);
    for (const char *p = pat; *p; p++) {
        if (*p == '(' && p > pat && comp_strchr("+@!*?", p[-1]) &&
            sb_putc(&b, SHELL_GLOB_ESC) < 0)
            return false;
        if (sb_putc(&b, *p) < 0) return false;
    }
    return shell_glob_match(b.p ? b.p : "", name);
}

static void help_puts_n(const char *s, size_t n) {
    char b[160];
    while (n > 0) {
        size_t k = n < sizeof b - 1 ? n : sizeof b - 1;
        memcpy(b, s, k);
        b[k] = '\0';
        shell_write(b);
        s += k;
        n -= k;
    }
}

static void help_spaces(int n) {
    while (n-- > 0) shell_write(" ");
}

static char help_flag(const struct sh_bash_topic *t) {
    return t->bi && shell_builtin_disabled(t->name) ? '*' : ' ';
}

/* C's strncpy: N bytes of SRC into DST, NUL-padded, no terminator past N */
static void help_strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
}

/* dispcolumn: the single-byte cut. strncpy into a buffer WIDTH long and a
 * `>` at a fixed place -- so a synopsis that just fits gets one too. */
static void help_dispcolumn(int i, int width, int height, int n) {
    char buf[136];
    const struct sh_bash_topic *t = &g_bash_topics[i];
    memset(buf, 0, sizeof buf);
    buf[0] = help_flag(t);
    help_strncpy(buf + 1, t->syn, (size_t)(width - 2));
    buf[width - 2] = '>';
    buf[width - 1] = '\0';
    shell_write(buf);
    if ((i << 1) >= n || i + height >= n) { shell_write("\n"); return; }
    help_spaces(width - (int)strlen(buf));
    t = &g_bash_topics[i + height];
    memset(buf, 0, sizeof buf);
    buf[0] = help_flag(t);
    help_strncpy(buf + 1, t->syn, (size_t)(width - 3));
    buf[width - 3] = '>';
    buf[width - 2] = '\0';
    shell_write(buf);
    shell_write("\n");
}

/* wcsnwidth (lib/sh/wcsnwidth.c) for this text: every character is one
 * column wide (the synopses are printable ASCII). */
static int help_wcsnwidth(const char *s, size_t n, int max) {
    int len = 0;
    const char *ws = s;
    while (n-- > 0 && *ws++ != '\0') {
        if (1 == max - len) return (int)(ws - s);
        if (1 > max - len) return (int)(--ws - s);
        len++;
    }
    return (int)(ws - s);
}

/* one column the multibyte way: WHICH 0 the first, 1 the second */
static int help_wcolumn(char *w, const struct sh_bash_topic *t, int width, int which) {
    size_t len = strlen(t->syn);
    size_t slen = len >= (size_t)width ? (size_t)(width - 2) : len;
    size_t cn = len < slen + 1 ? len : slen + 1;   /* mbstowcs's count */
    memcpy(w + 1, t->syn, cn);
    w[cn + 1] = '\0';
    for (size_t j = 1; j < cn; j++)
        if (w[j] == '\n' || w[j] == '\t') w[j] = ' ';
    int dispchars = help_wcsnwidth(w + 1, slen, width - 2);
    int dispcols = dispchars + 1;
    w[0] = help_flag(t);
    if (dispcols >= width - 2) {
        if (which == 0) { w[dispchars] = '>'; w[dispchars + 1] = '\0'; }
        else { w[dispchars - 1] = '>'; w[dispchars] = '\0'; }
    }
    return dispcols;
}

/* wdispcolumn: the cut a multibyte locale gets */
static void help_wdispcolumn(int i, int width, int height, int n) {
    char w[136];
    int dispcols = help_wcolumn(w, &g_bash_topics[i], width, 0);
    shell_write(w);
    if ((i << 1) >= n || i + height >= n) { shell_write("\n"); return; }
    help_spaces(width - dispcols);
    (void)help_wcolumn(w, &g_bash_topics[i + height], width, 1);
    shell_write(w);
    shell_write("\n");
}

static void help_show_version(void) {
    shell_write("GNU bash, version 5.2.15(1)-release (x86_64-pc-tobyos)\n");
}

/* show_builtin_command_help */
static void help_show_all(void) {
    shell_write(
"These shell commands are defined internally.  Type `help' to see this list.\n"
"Type `help name' to find out more about the function `name'.\n"
"Use `info bash' to find out more about the shell in general.\n"
"Use `man -k' or `info' to find out more about commands not in this list.\n"
"\n"
"A star (*) next to a name means that the command is disabled.\n"
"\n");
    int width = shell_default_columns() / 2;
    if (width > 128) width = 128;
    if (width <= 3) width = 40;
    int n = SH_BASH_TOPICS, height = (n + 1) / 2;
    bool mb = shell_locale_multibyte();
    for (int i = 0; i < height; i++) {
        if (mb) help_wdispcolumn(i, width, height, n);
        else help_dispcolumn(i, width, height, n);
    }
#ifndef SHELL_HOSTED
    /* the kernel shell's own commands, which bash has not */
    bool first = true;
    for (const struct cmd *c = cmds; c->name; c++) {
        bool bash_has = false;
        for (int k = 0; g_bash_topics[k].name; k++)
            if (g_bash_topics[k].bi && strcmp(g_bash_topics[k].name, c->name) == 0)
                bash_has = true;
        if (bash_has) continue;
        if (first) shell_write("\nThe tobyOS kernel shell's own commands:\n");
        first = false;
        shell_printf("  %-8s  %s\n", c->name, c->help);
    }
#endif
}

/* show_longdoc: the doc after BASE_INDENT (4) spaces */
static void help_show_longdoc(const struct sh_bash_topic *t) {
    shell_write("    ");
    shell_write(t->doc);
    shell_write("\n");
}

/* show_desc: "NAME - " and the doc's first line, its newline included */
static void help_show_desc(const struct sh_bash_topic *t) {
    shell_write(t->name);
    shell_write(" - ");
    const char *nl = comp_strchr(t->doc, '\n');
    help_puts_n(t->doc, nl ? (size_t)(nl - t->doc) + 1 : strlen(t->doc));
}

/* show_manpage */
static void help_show_manpage(const struct sh_bash_topic *t) {
    shell_write("NAME\n    ");
    shell_write(t->name);
    shell_write(" - ");
    const char *nl = comp_strchr(t->doc, '\n');
    help_puts_n(t->doc, nl ? (size_t)(nl - t->doc) + 1 : strlen(t->doc));
    shell_write("\nSYNOPSIS\n    ");
    shell_write(t->syn);
    shell_write("\n\nDESCRIPTION\n");
    help_show_longdoc(t);
    shell_write("\nSEE ALSO\n    bash(1)\n\nIMPLEMENTATION\n    ");
    help_show_version();
    shell_write("    Copyright (C) 2022 Free Software Foundation, Inc.\n"
                "    License GPLv3+: GNU GPL version 3 or later "
                "<http://gnu.org/licenses/gpl.html>\n\n");
}

static void cmd_help(int argc, char **argv) {
    shell_set_status(0);
    struct sh_igo g;
    sh_igo_init(&g, "help", argc, argv);
    bool dflag = false, mflag = false, sflag = false;
    int c;
    while ((c = sh_igo_next(&g, "dms")) != -1) {
        switch (c) {
        case 'd': dflag = true; break;
        case 'm': mflag = true; break;
        case 's': sflag = true; break;
        case SH_IGO_HELP:
            shell_help_topic("help");
            shell_set_status(2);
            return;
        default:
            sh_builtin_usage("help");
            shell_set_status(2);
            return;
        }
    }
    int first = g.ind;
    if (first >= argc) {
        help_show_version();
        help_show_all();
        return;
    }
    if (help_glob_pattern_p(argv[first])) {
        shell_write(first + 1 < argc ? "Shell commands matching keywords `"
                                     : "Shell commands matching keyword `");
        for (int i = first; i < argc; i++) {
            if (i > first) shell_write(", ");
            shell_write(argv[i]);
        }
        shell_write("'\n\n");
    }
    int found = 0;
    const char *pattern = "";
    for (int i = first; i < argc; i++) {
        pattern = argv[i];
        size_t plen = strlen(pattern);
        bool this_found = false;
        /* pass 1: the name, or a pattern that matches it; pass 2, only
         * when pass 1 found nothing: a prefix of it (bash-4.2's rule) */
        for (int pass = 1; pass < 3; pass++) {
            for (int k = 0; g_bash_topics[k].name; k++) {
                const struct sh_bash_topic *t = &g_bash_topics[k];
                bool m = pass == 1 ? (strcmp(pattern, t->name) == 0 ||
                                      help_match(pattern, t->name))
                                   : strncmp(pattern, t->name, plen) == 0;
                if (!m) continue;
                this_found = true;
                found++;
                if (dflag) { help_show_desc(t); continue; }
                if (mflag) { help_show_manpage(t); continue; }
                shell_write(t->name);
                shell_write(": ");
                shell_write(t->syn);
                shell_write("\n");
                if (!sflag) help_show_longdoc(t);
            }
            if (pass == 1 && this_found) break;
        }
#ifndef SHELL_HOSTED
        /* the kernel shell's own commands, after bash's topics */
        for (const struct cmd *cc = cmds; cc->name; cc++) {
            bool bash_has = false;
            for (int k = 0; g_bash_topics[k].name; k++)
                if (g_bash_topics[k].bi && strcmp(g_bash_topics[k].name, cc->name) == 0)
                    bash_has = true;
            if (bash_has) continue;
            if (strcmp(pattern, cc->name) != 0 && !shell_glob_match(pattern, cc->name) &&
                strncmp(pattern, cc->name, plen) != 0)
                continue;
            shell_printf("%s: %s\n", cc->name, cc->help);
            found++;
        }
#endif
    }
    if (found == 0) {
        kprintf("help: no help topics match `%s'.  Try `help help' or "
                "`man -k %s' or `info %s'.\n", pattern, pattern, pattern);
        shell_set_status(1);
    }
}

/* ---- `NAME --help` ---------------------------------------------------------
 * bash's builtins meet `--help` in their option reading (common.h,
 * bashgetopt.c): internal_getopt returns GETOPT_HELP for a word `--help`
 * where it would read an option word -- after other options too, never
 * after `--`, an operand, or as an option's argument -- and CHECK_HELPOPT
 * and no_options look at the FIRST word only. Either way the builtin prints
 * builtin_help and returns 2 before doing anything else. tsh's builtins
 * read their options their own ways, so the question is answered here, once,
 * before one is called: by bash's option letters for it (from each .def's
 * internal_getopt call; ulimit's are the ones the guest's bash takes). OPTS
 * 0 means the first word only. Not here: echo, test, [, true, false and :,
 * which have no --help in bash; bind, whose --help is its usage line (bind
 * handles it); local, below. */
struct sh_helpopt { const char *name; const char *opts; };
static const struct sh_helpopt g_helpopts[] = {
    { ".", 0 }, { "bg", 0 }, { "break", 0 }, { "builtin", 0 }, { "caller", 0 },
    { "continue", 0 }, { "dirs", 0 }, { "eval", 0 }, { "exit", 0 }, { "fg", 0 },
    { "getopts", 0 }, { "kill", 0 }, { "let", 0 }, { "logout", 0 }, { "popd", 0 },
    { "pushd", 0 }, { "return", 0 }, { "shift", 0 }, { "source", 0 }, { "times", 0 },
    { "alias", "p" }, { "cd", "eLP" }, { "command", "pvV" },
    { "compgen", "abcdefgjko:prsuvA:G:W:P:S:X:F:C:DEI" },
    { "complete", "abcdefgjko:prsuvA:G:W:P:S:X:F:C:DEI" },
    { "compopt", "+o:DEI" }, { "declare", "+acfgilnprtuxAFGI" },
    { "disown", "ahr" }, { "enable", "adnpsf:" }, { "exec", "cla:" },
    { "export", "aAfnp" }, { "fc", ":e:lnrs" }, { "hash", "dlp:rt" },
    { "help", "dms" }, { "history", "acd:npsrw" }, { "jobs", "lpnxrs" },
    { "mapfile", "d:u:n:O:tC:c:s:" }, { "printf", "v:" }, { "pwd", "LP" },
    { "read", "ersa:d:i:n:p:t:u:N:" }, { "readarray", "d:u:n:O:tC:c:s:" },
    { "readonly", "aAfnp" }, { "set", "+abefhikmnprtuvxBCEHPTo;" },
    { "shopt", "psuoq" }, { "suspend", "f" }, { "trap", "lp" }, { "type", "afptP" },
    { "typeset", "+acfgilnprtuxAFGI" },
    { "ulimit", "aSHc;d;e;f;i;l;m;n;p;q;r;s;t;u;v;x;R;" },
    { "umask", "Sp" }, { "unalias", "a" }, { "unset", "fnv" }, { "wait", "fnp:" },
    { 0, 0 }
};

static bool shell_builtin_wants_help(int argc, char **argv) {
    if (argc < 2 || !argv[0]) return false;
    const char *name = argv[0], *opts = 0;
    bool known = false;
    /* `local --help` is caught before the function test (declare.def);
     * inside a function local reads declare's options */
    if (strcmp(name, "local") == 0) {
        known = true;
        opts = g_fn_depth > 0 ? "+acfgilnprtuxAFGI" : 0;
    }
    for (int i = 0; !known && g_helpopts[i].name; i++)
        if (strcmp(g_helpopts[i].name, name) == 0) {
            known = true;
            opts = g_helpopts[i].opts;
        }
    if (!known) return false;
    if (!opts) return strcmp(argv[1], "--help") == 0;
    struct sh_igo g;
    sh_igo_init(&g, name, argc, argv);
    g.quiet = true;
    int c;
    while ((c = sh_igo_next(&g, opts)) != -1) {
        if (c == SH_IGO_HELP) return true;
        if (c == '?') return false;
    }
    return false;
}

/* Run builtin C as NAME ARGS would: its --help first, as bash's would. */
static void shell_call_builtin(const struct cmd *c, int argc, char **argv) {
    if (shell_builtin_wants_help(argc, argv)) {
        shell_help_topic(argv[0]);
        shell_set_status(2);
        return;
    }
    c->fn(argc, argv);
}
