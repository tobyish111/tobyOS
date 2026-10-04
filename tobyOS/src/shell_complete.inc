/* shell_complete.inc -- programmable completion: `complete`, `compgen`,
 * `compopt`, and the generator the line editor's Tab runs.
 *
 * Included once by shell.c, near its end; everything here is static.
 *
 * THE SPEC IS BASH 5.2.15 (builtins/complete.def, pcomplete.c, pcomplib.c,
 * bashline.c; readline's complete.c for the filename generator), read as a
 * description of behaviour -- this is tsh's own code. tsh had a stub: compgen
 * listed files and /etc/passwd and returned 1 for everything else, complete
 * stored -W/-F/-C/-G text and ran none of it, compopt could not see an
 * invalid option (Oils 0286 0748-0798: 26 cases). What bash does, and so what
 * this does:
 *
 *   - options parse as bash's internal_getopt parses them, with its messages
 *     and usage lines, status 2 on any error;
 *   - a spec is ACTIONS (-a -b ... -A name), OPTIONS (-o name) and seven
 *     strings (-G -W -P -S -X -C -F). `complete` keeps one per command name
 *     in a table walked in bash's hash order (512 buckets, FNV-1, newest
 *     first in a bucket), which is the order `complete -p` prints in;
 *   - generating runs every action in bash's fixed order, then the glob,
 *     the word list (split at $IFS, quotes respected, then fully expanded),
 *     the function (COMP_* bound, its COMPREPLY taken), the command (its
 *     output one match a line); then -X filters, -P/-S decorate, and
 *     dirnames/plusdirs add directories; compgen adds bashdefault/default.
 */

/* ---- the action and option tables (bash's names, bits and letters) ---- */

#define CA_ALIAS      (1ul << 0)
#define CA_ARRAYVAR   (1ul << 1)
#define CA_BINDING    (1ul << 2)
#define CA_BUILTIN    (1ul << 3)
#define CA_COMMAND    (1ul << 4)
#define CA_DIRECTORY  (1ul << 5)
#define CA_DISABLED   (1ul << 6)
#define CA_ENABLED    (1ul << 7)
#define CA_EXPORT     (1ul << 8)
#define CA_FILE       (1ul << 9)
#define CA_FUNCTION   (1ul << 10)
#define CA_GROUP      (1ul << 11)
#define CA_HELPTOPIC  (1ul << 12)
#define CA_HOSTNAME   (1ul << 13)
#define CA_JOB        (1ul << 14)
#define CA_KEYWORD    (1ul << 15)
#define CA_RUNNING    (1ul << 16)
#define CA_SERVICE    (1ul << 17)
#define CA_SETOPT     (1ul << 18)
#define CA_SHOPT      (1ul << 19)
#define CA_SIGNAL     (1ul << 20)
#define CA_STOPPED    (1ul << 21)
#define CA_USER       (1ul << 22)
#define CA_VARIABLE   (1ul << 23)

#define COPT_DEFAULT     (1ul << 1)
#define COPT_FILENAMES   (1ul << 2)
#define COPT_DIRNAMES    (1ul << 3)
#define COPT_NOQUOTE     (1ul << 4)
#define COPT_NOSPACE     (1ul << 5)
#define COPT_BASHDEFAULT (1ul << 6)
#define COPT_PLUSDIRS    (1ul << 7)
#define COPT_NOSORT      (1ul << 8)

static const struct { const char *name; unsigned long flag; char opt; }
g_compacts[] = {
    { "alias",     CA_ALIAS,     'a' }, { "arrayvar",  CA_ARRAYVAR,   0  },
    { "binding",   CA_BINDING,    0  }, { "builtin",   CA_BUILTIN,   'b' },
    { "command",   CA_COMMAND,   'c' }, { "directory", CA_DIRECTORY, 'd' },
    { "disabled",  CA_DISABLED,   0  }, { "enabled",   CA_ENABLED,    0  },
    { "export",    CA_EXPORT,    'e' }, { "file",      CA_FILE,      'f' },
    { "function",  CA_FUNCTION,   0  }, { "helptopic", CA_HELPTOPIC,  0  },
    { "hostname",  CA_HOSTNAME,   0  }, { "group",     CA_GROUP,     'g' },
    { "job",       CA_JOB,       'j' }, { "keyword",   CA_KEYWORD,   'k' },
    { "running",   CA_RUNNING,    0  }, { "service",   CA_SERVICE,   's' },
    { "setopt",    CA_SETOPT,     0  }, { "shopt",     CA_SHOPT,      0  },
    { "signal",    CA_SIGNAL,     0  }, { "stopped",   CA_STOPPED,    0  },
    { "user",      CA_USER,      'u' }, { "variable",  CA_VARIABLE,  'v' },
    { 0, 0, 0 },
};

static const struct { const char *name; unsigned long flag; } g_compopts[] = {
    { "bashdefault", COPT_BASHDEFAULT }, { "default",   COPT_DEFAULT   },
    { "dirnames",    COPT_DIRNAMES    }, { "filenames", COPT_FILENAMES },
    { "noquote",     COPT_NOQUOTE     }, { "nosort",    COPT_NOSORT    },
    { "nospace",     COPT_NOSPACE     }, { "plusdirs",  COPT_PLUSDIRS  },
    { 0, 0 },
};

static int comp_find_act(const char *name) {
    for (int i = 0; g_compacts[i].name; i++)
        if (strcmp(g_compacts[i].name, name) == 0) return i;
    return -1;
}

static int comp_find_opt(const char *name) {
    for (int i = 0; g_compopts[i].name; i++)
        if (strcmp(g_compopts[i].name, name) == 0) return i;
    return -1;
}

/* bash's names for the -D, -E and -I pseudo-commands (pcomplete.h). */
#define COMP_DEFAULTCMD  "_DefaultCmD_"
#define COMP_EMPTYCMD    "_EmptycmD_"
#define COMP_INITIALWORD "_InitialWorD_"

/* ---- compspecs ---------------------------------------------------------- */

struct compspec {
    int refcount;               /* one `complete` call shares one spec */
    unsigned long actions, options;
    char *globpat, *words, *prefix, *suffix, *funcname, *command, *filterpat;
};

static struct sh_htab g_progcomp = { 0, 0, 0, 0, 512 };

static void compspec_unref(struct compspec *cs) {
    if (!cs || --cs->refcount > 0) return;
    kfree(cs->globpat); kfree(cs->words); kfree(cs->prefix); kfree(cs->suffix);
    kfree(cs->funcname); kfree(cs->command); kfree(cs->filterpat);
    kfree(cs);
}

static struct compspec *compspec_search(const char *name) {
    int i = sh_htab_find(&g_progcomp, name);
    return i >= 0 ? (struct compspec *)g_progcomp.e[i].data : 0;
}

static bool progcomp_insert(const char *name, struct compspec *cs) {
    bool created;
    int i = sh_htab_insert(&g_progcomp, name, &created);
    if (i < 0) return false;
    cs->refcount++;
    if (!created) compspec_unref((struct compspec *)g_progcomp.e[i].data);
    g_progcomp.e[i].data = cs;
    return true;
}

static bool progcomp_remove(const char *name) {
    int i = sh_htab_find(&g_progcomp, name);
    if (i < 0) return false;
    compspec_unref((struct compspec *)g_progcomp.e[i].data);
    sh_htab_remove_at(&g_progcomp, i);
    return true;
}

static void progcomp_flush(void) {
    while (g_progcomp.n > 0) {
        compspec_unref((struct compspec *)g_progcomp.e[g_progcomp.n - 1].data);
        sh_htab_remove_at(&g_progcomp, g_progcomp.n - 1);
    }
}

/* ---- bash's builtin table and help topics, in its order -----------------
 * Every `help` topic with its synopsis; `bi` when it is a builtin (bash's
 * table has a function for it) rather than a reserved word's topic.
 * Measured from the guest's bash 5.2.15 (`help -s` for each `compgen -A
 * helptopic`, logs/b12/guest-bash-helps.txt). The synopsis is also the
 * usage line a builtin prints on a bad option. */
struct sh_bash_topic { const char *name; const char *syn; bool bi; };
static const struct sh_bash_topic g_bash_topics[] = {
    { "%", "job_spec [&]", false },
    { "(( ... ))", "(( expression ))", false },
    { ".", ". filename [arguments]", true },
    { ":", ":", true },
    { "[", "[ arg... ]", true },
    { "[[ ... ]]", "[[ expression ]]", false },
    { "alias", "alias [-p] [name[=value] ... ]", true },
    { "bg", "bg [job_spec ...]", true },
    { "bind", "bind [-lpsvPSVX] [-m keymap] [-f filename] [-q name] [-u name] [-r keyseq] [-x keyseq:shell-command] [keyseq:readline-function or readline-command]", true },
    { "break", "break [n]", true },
    { "builtin", "builtin [shell-builtin [arg ...]]", true },
    { "caller", "caller [expr]", true },
    { "case", "case WORD in [PATTERN [| PATTERN]...) COMMANDS ;;]... esac", false },
    { "cd", "cd [-L|[-P [-e]] [-@]] [dir]", true },
    { "command", "command [-pVv] command [arg ...]", true },
    { "compgen", "compgen [-abcdefgjksuv] [-o option] [-A action] [-G globpat] [-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [word]", true },
    { "complete", "complete [-abcdefgjksuv] [-pr] [-DEI] [-o option] [-A action] [-G globpat] [-W wordlist] [-F function] [-C command] [-X filterpat] [-P prefix] [-S suffix] [name ...]", true },
    { "compopt", "compopt [-o|+o option] [-DEI] [name ...]", true },
    { "continue", "continue [n]", true },
    { "coproc", "coproc [NAME] command [redirections]", false },
    { "declare", "declare [-aAfFgiIlnrtux] [name[=value] ...] or declare -p [-aAfFilnrtux] [name ...]", true },
    { "dirs", "dirs [-clpv] [+N] [-N]", true },
    { "disown", "disown [-h] [-ar] [jobspec ... | pid ...]", true },
    { "echo", "echo [-neE] [arg ...]", true },
    { "enable", "enable [-a] [-dnps] [-f filename] [name ...]", true },
    { "eval", "eval [arg ...]", true },
    { "exec", "exec [-cl] [-a name] [command [argument ...]] [redirection ...]", true },
    { "exit", "exit [n]", true },
    { "export", "export [-fn] [name[=value] ...] or export -p", true },
    { "false", "false", true },
    { "fc", "fc [-e ename] [-lnr] [first] [last] or fc -s [pat=rep] [command]", true },
    { "fg", "fg [job_spec]", true },
    { "for", "for NAME [in WORDS ... ] ; do COMMANDS; done", false },
    { "for ((", "for (( exp1; exp2; exp3 )); do COMMANDS; done", false },
    { "function", "function name { COMMANDS ; } or name () { COMMANDS ; }", false },
    { "getopts", "getopts optstring name [arg ...]", true },
    { "hash", "hash [-lr] [-p pathname] [-dt] [name ...]", true },
    { "help", "help [-dms] [pattern ...]", true },
    { "history", "history [-c] [-d offset] [n] or history -anrw [filename] or history -ps arg [arg...]", true },
    { "if", "if COMMANDS; then COMMANDS; [ elif COMMANDS; then COMMANDS; ]... [ else COMMANDS; ] fi", false },
    { "jobs", "jobs [-lnprs] [jobspec ...] or jobs -x command [args]", true },
    { "kill", "kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]", true },
    { "let", "let arg [arg ...]", true },
    { "local", "local [option] name[=value] ...", true },
    { "logout", "logout [n]", true },
    { "mapfile", "mapfile [-d delim] [-n count] [-O origin] [-s count] [-t] [-u fd] [-C callback] [-c quantum] [array]", true },
    { "popd", "popd [-n] [+N | -N]", true },
    { "printf", "printf [-v var] format [arguments]", true },
    { "pushd", "pushd [-n] [+N | -N | dir]", true },
    { "pwd", "pwd [-LP]", true },
    { "read", "read [-ers] [-a array] [-d delim] [-i text] [-n nchars] [-N nchars] [-p prompt] [-t timeout] [-u fd] [name ...]", true },
    { "readarray", "readarray [-d delim] [-n count] [-O origin] [-s count] [-t] [-u fd] [-C callback] [-c quantum] [array]", true },
    { "readonly", "readonly [-aAf] [name[=value] ...] or readonly -p", true },
    { "return", "return [n]", true },
    { "select", "select NAME [in WORDS ... ;] do COMMANDS; done", false },
    { "set", "set [-abefhkmnptuvxBCEHPT] [-o option-name] [--] [-] [arg ...]", true },
    { "shift", "shift [n]", true },
    { "shopt", "shopt [-pqsu] [-o] [optname ...]", true },
    { "source", "source filename [arguments]", true },
    { "suspend", "suspend [-f]", true },
    { "test", "test [expr]", true },
    { "time", "time [-p] pipeline", false },
    { "times", "times", true },
    { "trap", "trap [-lp] [[arg] signal_spec ...]", true },
    { "true", "true", true },
    { "type", "type [-afptP] name [name ...]", true },
    { "typeset", "typeset [-aAfFgiIlnrtux] name[=value] ... or typeset -p [-aAfFilnrtux] [name ...]", true },
    { "ulimit", "ulimit [-SHabcdefiklmnpqrstuvxPRT] [limit]", true },
    { "umask", "umask [-p] [-S] [mode]", true },
    { "unalias", "unalias [-a] name [name ...]", true },
    { "unset", "unset [-f] [-v] [-n] [name ...]", true },
    { "until", "until COMMANDS; do COMMANDS-2; done", false },
    { "variables", "variables - Names and meanings of some shell variables", false },
    { "wait", "wait [-fn] [-p var] [id ...]", true },
    { "while", "while COMMANDS; do COMMANDS-2; done", false },
    { "{ ... }", "{ COMMANDS ; }", false },
    { 0, 0, false },
};

/* The usage line for builtin NAME: "NAME: usage: SYNOPSIS", no prefix --
 * bash's builtin_usage writes it bare. */
static void sh_builtin_usage(const char *name) {
    for (int i = 0; g_bash_topics[i].name; i++)
        if (g_bash_topics[i].bi && strcmp(g_bash_topics[i].name, name) == 0) {
            kprintf_raw("%s: usage: %s\n", name, g_bash_topics[i].syn);
            return;
        }
}

/* bash's reserved words, in its word_token_alist order. */
static const char *const g_bash_keywords[] = {
    "if", "then", "else", "elif", "fi", "case", "esac", "for", "select",
    "while", "until", "do", "done", "in", "function", "time", "{", "}", "!",
    "[[", "]]", "coproc", 0
};

/* The `set -o` names, in bash's o_options order. */
static const char *const g_bash_setopts[] = {
    "allexport", "braceexpand", "emacs", "errexit", "errtrace", "functrace",
    "hashall", "histexpand", "history", "ignoreeof", "interactive-comments",
    "keyword", "monitor", "noclobber", "noexec", "noglob", "nolog", "notify",
    "nounset", "onecmd", "physical", "pipefail", "posix", "privileged",
    "verbose", "vi", "xtrace", 0
};

/* bash's signal_names[] on the guest (musl: 32-34 are SIGJUNK, the
 * real-time range starts at 35): index = signal number, then the three
 * trap-only conditions. `compgen -A signal` lists EXIT, these, and those. */
static void sh_bash_signame(int n, char *buf, size_t cap) {
    static const char *const low[] = {
        "EXIT", "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT",
        "SIGBUS", "SIGFPE", "SIGKILL", "SIGUSR1", "SIGSEGV", "SIGUSR2",
        "SIGPIPE", "SIGALRM", "SIGTERM", "SIGSTKFLT", "SIGCHLD", "SIGCONT",
        "SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU", "SIGURG", "SIGXCPU",
        "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGIO", "SIGPWR",
        "SIGSYS",
    };
    if (n >= 0 && n <= 31) ksnprintf(buf, cap, "%s", low[n]);
    else if (n >= 32 && n <= 34) ksnprintf(buf, cap, "SIGJUNK(%d)", n);
    else if (n == 35) ksnprintf(buf, cap, "SIGRTMIN");
    else if (n >= 36 && n <= 49) ksnprintf(buf, cap, "SIGRTMIN+%d", n - 35);
    else if (n >= 50 && n <= 63) ksnprintf(buf, cap, "SIGRTMAX-%d", 64 - n);
    else if (n == 64) ksnprintf(buf, cap, "SIGRTMAX");
    else if (n == 65) ksnprintf(buf, cap, "DEBUG");
    else if (n == 66) ksnprintf(buf, cap, "ERR");
    else if (n == 67) ksnprintf(buf, cap, "RETURN");
    else buf[0] = '\0';
}

/* ---- the generators ----------------------------------------------------- */

/* A variable bash lists: set, or declared with a value (NOVALUE is bash's
 * "invisible"), or a live array. */
static void comp_collect_vars(struct sh_strv *out, bool exported_only,
                              bool arrays_only) {
    struct sh_strv names = { 0, 0, 0 };
    if (!arrays_only) {
        for (int i = 0; i < g_envc; i++) {
            const char *e = g_env[i];
            if (!e) continue;
            if (g_env_flags[i] & SHVAR_NOVALUE) continue;
            if (exported_only && !(g_env_flags[i] & SHVAR_EXPORTED)) continue;
            size_t kl = env_key_len(e);
            if (!kl || !shell_name_is_valid(e, kl)) continue;
            char nb[256];
            if (kl >= sizeof nb) continue;
            memcpy(nb, e, kl);
            nb[kl] = '\0';
            if (!sv_has(&names, nb)) (void)sv_add(&names, nb);
        }
    }
    if (!exported_only) {
        for (int i = 0; i < g_narrays; i++) {
            const struct sh_array *a = g_arrays[i];
            if (!a || !a->name || !shell_name_is_valid(a->name, strlen(a->name)))
                continue;
            int ix = env_find(a->name, strlen(a->name));
            if (ix >= 0 && (g_env_flags[ix] & SHVAR_NOVALUE)) continue;
            if (!sv_has(&names, a->name)) (void)sv_add(&names, a->name);
        }
        if (!arrays_only) {
            for (int d = 0; d < DV_COUNT; d++)
                if (!g_dynvar_gone[d] && !sv_has(&names, g_dynvar_names[d]))
                    (void)sv_add(&names, g_dynvar_names[d]);
            static const char *const synth[] = { "LINENO", 0 };
            for (int k = 0; synth[k]; k++)
                if (!sv_has(&names, synth[k])) (void)sv_add(&names, synth[k]);
        }
    }
    sv_sort(&names);
    for (int i = 0; i < names.n; i++) (void)sv_add(out, names.v[i]);
    sv_free(&names);
}

static void comp_collect_aliases(struct sh_strv *out) {
    struct sh_strv a = { 0, 0, 0 };
    for (int i = 0; i < g_alias_tab.n; i++)
        (void)sv_add(&a, g_alias_tab.e[i].key);
    sv_sort(&a);
    for (int i = 0; i < a.n; i++) (void)sv_add(out, a.v[i]);
    sv_free(&a);
}

static void comp_collect_functions(struct sh_strv *out) {
    struct sh_strv f = { 0, 0, 0 };
    for (int i = 0; i < g_nfunctions; i++)
        if (g_functions[i].name) (void)sv_add(&f, g_functions[i].name);
    sv_sort(&f);
    for (int i = 0; i < f.n; i++) (void)sv_add(out, f.v[i]);
    sv_free(&f);
}

/* which: 0 every builtin, 1 enabled, 2 disabled, 3 every help topic */
static void comp_collect_builtins(struct sh_strv *out, int which) {
    for (int i = 0; g_bash_topics[i].name; i++) {
        const struct sh_bash_topic *t = &g_bash_topics[i];
        if (which != 3 && !t->bi) continue;
        if (which == 1 && shell_builtin_disabled(t->name)) continue;
        if (which == 2 && !shell_builtin_disabled(t->name)) continue;
        (void)sv_add(out, t->name);
    }
}

/* The first field of each line of a colon file (/etc/passwd, /etc/group). */
static void comp_collect_colon_file(struct sh_strv *out, const char *path) {
    void *buf = 0;
    size_t sz = 0;
    if (vfs_read_all(path, &buf, &sz) != VFS_OK) return;
    const char *p = (const char *)buf, *end = p + sz;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        const char *eol = p;
        if (p < end) p++;
        const char *c = line;
        while (c < eol && *c != ':') c++;
        if (c > line && *line != '#') (void)sv_add_n(out, line, (size_t)(c - line));
    }
    kfree(buf);
}

/* Whitespace-separated fields of a file, skipping comments. With
 * skip_first, every line's first field (an address) is dropped -- the
 * hosts file; else the names and aliases of /etc/services ("name port/proto
 * alias..." -- the port field is dropped too). */
static void comp_collect_field_file(struct sh_strv *out, const char *path,
                                    bool hosts) {
    void *buf = 0;
    size_t sz = 0;
    if (vfs_read_all(path, &buf, &sz) != VFS_OK) return;
    const char *p = (const char *)buf, *end = p + sz;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        const char *eol = p;
        if (p < end) p++;
        const char *hash = line;
        while (hash < eol && *hash != '#') hash++;
        int field = 0;
        const char *q = line;
        while (q < hash) {
            while (q < hash && (*q == ' ' || *q == '\t' || *q == '\r')) q++;
            if (q >= hash) break;
            const char *ws = q;
            while (q < hash && *q != ' ' && *q != '\t' && *q != '\r') q++;
            bool take = hosts ? field >= 1 : field != 1;
            if (take) (void)sv_add_n(out, ws, (size_t)(q - ws));
            field++;
        }
    }
    kfree(buf);
}

/* Job command names (their first word): `state` -1 every job, 0 running,
 * 1 stopped -- highest slot first, as bash walks its job table. */
static void comp_collect_jobs(struct sh_strv *out, int state) {
    for (int i = g_jobs_cap - 1; i >= 0; i--) {
        const struct job *j = g_jobs ? g_jobs[i] : 0;
        if (!j || j->id == 0) continue;
        if (state == 0 && (j->stopped || j->dead)) continue;
        if (state == 1 && !j->stopped) continue;
        const char *nm = j->name ? j->name : "";
        size_t k = 0;
        while (nm[k] && nm[k] != ' ' && nm[k] != '\t' && nm[k] != '\n') k++;
        (void)sv_add_n(out, nm, k);
    }
}

/* Keep the members of `all` that begin with `text`. */
static void comp_prefix_filter(struct sh_strv *out, const struct sh_strv *all,
                               const char *text) {
    size_t tl = strlen(text);
    for (int i = 0; i < all->n; i++)
        if (tl == 0 || strncmp(all->v[i], text, tl) == 0) (void)sv_add(out, all->v[i]);
}

/* bash_dequote_text -> bash_dequote_filename: the quotes and backslashes
 * come off the word typed. Its starting quote state is the word's first
 * character when that is a quote -- which therefore CLOSES at once, as
 * bash's does. A backslash stays inside single quotes, and inside double
 * quotes unless it escapes one of " \ $ ` newline. */
static void comp_dequote(const char *text, struct shell_sb *out) {
    char quoted = (text[0] == '"' || text[0] == '\'') ? text[0] : 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\\') {
            if (quoted == '\'')
                (void)sb_putc(out, '\\');
            else if (quoted == '"' && p[1] != '"' && p[1] != '\\' &&
                     p[1] != '$' && p[1] != '`' && p[1] != '\n')
                (void)sb_putc(out, '\\');
            if (!*++p) return;
            (void)sb_putc(out, *p);
            continue;
        }
        if (quoted && *p == quoted) { quoted = 0; continue; }
        if (!quoted && (*p == '\'' || *p == '"')) { quoted = *p; continue; }
        (void)sb_putc(out, *p);
    }
}

static bool comp_stat(const char *path, struct vfs_stat *st) {
    char canon[VFS_PATH_MAX];
    if (shell_canonicalize_path(path, canon, sizeof canon) < 0) return false;
    return vfs_stat(canon, st) == VFS_OK;
}

static bool comp_is_dir(const char *path) {
    struct vfs_stat st;
    return comp_stat(path, &st) && st.type == VFS_TYPE_DIR;
}

static bool comp_is_exec_file(const char *path) {
    struct vfs_stat st;
    return comp_stat(path, &st) && st.type != VFS_TYPE_DIR && test_access_ok(&st, 1u);
}

/* readline's rl_filename_completion_function, every match: TEXT's directory
 * part (tilde-expanded to look in, kept as typed in the result) and the
 * entries beginning with its last component -- with an empty last
 * component, every entry but `.` and `..`. Directory order. */
static void comp_filenames(struct sh_strv *out, const char *text) {
    const char *slash = comp_strrchr(text, '/');
    SH_SB(users);               /* the directory as typed, with its slash */
    SH_SB(look);                /* the directory to open */
    const char *fname = text;
    if (slash) {
        if (sb_putmem(&users, text, (size_t)(slash - text) + 1) < 0) return;
        fname = slash + 1;
    }
    if (users.len == 0) {
        (void)sb_puts(&look, ".");
    } else if (users.p[0] == '~') {
        size_t ue = 1;
        while (ue < users.len && users.p[ue] != '/') ue++;
        const char *home = 0;
        if (ue == 1) home = env_get("HOME");
        if (!home) { (void)sb_puts(&look, users.p); }
        else { (void)sb_puts(&look, home); (void)sb_puts(&look, users.p + ue); }
    } else {
        (void)sb_puts(&look, users.p);
    }
    char dir[VFS_PATH_MAX];
    if (shell_canonicalize_path(look.p, dir, sizeof dir) < 0) return;
    struct vfs_dir d;
    if (vfs_opendir(dir, &d) != VFS_OK) return;
    size_t fl = strlen(fname);
    struct vfs_dirent ent;
    while (vfs_readdir(&d, &ent) == VFS_OK) {
        const char *nm = ent.name;
        if (fl == 0) {
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
        } else if (strncmp(nm, fname, fl) != 0) {
            continue;
        }
        SH_SB(r);
        if (sb_puts(&r, users.p) < 0 || sb_puts(&r, nm) < 0) break;
        (void)sv_add(out, r.p);
    }
    vfs_closedir(&d);
}

/* bash_directory_completion_matches: the filename matches that are
 * directories (following links). */
static void comp_directories(struct sh_strv *out, const char *text) {
    struct sh_strv m = { 0, 0, 0 };
    comp_filenames(&m, text);
    for (int i = 0; i < m.n; i++) {
        SH_SB(path);
        const char *v = m.v[i];
        if (v[0] == '~' && (v[1] == '/' || v[1] == '\0')) {
            const char *home = env_get("HOME");
            (void)sb_puts(&path, home ? home : "");
            (void)sb_puts(&path, v + 1);
        } else {
            (void)sb_puts(&path, v);
        }
        if (comp_is_dir(path.p)) (void)sv_add(out, v);
    }
    sv_free(&m);
}

static bool comp_has_glob(const char *s) {
    for (; *s; s++) {
        if (*s == '\\' && s[1]) { s++; continue; }
        if (*s == '*' || *s == '?' || *s == '[') return true;
    }
    return false;
}

/* bash's command_word_completion_function, every match: aliases, reserved
 * words, functions, enabled builtins, then -- a word naming a directory
 * here once, then each $PATH directory's executable files beginning with
 * it. A word with a slash is a path: its executables and directories. */
static void comp_commands(struct sh_strv *out, const char *text) {
    size_t tl = strlen(text);
    bool absolute = comp_strchr(text, '/') != 0;
    if (!absolute && !comp_has_glob(text)) {
        struct sh_strv a = { 0, 0, 0 };
        comp_collect_aliases(&a);
        comp_prefix_filter(out, &a, text);
        sv_free(&a);
        for (int i = 0; g_bash_keywords[i]; i++)
            if (strncmp(g_bash_keywords[i], text, tl) == 0)
                (void)sv_add(out, g_bash_keywords[i]);
        struct sh_strv f = { 0, 0, 0 };
        comp_collect_functions(&f);
        comp_prefix_filter(out, &f, text);
        sv_free(&f);
        struct sh_strv b = { 0, 0, 0 };
        comp_collect_builtins(&b, 1);
        comp_prefix_filter(out, &b, text);
        sv_free(&b);
        if (tl && text[0] != '~' && comp_is_dir(text)) (void)sv_add(out, text);
        const char *path = env_get("PATH");
        if (!path) return;
        const char *p = path;
        for (;;) {
            const char *e = p;
            while (*e && *e != ':') e++;
            SH_SB(dir);
            if (e == p) (void)sb_puts(&dir, ".");
            else (void)sb_putmem(&dir, p, (size_t)(e - p));
            if (dir.p[0] == '~') {
                const char *home = env_get("HOME");
                SH_SB(x);
                (void)sb_puts(&x, home ? home : "");
                (void)sb_puts(&x, dir.p + 1);
                (void)sb_set(&dir, x.p);
            }
            SH_SB(hint);
            (void)sb_puts(&hint, dir.p);
            if (hint.len && hint.p[hint.len - 1] != '/') (void)sb_putc(&hint, '/');
            (void)sb_puts(&hint, text);
            struct sh_strv m = { 0, 0, 0 };
            comp_filenames(&m, hint.p);
            for (int i = 0; i < m.n; i++) {
                const char *base = comp_strrchr(m.v[i], '/');
                base = base ? base + 1 : m.v[i];
                if (strncmp(base, text, tl) == 0 && comp_is_exec_file(m.v[i]))
                    (void)sv_add(out, base);
            }
            sv_free(&m);
            if (!*e) break;
            p = e + 1;
        }
        return;
    }
    struct sh_strv m = { 0, 0, 0 };
    if (comp_has_glob(text)) {
        /* a pattern: its matches that are executable or directories */
        (void)m;
        return;
    }
    comp_filenames(&m, text);
    for (int i = 0; i < m.n; i++) {
        SH_SB(path);
        const char *v = m.v[i];
        if (v[0] == '~' && (v[1] == '/' || v[1] == '\0')) {
            const char *home = env_get("HOME");
            (void)sb_puts(&path, home ? home : "");
            (void)sb_puts(&path, v + 1);
        } else {
            (void)sb_puts(&path, v);
        }
        if (comp_is_exec_file(path.p) || comp_is_dir(path.p)) (void)sv_add(out, v);
    }
    sv_free(&m);
}

/* rl_username_completion_function: a leading `~` is kept on the results. */
static void comp_users(struct sh_strv *out, const char *text) {
    bool tilde = text[0] == '~';
    const char *t = tilde ? text + 1 : text;
    struct sh_strv all = { 0, 0, 0 };
    comp_collect_colon_file(&all, "/etc/passwd");
    size_t tl = strlen(t);
    for (int i = 0; i < all.n; i++) {
        if (strncmp(all.v[i], t, tl) != 0) continue;
        SH_SB(r);
        if (tilde) (void)sb_putc(&r, '~');
        (void)sb_puts(&r, all.v[i]);
        (void)sv_add(out, r.p);
    }
    sv_free(&all);
}

/* Run each action of `actions` for TEXT, in bash's order, appending. */
static void comp_gen_actions(struct sh_strv *ret, unsigned long acts,
                             const char *text) {
    SH_SB(dq);
    comp_dequote(text, &dq);
    const char *t = dq.p;
    struct sh_strv all = { 0, 0, 0 };
#define COMP_ITEMS(flag, fill) \
    do { if (acts & (flag)) { fill; comp_prefix_filter(ret, &all, t); sv_free(&all); } } while (0)
    COMP_ITEMS(CA_ALIAS, comp_collect_aliases(&all));
    COMP_ITEMS(CA_ARRAYVAR, comp_collect_vars(&all, false, true));
    COMP_ITEMS(CA_BINDING, rl_funmap_names(&all));
    COMP_ITEMS(CA_BUILTIN, comp_collect_builtins(&all, 0));
    COMP_ITEMS(CA_DISABLED, comp_collect_builtins(&all, 2));
    COMP_ITEMS(CA_ENABLED, comp_collect_builtins(&all, 1));
    COMP_ITEMS(CA_EXPORT, comp_collect_vars(&all, true, false));
    COMP_ITEMS(CA_FUNCTION, comp_collect_functions(&all));
    COMP_ITEMS(CA_HELPTOPIC, comp_collect_builtins(&all, 3));
    COMP_ITEMS(CA_HOSTNAME, {
        const char *hf = env_get("HOSTFILE");
        comp_collect_field_file(&all, hf && *hf ? hf : "/etc/hosts", true);
    });
    COMP_ITEMS(CA_JOB, comp_collect_jobs(&all, -1));
    COMP_ITEMS(CA_KEYWORD, for (int i = 0; g_bash_keywords[i]; i++) (void)sv_add(&all, g_bash_keywords[i]));
    COMP_ITEMS(CA_RUNNING, comp_collect_jobs(&all, 0));
    COMP_ITEMS(CA_SETOPT, for (int i = 0; g_bash_setopts[i]; i++) (void)sv_add(&all, g_bash_setopts[i]));
    COMP_ITEMS(CA_SHOPT, for (int i = 0; g_shopts[i].name; i++) (void)sv_add(&all, g_shopts[i].name));
    COMP_ITEMS(CA_SIGNAL, for (int i = 0; i <= 67; i++) { char nb[24]; sh_bash_signame(i, nb, sizeof nb); if (nb[0]) (void)sv_add(&all, nb); });
    COMP_ITEMS(CA_STOPPED, comp_collect_jobs(&all, 1));
    COMP_ITEMS(CA_VARIABLE, comp_collect_vars(&all, false, false));
#undef COMP_ITEMS
    /* THE REST TAKE THE WORD AS GIVEN: bash dequotes only the item lists
     * above (and -W). A file name is dequoted only where readline set its
     * dequoting hook -- an interactive shell; `compgen -f "foo'"` in a
     * script looks for names beginning with foo' (Oils 0786). */
    if (acts & CA_COMMAND) comp_commands(ret, text);
    if (acts & CA_FILE) comp_filenames(ret, g_interactive ? t : text);
    if (acts & CA_USER) comp_users(ret, text);
    if (acts & CA_GROUP) {
        comp_collect_colon_file(&all, "/etc/group");
        comp_prefix_filter(ret, &all, text);
        sv_free(&all);
    }
    if (acts & CA_SERVICE) {
        comp_collect_field_file(&all, "/etc/services", false);
        comp_prefix_filter(ret, &all, text);
        sv_free(&all);
    }
    if (acts & CA_DIRECTORY) comp_directories(ret, text);
}

/* split_at_delims with $IFS: fields end at an IFS character outside quotes,
 * a backslash pair, $( ), ${ } or ` `; a run of IFS characters is one
 * break. The quotes stay for the expansion that follows. */
static const char *comp_skip_quoted(const char *p) {
    char c = *p;
    if (c == '\\') return p[1] ? p + 2 : p + 1;
    if (c == '\'') {
        p++;
        while (*p && *p != '\'') p++;
        return *p ? p + 1 : p;
    }
    if (c == '"' || c == '`') {
        p++;
        while (*p && *p != c) {
            if (*p == '\\' && p[1]) p += 2;
            else if (c == '"' && *p == '$' && (p[1] == '(' || p[1] == '{')) p = comp_skip_quoted(p);
            else p++;
        }
        return *p ? p + 1 : p;
    }
    if (c == '$' && (p[1] == '(' || p[1] == '{')) {
        char open = p[1], close = open == '(' ? ')' : '}';
        int depth = 1;
        p += 2;
        while (*p && depth > 0) {
            if (*p == '\\' || *p == '\'' || *p == '"' || *p == '`' ||
                (*p == '$' && (p[1] == '(' || p[1] == '{'))) {
                p = comp_skip_quoted(p);
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) depth--;
            p++;
        }
        return p;
    }
    if (c == '$' && p[1] == '\'') {
        p += 2;
        while (*p && *p != '\'') { if (*p == '\\' && p[1]) p++; p++; }
        return *p ? p + 1 : p;
    }
    return p + 1;
}

static void comp_split_ifs(const char *s, struct sh_strv *out) {
    const char *ifs = env_get("IFS");
    if (!ifs) ifs = " \t\n";
    const char *p = s;
    while (*p && comp_strchr(ifs, *p) && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
    while (*p) {
        const char *ws = p;
        while (*p && !comp_strchr(ifs, *p)) {
            if (*p == '\\' || *p == '\'' || *p == '"' || *p == '`' ||
                (*p == '$' && (p[1] == '(' || p[1] == '{' || p[1] == '\'')))
                p = comp_skip_quoted(p);
            else
                p++;
        }
        if (p > ws) (void)sv_add_n(out, ws, (size_t)(p - ws));
        while (*p && comp_strchr(ifs, *p)) p++;
    }
}

/* Expand each word as a command's arguments would be -- brace, tilde,
 * parameter, command and arithmetic expansion, splitting, globbing, quote
 * removal -- through the array-literal expander, every word an ordinary
 * element (plain_only). */
static void comp_expand_words(const struct sh_strv *words, struct sh_strv *out) {
    static unsigned seq;
    char tmp[32];
    ksnprintf(tmp, sizeof tmp, "%ccomp%u", SH_ARRAY_TMP_MARK, seq++);
    SH_SB(lit);
    for (int i = 0; i < words->n; i++) {
        /* ONE WORD STAYS ONE ELEMENT. A word from split_at_delims can hold
         * a bare blank or `;` (IFS=':' leaves "ham cheese" whole), which
         * the literal's own parser would take as a separator -- so every
         * unquoted character it treats specially is escaped first. Quoted
         * spans and substitutions pass through untouched. */
        if (sb_putc(&lit, ' ') < 0) return;
        const char *w = words->v[i];
        if (*w == '#' && sb_putc(&lit, '\\') < 0) return;
        for (const char *p = w; *p; ) {
            if (*p == '\\' || *p == '\'' || *p == '"' || *p == '`' ||
                (*p == '$' && (p[1] == '(' || p[1] == '{' || p[1] == '\''))) {
                const char *e = comp_skip_quoted(p);
                if (sb_putmem(&lit, p, (size_t)(e - p)) < 0) return;
                p = e;
                continue;
            }
            if (comp_strchr(" \t\n;()|&<>", *p) && sb_putc(&lit, '\\') < 0) return;
            if (sb_putc(&lit, *p++) < 0) return;
        }
    }
    if (sb_puts(&lit, " )") < 0) return;
    if (!sh_array_create(tmp)) return;
    long next = 0;
    if (sh_array_literal_fill_ex(tmp, tmp, lit.p, false, &next, true, 0) == 0) {
        struct sh_array *a = sh_array_find_literal(tmp);
        for (int i = 0; a && i < a->n; i++) (void)sv_add(out, a->e[i].val);
    }
    sh_array_unset_literal(tmp);
}

static void comp_wordlist(struct sh_strv *ret, const char *wl, const char *text) {
    if (!wl || !*wl) return;
    struct sh_strv toks = { 0, 0, 0 }, ex = { 0, 0, 0 };
    comp_split_ifs(wl, &toks);
    comp_expand_words(&toks, &ex);
    SH_SB(dq);
    comp_dequote(text, &dq);
    size_t tl = dq.len;
    for (int i = 0; i < ex.n; i++)
        if (tl == 0 || strncmp(ex.v[i], dq.p, tl) == 0) (void)sv_add(ret, ex.v[i]);
    sv_free(&toks);
    sv_free(&ex);
}

/* -G: the pattern's matches as pathname expansion makes them, TEXT unused;
 * nothing when it matches nothing. */
static void comp_globpat(struct sh_strv *ret, const char *pat) {
    SH_SB(w);
    for (const char *p = pat; *p; p++) {
        char c = *p;
        if (c == '$' || c == '`' || c == '"' || c == '\'' || c == ' ' ||
            c == '\t' || c == '\n' || c == '{' || c == '}' || c == ';' ||
            c == '&' || c == '|' || c == '<' || c == '>' ||
            (c == '~' && p == pat))
            (void)sb_putc(&w, '\\');
        (void)sb_putc(&w, c);
    }
    struct sh_strv one = { 0, 0, 0 }, ex = { 0, 0, 0 };
    (void)sv_add(&one, w.p);
    bool saved = g_shopt_nullglob;
    g_shopt_nullglob = true;
    comp_expand_words(&one, &ex);
    g_shopt_nullglob = saved;
    for (int i = 0; i < ex.n; i++) (void)sv_add(ret, ex.v[i]);
    sv_free(&one);
    sv_free(&ex);
}

/* Bind COMP_LINE, COMP_POINT, COMP_TYPE, COMP_KEY (exported for a -C
 * command), and for a function COMP_WORDS and COMP_CWORD. */
static void comp_bind_vars(const char *line, int point, const struct sh_strv *words,
                           int cword, bool exported) {
    char nb[24];
    (void)env_set("COMP_LINE", line);
    ksnprintf(nb, sizeof nb, "%d", point);
    (void)env_set("COMP_POINT", nb);
    (void)env_set("COMP_TYPE", "0");
    (void)env_set("COMP_KEY", "0");
    if (exported) {
        const char *const ex[] = { "COMP_LINE", "COMP_POINT", "COMP_TYPE", "COMP_KEY", 0 };
        for (int i = 0; ex[i]; i++) {
            int ix = env_find(ex[i], strlen(ex[i]));
            if (ix >= 0) g_env_flags[ix] |= SHVAR_EXPORTED;
        }
        return;
    }
    sh_array_unset("COMP_WORDS");
    (void)sh_array_create("COMP_WORDS");
    for (int i = 0; words && i < words->n; i++) (void)sh_array_set("COMP_WORDS", i, words->v[i]);
    ksnprintf(nb, sizeof nb, "%d", cword);
    (void)env_set("COMP_CWORD", nb);
}

static void comp_unbind_vars(void) {
    const char *const v[] = { "COMP_LINE", "COMP_POINT", "COMP_TYPE", "COMP_KEY", "COMP_CWORD", 0 };
    for (int i = 0; v[i]; i++) {
        int ix = env_find(v[i], strlen(v[i]));
        if (ix >= 0) env_remove_at(ix);
    }
    sh_array_unset("COMP_WORDS");
    {
        int ix = env_find("COMP_WORDS", 10);
        if (ix >= 0) env_remove_at(ix);
    }
}

/* The completion being generated: what compopt with no NAME changes. */
static struct compspec *g_pcomp_curcs;
static const char *g_pcomp_curcmd;
static bool g_pcomp_completing;     /* inside the line editor's Tab */

/* -F: run FUNC with $1 the command, $2 the word, $3 the word before it;
 * its COMPREPLY (a scalar is element 0) is the list, in index order.
 * Unknown function: bash's internal error, and nothing. */
static void comp_function(struct sh_strv *ret, const char *func, const char *cmd,
                          const char *text, const char *line, int point,
                          const struct sh_strv *words, int cword, bool *found) {
    *found = true;
    if (!shell_function_lookup(func)) {
        kprintf("completion: function `%s' not found\n", func);
        return;
    }
    comp_bind_vars(line, point, words, cword - 1, false);
    const char *prev = (words && cword >= 2 && cword - 2 < words->n) ? words->v[cword - 2] : "";
    char *av[5];
    av[0] = (char *)func;
    av[1] = (char *)cmd;
    av[2] = (char *)text;
    av[3] = (char *)prev;
    av[4] = 0;
    struct shell_simple sc;
    memset(&sc, 0, sizeof sc);
    sc.argc = 4;
    sc.argv = av;
    sc.argv_cap = 5;
    int rc = shell_run_function(&sc);
    if (rc == 127) *found = false;
    comp_unbind_vars();
    /* An error that discards the line (an expansion error in the function)
     * leaves the completion too: bash's jump_to_top_level never comes back
     * to compgen, so nothing is listed. */
    if (g_shell_flow == SHELL_FLOW_DISCARD || g_shell_flow == SHELL_FLOW_EXIT ||
        g_shell_flow == SHELL_FLOW_INTERRUPT) {
        *found = false;
        sh_array_unset("COMPREPLY");
        int ix = env_find("COMPREPLY", 9);
        if (ix >= 0) env_remove_at(ix);
        return;
    }
    struct sh_array *a = sh_array_find("COMPREPLY");
    if (!a) {
        const char *s = env_get("COMPREPLY");
        int ix = env_find("COMPREPLY", 9);
        if (s && ix >= 0 && !(g_env_flags[ix] & SHVAR_NOVALUE) && *found)
            (void)sv_add(ret, s);
    } else if (*found && !a->assoc) {
        for (int i = 0; i < a->n; i++) (void)sv_add(ret, a->e[i].val);
    }
    sh_array_unset("COMPREPLY");
    {
        int ix = env_find("COMPREPLY", 9);
        if (ix >= 0) env_remove_at(ix);
    }
}

/* Quote a word in single quotes, as bash's sh_single_quote. */
static void comp_single_quote(struct shell_sb *b, const char *s) {
    (void)sb_putc(b, '\'');
    for (; *s; s++) {
        if (*s == '\'') (void)sb_puts(b, "'\\''");
        else (void)sb_putc(b, *s);
    }
    (void)sb_putc(b, '\'');
}

/* -C: `COMMAND 'cmd' 'word' 'prev'` as a command substitution; its output
 * a match a line (a backslash-newline continues one). */
static void comp_command(struct sh_strv *ret, const char *command, const char *cmd,
                         const char *text, const char *line, int point,
                         const struct sh_strv *words, int cword) {
    comp_bind_vars(line, point, words, cword, true);
    const char *prev = (words && cword >= 2 && cword - 2 < words->n) ? words->v[cword - 2] : "";
    SH_SB(c);
    (void)sb_puts(&c, command);
    (void)sb_putc(&c, ' ');
    comp_single_quote(&c, cmd);
    (void)sb_putc(&c, ' ');
    comp_single_quote(&c, text);
    (void)sb_putc(&c, ' ');
    comp_single_quote(&c, prev);
    SH_SB(outb);
    (void)shell_capture_command(c.p, &outb);
    comp_unbind_vars();
    while (outb.len && outb.p[outb.len - 1] == '\n') sb_truncate(&outb, outb.len - 1);
    const char *p = outb.p;
    while (*p) {
        const char *ws = p;
        while (*p && *p != '\n') {
            if (*p == '\\' && p[1] == '\n') p++;
            p++;
        }
        (void)sv_add_n(ret, ws, (size_t)(p - ws));
        while (*p == '\n') p++;
    }
}

/* -X: remove the matches of PAT (a leading `!` keeps them instead); `&`
 * in PAT stands for the word, `\&` for a literal `&`. Matched as extended
 * patterns, case-insensitively under nocasematch. */
static void comp_filter(struct sh_strv *ret, const char *pat, const char *text) {
    SH_SB(np);
    for (const char *p = pat; *p; p++) {
        if (*p == '&') {
            for (const char *t = text; *t; t++) {
                if (*t == '*' || *t == '?' || *t == '[' || *t == ']' || *t == '\\' ||
                    *t == '(' || *t == ')' || *t == '|' || *t == '!' || *t == '@' || *t == '+')
                    (void)sb_putc(&np, '\\');
                (void)sb_putc(&np, *t);
            }
            continue;
        }
        if (*p == '\\' && p[1] == '&') p++;
        (void)sb_putc(&np, *p);
    }
    bool neg = np.p[0] == '!' && (!g_shopt_extglob || np.p[1] != '(');
    const char *t = neg ? np.p + 1 : np.p;
    bool saved_nocase = g_glob_nocase;
    g_glob_nocase = g_shopt_misc[3];
    int k = 0;
    for (int i = 0; i < ret->n; i++) {
        bool m = shell_glob_match(t, ret->v[i]);
        bool drop = neg ? !m : m;
        if (drop) kfree(ret->v[i]);
        else ret->v[k++] = ret->v[i];
    }
    ret->n = k;
    if (ret->v) ret->v[k] = 0;
    g_glob_nocase = saved_nocase;
}

/* gen_compspec_completions. `found` comes back false when -F's function
 * was not found (127), which discards everything, as bash does. */
static void comp_generate(const struct compspec *cs, const char *cmd,
                          const char *text, const char *line, int point,
                          const struct sh_strv *words, int cword,
                          struct sh_strv *ret) {
    comp_gen_actions(ret, cs->actions, text);
    if (cs->globpat) comp_globpat(ret, cs->globpat);
    if (cs->words) comp_wordlist(ret, cs->words, text);
    bool found = true;
    if (cs->funcname)
        comp_function(ret, cs->funcname, cmd, text, line, point, words, cword, &found);
    if (cs->command)
        comp_command(ret, cs->command, cmd, text, line, point, words, cword);
    if (!found) { sv_free(ret); return; }
    if (cs->filterpat && ret->n) comp_filter(ret, cs->filterpat, text);
    if ((cs->prefix || cs->suffix) && ret->n) {
        for (int i = 0; i < ret->n; i++) {
            SH_SB(d);
            (void)sb_puts(&d, cs->prefix);
            (void)sb_puts(&d, ret->v[i]);
            (void)sb_puts(&d, cs->suffix);
            char *nv = sb_steal(&d);
            if (nv) { kfree(ret->v[i]); ret->v[i] = nv; }
        }
    }
    if (ret->n == 0 && (cs->options & COPT_DIRNAMES))
        comp_directories(ret, text);
    else if (cs->options & COPT_PLUSDIRS)
        comp_directories(ret, text);
}

/* ---- printing ----------------------------------------------------------- */

static bool comp_has_shell_metas(const char *s) { return sh_contains_shell_metas(s); }

static void comp_print_name(struct shell_sb *b, const char *cmd) {
    if (strcmp(cmd, COMP_DEFAULTCMD) == 0) (void)sb_puts(b, "-D");
    else if (strcmp(cmd, COMP_EMPTYCMD) == 0) (void)sb_puts(b, "-E");
    else if (strcmp(cmd, COMP_INITIALWORD) == 0) (void)sb_puts(b, "-I");
    else if (!*cmd) (void)sb_puts(b, "''");
    else if (comp_has_shell_metas(cmd)) comp_single_quote(b, cmd);
    else (void)sb_puts(b, cmd);
}

static void comp_print_one(const char *cmd, const struct compspec *cs) {
    SH_SB(b);
    (void)sb_puts(&b, "complete ");
    for (int i = 0; g_compopts[i].name; i++)
        if (cs->options & g_compopts[i].flag) {
            (void)sb_puts(&b, "-o ");
            (void)sb_puts(&b, g_compopts[i].name);
            (void)sb_putc(&b, ' ');
        }
    for (int i = 0; g_compacts[i].name; i++)
        if (g_compacts[i].opt && (cs->actions & g_compacts[i].flag)) {
            (void)sb_putc(&b, '-');
            (void)sb_putc(&b, g_compacts[i].opt);
            (void)sb_putc(&b, ' ');
        }
    for (int i = 0; g_compacts[i].name; i++)
        if (!g_compacts[i].opt && (cs->actions & g_compacts[i].flag)) {
            (void)sb_puts(&b, "-A ");
            (void)sb_puts(&b, g_compacts[i].name);
            (void)sb_putc(&b, ' ');
        }
    const struct { const char *v; const char *flag; } quoted[] = {
        { cs->globpat, "-G" }, { cs->words, "-W" }, { cs->prefix, "-P" },
        { cs->suffix, "-S" }, { cs->filterpat, "-X" }, { cs->command, "-C" },
    };
    for (size_t i = 0; i < sizeof quoted / sizeof quoted[0]; i++) {
        if (!quoted[i].v) continue;
        (void)sb_puts(&b, quoted[i].flag);
        (void)sb_putc(&b, ' ');
        comp_single_quote(&b, quoted[i].v);
        (void)sb_putc(&b, ' ');
    }
    if (cs->funcname) {
        (void)sb_puts(&b, "-F ");
        if (comp_has_shell_metas(cs->funcname)) comp_single_quote(&b, cs->funcname);
        else (void)sb_puts(&b, cs->funcname);
        (void)sb_putc(&b, ' ');
    }
    comp_print_name(&b, cmd);
    shell_printf("%s\n", b.p);
}

static void comp_print_compopt(const char *cmd, const struct compspec *cs) {
    SH_SB(b);
    (void)sb_puts(&b, "compopt ");
    for (int i = 0; g_compopts[i].name; i++) {
        (void)sb_puts(&b, (cs->options & g_compopts[i].flag) ? "-o " : "+o ");
        (void)sb_puts(&b, g_compopts[i].name);
        (void)sb_putc(&b, ' ');
    }
    comp_print_name(&b, cmd);
    shell_printf("%s\n", b.p);
}

/* ---- the builtins ------------------------------------------------------- */

/* complete -F's check, bash's check_identifier outside posix mode: a
 * function name that is not all digits and holds none of the characters
 * that break a shell word. */
static bool shell_function_name_ok(const char *s) {
    if (!s || !*s) return false;
    bool digits = true;
    for (const char *p = s; *p; p++) {
        if (comp_strchr("()<>;&| \t\n", *p)) return false;
        if (*p < '0' || *p > '9') digits = false;
    }
    return !digits;
}

/* `NAME --help`: bash's builtin_help -- the synopsis line, then the
 * description, indented. */
static void shell_help_topic(const char *name) {
    for (int i = 0; g_bash_topics[i].name; i++) {
        if (strcmp(g_bash_topics[i].name, name) != 0) continue;
        shell_printf("%s: %s\n", name, g_bash_topics[i].syn);
        for (const struct cmd *c = cmds; c->name; c++)
            if (strcmp(c->name, name) == 0) {
                shell_printf("    %s\n", c->help);
                break;
            }
        return;
    }
}

struct comp_args {
    unsigned long acts, copts;
    bool pflag, rflag, Dflag, Eflag, Iflag;
    const char *G, *W, *P, *S, *X, *F, *C;
};

/* build_actions: 0 options given, 1 none, 2 a usage error (reported). */
static int comp_build_actions(struct sh_igo *g, struct comp_args *a, bool flags_ok) {
    memset(a, 0, sizeof *a);
    bool given = false;
    int c;
    while ((c = sh_igo_next(g, "abcdefgjko:prsuvA:G:W:P:S:X:F:C:DEI")) != -1) {
        given = true;
        switch (c) {
        case 'r': case 'p': case 'D': case 'E': case 'I':
            if (!flags_ok) {
                kprintf("%s: -%c: invalid option\n", g->name, c);
                sh_builtin_usage(g->name);
                return 2;
            }
            if (c == 'r') a->rflag = true;
            else if (c == 'p') a->pflag = true;
            else if (c == 'D') a->Dflag = true;
            else if (c == 'E') a->Eflag = true;
            else a->Iflag = true;
            break;
        case 'a': a->acts |= CA_ALIAS; break;
        case 'b': a->acts |= CA_BUILTIN; break;
        case 'c': a->acts |= CA_COMMAND; break;
        case 'd': a->acts |= CA_DIRECTORY; break;
        case 'e': a->acts |= CA_EXPORT; break;
        case 'f': a->acts |= CA_FILE; break;
        case 'g': a->acts |= CA_GROUP; break;
        case 'j': a->acts |= CA_JOB; break;
        case 'k': a->acts |= CA_KEYWORD; break;
        case 's': a->acts |= CA_SERVICE; break;
        case 'u': a->acts |= CA_USER; break;
        case 'v': a->acts |= CA_VARIABLE; break;
        case 'o': {
            int k = comp_find_opt(g->optarg);
            if (k < 0) {
                kprintf("%s: %s: invalid option name\n", g->name, g->optarg);
                return 2;
            }
            a->copts |= g_compopts[k].flag;
            break;
        }
        case 'A': {
            int k = comp_find_act(g->optarg);
            if (k < 0) {
                kprintf("%s: %s: invalid action name\n", g->name, g->optarg);
                return 2;
            }
            a->acts |= g_compacts[k].flag;
            break;
        }
        case 'C': a->C = g->optarg; break;
        case 'F':
            if (!shell_function_name_ok(g->optarg)) {
                kprintf("%s: `%s': not a valid identifier\n", g->name, g->optarg);
                return 2;
            }
            a->F = g->optarg;
            break;
        case 'G': a->G = g->optarg; break;
        case 'P': a->P = g->optarg; break;
        case 'S': a->S = g->optarg; break;
        case 'W': a->W = g->optarg; break;
        case 'X': a->X = g->optarg; break;
        case SH_IGO_HELP:
            shell_help_topic(g->name);
            return 2;
        default:
            sh_builtin_usage(g->name);
            return 2;
        }
    }
    return given ? 0 : 1;
}

static struct compspec *comp_spec_from(const struct comp_args *a) {
    struct compspec *cs = (struct compspec *)kmalloc(sizeof *cs);
    if (!cs) return 0;
    memset(cs, 0, sizeof *cs);
    cs->actions = a->acts;
    cs->options = a->copts;
    cs->globpat = a->G ? shell_strdup(a->G) : 0;
    cs->words = a->W ? shell_strdup(a->W) : 0;
    cs->prefix = a->P ? shell_strdup(a->P) : 0;
    cs->suffix = a->S ? shell_strdup(a->S) : 0;
    cs->funcname = a->F ? shell_strdup(a->F) : 0;
    cs->command = a->C ? shell_strdup(a->C) : 0;
    cs->filterpat = a->X ? shell_strdup(a->X) : 0;
    return cs;
}

static void cmd_complete(int argc, char **argv) {
    if (argc < 2) {
        for (int i = 0; i < g_progcomp.n; i++)
            comp_print_one(g_progcomp.e[i].key, (struct compspec *)g_progcomp.e[i].data);
        shell_set_status(0);
        return;
    }
    struct sh_igo g;
    sh_igo_init(&g, "complete", argc, argv);
    struct comp_args a;
    int r = comp_build_actions(&g, &a, true);
    if (r == 2) { shell_set_status(2); return; }
    bool given = r == 0;
    int first = g.ind;
    const char *pseudo = a.Dflag ? COMP_DEFAULTCMD : a.Eflag ? COMP_EMPTYCMD :
                         a.Iflag ? COMP_INITIALWORD : 0;
    char *pv[2] = { (char *)pseudo, 0 };
    char **names = pseudo ? pv : argv + first;
    int nnames = pseudo ? 1 : argc - first;

    if (a.pflag || (first >= argc && !given)) {
        if (!pseudo && first >= argc) {
            for (int i = 0; i < g_progcomp.n; i++)
                comp_print_one(g_progcomp.e[i].key, (struct compspec *)g_progcomp.e[i].data);
            shell_set_status(0);
            return;
        }
        int rc = 0;
        for (int i = 0; i < nnames; i++) {
            struct compspec *cs = compspec_search(names[i]);
            if (cs) comp_print_one(names[i], cs);
            else { kprintf("complete: %s: no completion specification\n", names[i]); rc = 1; }
        }
        shell_set_status(rc);
        return;
    }
    if (a.rflag) {
        if (!pseudo && first >= argc) { progcomp_flush(); shell_set_status(0); return; }
        int rc = 0;
        for (int i = 0; i < nnames; i++)
            if (!progcomp_remove(names[i])) {
                kprintf("complete: %s: no completion specification\n", names[i]);
                rc = 1;
            }
        shell_set_status(rc);
        return;
    }
    if (!pseudo && first >= argc && given) {
        sh_builtin_usage("complete");
        shell_set_status(2);
        return;
    }
    struct compspec *cs = comp_spec_from(&a);
    if (!cs) { shell_set_status(1); return; }
    int rc = 0;
    cs->refcount = 1;                   /* held while inserting */
    for (int i = 0; i < nnames; i++)
        if (!progcomp_insert(names[i], cs)) rc = 1;
    compspec_unref(cs);
    shell_set_status(rc);
}

/* Print each match a line; 0 when there was one. */
static int comp_print_matches(const struct sh_strv *m) {
    for (int i = 0; i < m->n; i++) shell_printf("%s\n", m->v[i]);
    return m->n > 0 ? 0 : 1;
}

static void comp_bash_default(struct sh_strv *ret, const char *word);

static void cmd_compgen(int argc, char **argv) {
    if (argc < 2) { shell_set_status(0); return; }
    struct sh_igo g;
    sh_igo_init(&g, "compgen", argc, argv);
    struct comp_args a;
    int r = comp_build_actions(&g, &a, false);
    if (r == 2) { shell_set_status(2); return; }
    if (r == 1) { shell_set_status(0); return; }
    const char *word = g.ind < argc ? argv[g.ind] : "";
    if (a.F) kprintf("compgen: warning: -F option may not work as you expect\n");
    if (a.C) kprintf("compgen: warning: -C option may not work as you expect\n");
    struct compspec *cs = comp_spec_from(&a);
    if (!cs) { shell_set_status(1); return; }
    cs->refcount = 1;
    struct sh_strv m = { 0, 0, 0 };
    comp_generate(cs, "compgen", word, "", 0, 0, 0, &m);
    if (g_shell_flow == SHELL_FLOW_DISCARD || g_shell_flow == SHELL_FLOW_EXIT ||
        g_shell_flow == SHELL_FLOW_INTERRUPT) {
        sv_free(&m);
        compspec_unref(cs);
        return;                 /* the status the error left */
    }
    if (m.n == 0 && (a.copts & COPT_BASHDEFAULT)) comp_bash_default(&m, word);
    if (m.n == 0 && (a.copts & COPT_DEFAULT)) comp_filenames(&m, word);
    shell_set_status(comp_print_matches(&m));
    sv_free(&m);
    compspec_unref(cs);
}

static void cmd_compopt(int argc, char **argv) {
    struct sh_igo g;
    sh_igo_init(&g, "compopt", argc, argv);
    unsigned long on = 0, off = 0;
    bool Dflag = false, Eflag = false, Iflag = false;
    int c;
    while ((c = sh_igo_next(&g, "+o:DEI")) != -1) {
        switch (c) {
        case 'o': {
            int k = comp_find_opt(g.optarg);
            if (k < 0) {
                kprintf("compopt: %s: invalid option name\n", g.optarg);
                shell_set_status(2);
                return;
            }
            if (g.opttype == '-') on |= g_compopts[k].flag;
            else off |= g_compopts[k].flag;
            break;
        }
        case 'D': Dflag = true; break;
        case 'E': Eflag = true; break;
        case 'I': Iflag = true; break;
        case SH_IGO_HELP:
            shell_help_topic("compopt");
            shell_set_status(2);
            return;
        default:
            sh_builtin_usage("compopt");
            shell_set_status(2);
            return;
        }
    }
    const char *pseudo = Dflag ? COMP_DEFAULTCMD : Eflag ? COMP_EMPTYCMD :
                         Iflag ? COMP_INITIALWORD : 0;
    if (g.ind >= argc && !pseudo) {
        if (!g_pcomp_completing || !g_pcomp_curcs) {
            kprintf("compopt: not currently executing completion function\n");
            shell_set_status(1);
            return;
        }
        if (!on && !off) comp_print_compopt(g_pcomp_curcmd ? g_pcomp_curcmd : "", g_pcomp_curcs);
        g_pcomp_curcs->options |= on;
        g_pcomp_curcs->options &= ~off;
        shell_set_status(0);
        return;
    }
    char *pv[2] = { (char *)pseudo, 0 };
    char **names = pseudo ? pv : argv + g.ind;
    int nnames = pseudo ? 1 : argc - g.ind;
    int rc = 0;
    for (int i = 0; i < nnames; i++) {
        struct compspec *cs = compspec_search(names[i]);
        if (!cs) {
            kprintf("compopt: %s: no completion specification\n", names[i]);
            rc = 1;
            continue;
        }
        if (!on && !off) { comp_print_compopt(names[i], cs); continue; }
        cs->options |= on;
        cs->options &= ~off;
    }
    shell_set_status(rc);
}

/* bash_default_completion for compgen -o bashdefault: a word beginning
 * with `$` completes variable names (the `$` kept), with `~` user names,
 * with `@` host names; otherwise command names when it has no slash, and
 * file names when nothing else matched. */
static void comp_bash_default(struct sh_strv *ret, const char *word) {
    if (word[0] == '$' && word[1] != '(') {
        bool brace = word[1] == '{';
        const char *v = word + 1 + (brace ? 1 : 0);
        struct sh_strv all = { 0, 0, 0 }, m = { 0, 0, 0 };
        comp_collect_vars(&all, false, false);
        comp_prefix_filter(&m, &all, v);
        for (int i = 0; i < m.n; i++) {
            SH_SB(r);
            (void)sb_puts(&r, brace ? "${" : "$");
            (void)sb_puts(&r, m.v[i]);
            if (brace) (void)sb_putc(&r, '}');
            (void)sv_add(ret, r.p);
        }
        sv_free(&all);
        sv_free(&m);
        return;
    }
    if (word[0] == '~' && !comp_strchr(word, '/')) { comp_users(ret, word); return; }
    if (word[0] == '@') {
        struct sh_strv all = { 0, 0, 0 };
        const char *hf = env_get("HOSTFILE");
        comp_collect_field_file(&all, hf && *hf ? hf : "/etc/hosts", true);
        size_t tl = strlen(word + 1);
        for (int i = 0; i < all.n; i++)
            if (strncmp(all.v[i], word + 1, tl) == 0) {
                SH_SB(r);
                (void)sb_putc(&r, '@');
                (void)sb_puts(&r, all.v[i]);
                (void)sv_add(ret, r.p);
            }
        sv_free(&all);
        return;
    }
    comp_filenames(ret, word);
}

/* ---- the line editor's Tab ----------------------------------------------
 * The compspec for the command whose argument is being completed (bash's
 * progcomp_search: the full word, then its basename, then -D's default),
 * run as an interactive completion is run: COMP_LINE the line up to the
 * cursor's end, COMP_POINT the cursor, COMP_WORDS the line split at
 * readline's word breaks, COMP_CWORD the word being completed. Returns the
 * matches in *out and true when a compspec applied; false sends the editor
 * to its own command/file completion. -o default / bashdefault fall back to
 * filenames when the spec produced nothing. */
bool tsh_progcomp(const char *line, size_t point, const char *word,
                  char ***out, int *nout, unsigned long *opts) {
    *out = 0;
    *nout = 0;
    if (opts) *opts = 0;
    /* COMP_WORDS: readline's breaks (" \t\n\"'@><=;|&(:"), quotes kept. */
    struct sh_strv words = { 0, 0, 0 };
    int cword = 0;
    {
        const char *breaks = env_get("COMP_WORDBREAKS");
        if (!breaks) breaks = " \t\n\"'><=;|&(:";
        size_t i = 0, len = strlen(line);
        int wn = 0;
        bool placed = false;
        while (i < len) {
            while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\n')) {
                if (!placed && i == point) { (void)sv_add(&words, ""); cword = wn++; placed = true; }
                i++;
            }
            if (i >= len) break;
            size_t ws = i;
            if (comp_strchr(breaks, line[i]) && line[i] != '"' && line[i] != '\'') {
                while (i < len && comp_strchr(breaks, line[i]) && line[i] != ' ' &&
                       line[i] != '\t' && line[i] != '\n' && line[i] != '"' && line[i] != '\'')
                    i++;
            } else {
                while (i < len && !(comp_strchr(breaks, line[i]) && line[i] != '"' && line[i] != '\'')) {
                    if (line[i] == '\\' && i + 1 < len) { i += 2; continue; }
                    if (line[i] == '\'' || line[i] == '"') {
                        char q = line[i++];
                        while (i < len && line[i] != q) i++;
                        if (i < len) i++;
                        continue;
                    }
                    i++;
                }
            }
            (void)sv_add_n(&words, line + ws, i - ws);
            if (!placed && point >= ws && point <= i) { cword = wn; placed = true; }
            wn++;
        }
        if (!placed) { (void)sv_add(&words, ""); cword = wn; }
    }
    if (words.n == 0) { sv_free(&words); return false; }
    const char *cmd = words.v[0];
    struct compspec *cs = 0;
    const char *found_name = cmd;
    if (cword == 0) {
        cs = compspec_search(COMP_INITIALWORD);
        found_name = COMP_INITIALWORD;
    } else {
        cs = compspec_search(cmd);
        if (!cs) {
            const char *base = comp_strrchr(cmd, '/');
            if (base && base[1]) { cs = compspec_search(base + 1); found_name = base + 1; }
        }
        if (!cs) { cs = compspec_search(COMP_DEFAULTCMD); found_name = COMP_DEFAULTCMD; }
    }
    if (!cs) { sv_free(&words); return false; }
    cs->refcount++;
    struct compspec *saved_cs = g_pcomp_curcs;
    const char *saved_cmd = g_pcomp_curcmd;
    bool saved_comp = g_pcomp_completing;
    g_pcomp_curcs = cs;
    g_pcomp_curcmd = found_name;
    g_pcomp_completing = true;
    SH_SB(lb);
    (void)sb_puts(&lb, line);
    struct sh_strv m = { 0, 0, 0 };
    comp_generate(cs, cmd, word, lb.p, (int)point, &words, cword + 1, &m);
    if (m.n == 0 && (cs->options & COPT_BASHDEFAULT)) comp_bash_default(&m, word);
    if (m.n == 0 && (cs->options & COPT_DEFAULT)) comp_filenames(&m, word);
    if (opts) *opts = cs->options;
    g_pcomp_curcs = saved_cs;
    g_pcomp_curcmd = saved_cmd;
    g_pcomp_completing = saved_comp;
    compspec_unref(cs);
    sv_free(&words);
    *out = m.v;
    *nout = m.n;
    return true;
}

void tsh_progcomp_free(char **v, int n) {
    for (int i = 0; i < n; i++) kfree(v[i]);
    kfree(v);
}
