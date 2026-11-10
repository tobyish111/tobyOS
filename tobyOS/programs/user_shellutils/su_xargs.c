/* su_xargs.c -- xargs, as GNU findutils' 4.10 (logs/utilgate/xargs.sh):
 * run COMMAND (echo) with INITIAL-ARGS and the items read from standard
 * input (or -a FILE), as many commands as it takes.
 *
 *   - ITEMS are blank- or newline-separated; '...' and "..." quote (no
 *     newline inside), a backslash escapes the next character, a newline
 *     included; an unclosed quote is "unmatched single quote; by default
 *     quotes are special to xargs unless you use the -0 option", status 1.
 *     -0 or -d DELIM (one character, or \n \t \xHH \OOO ...) take the
 *     input literally, empty items kept. -E STR (-e[STR]) ends the input
 *     at an item equal to STR.
 *   - A command gets at most -n items, -L non-blank lines (a line ending
 *     in a blank goes on into the next; -l is -L 1), -s bytes in all
 *     (every word and its NUL, the command's own included; 128 KiB by
 *     default); -x exits rather than split. -I R runs one command per
 *     line, R replaced in every INITIAL-ARG, leading blanks dropped (-i is
 *     -I {}); -I, -n and -L override each other with GNU's warnings.
 *   - Without -r an empty input still runs COMMAND once. -t prints each
 *     command on stderr first (shell-quoted where needed); -p asks on the
 *     terminal; -P N runs N at a time (0: no limit), --process-slot-var
 *     naming each one's slot; the command's stdin is /dev/null unless -a
 *     or -o (the terminal).
 *   - Status: 123 if any command exited 1-125; 124 when one exits 255
 *     ("exited with status 255; aborting"), 125 when a signal kills one
 *     ("terminated by signal N"), 126 when it cannot run, 127 when it is
 *     not found -- each of these four at once.
 *
 * It kept no quoting, took "-n2" as the command (so `xargs -n2 echo` ran
 * nothing, silently), took -0 and -t and -r as nothing, had no -L -I -d
 * -E -a -s -x -P, and said only 0 or the last command's status. */

#define XA_DEFAULT_SIZE (128 * 1024)

struct xa {
    /* options */
    int nul, delim, have_delim, max_args, max_lines, replace, trace, prompt;
    int norun_empty, exit_size, max_procs, open_tty, from_file;
    long max_size;
    const char *eof_str, *repl, *slot_var;
    /* the command */
    char **init;
    int ninit;
    size_t init_size;
    /* the current command's items */
    char **items;
    int nitems, cap;
    size_t size;
    int lines;
    /* children */
    pid_t *kids;
    int nkids;
    int status;              /* 0, or 123 */
    int ran;                 /* a command has been started */
    FILE *in;
};

#define XA_MAXPROCS 64

static void xa_forget(struct xa *x, pid_t p) {
    for (int i = 0; i < XA_MAXPROCS; i++)
        if (x->kids[i] == p) { x->kids[i] = 0; break; }
    x->nkids--;
}

static void xa_die_status(struct xa *x, int code) {
    /* the four immediate statuses: wait for the rest first, as GNU does */
    while (x->nkids > 0) {
        int st;
        pid_t p = waitpid(-1, &st, 0);
        if (p < 0) break;
        xa_forget(x, p);
    }
    exit(code);
}

/* the exit of one child */
static void xa_reaped(struct xa *x, const char *name, int st) {
    if (WIFSIGNALED(st)) {
        su_error(0, 0, "%s: terminated by signal %d", name, WTERMSIG(st));
        xa_die_status(x, 125);
    }
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 1;
    if (code == 255) {
        su_error(0, 0, "%s: exited with status 255; aborting", name);
        xa_die_status(x, 124);
    }
    if (code == 126 || code == 127) {
        /* the child's own report of a failed exec */
        xa_die_status(x, code);
    }
    if (code) x->status = 123;
}

static void xa_wait_one(struct xa *x) {
    int st;
    pid_t p = waitpid(-1, &st, 0);
    if (p < 0) { x->nkids = 0; return; }
    xa_forget(x, p);
    xa_reaped(x, x->init[0], st);
}

/* what execvp would say of NAME before it is run: 0, ENOENT or EACCES.
 * The native ABI has no close-on-exec to carry exec's errno back from the
 * child as GNU's does, so it is asked first -- and a command's own exit
 * 126 or 127 still counts as any other failing command (123). */
static int xa_exec_errno(const char *name) {
    struct stat st;
    if (strchr(name, '/')) {
        if (stat(name, &st) != 0) return errno;
        if (!S_ISREG(st.st_mode) || !(st.st_mode & 0111)) return EACCES;
        return 0;
    }
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    int seen_acces = 0;
    for (const char *p = path;;) {
        const char *e = strchr(p, ':');
        size_t dl = e ? (size_t)(e - p) : strlen(p);
        char *cand = (char *)malloc(dl + strlen(name) + 3);
        if (!cand) su_error(1, ENOMEM, "memory exhausted");
        if (dl == 0) sprintf(cand, "./%s", name);
        else sprintf(cand, "%.*s/%s", (int)dl, p, name);
        int ok = stat(cand, &st) == 0;
        free(cand);
        if (ok) {
            if (S_ISREG(st.st_mode) && (st.st_mode & 0111)) return 0;
            seen_acces = 1;
        }
        if (!e) break;
        p = e + 1;
    }
    return seen_acces ? EACCES : ENOENT;
}

static char *xa_replace(const char *arg, const char *r, const char *with) {
    size_t rl = strlen(r), wl = strlen(with), n = 0, cap = strlen(arg) + 1;
    for (const char *p = arg; rl && (p = strstr(p, r)); p += rl) cap += wl;
    char *out = (char *)malloc(cap);
    if (!out) su_error(1, ENOMEM, "memory exhausted");
    const char *p = arg;
    while (*p) {
        if (rl && strncmp(p, r, rl) == 0) {
            memcpy(out + n, with, wl);
            n += wl;
            p += rl;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return out;
}

/* run the command with INITIAL-ARGS and the items gathered (or, with -I,
 * ITEM replaced into them) */
static void xa_run(struct xa *x, const char *item) {
    int argn = x->ninit + (item ? 0 : x->nitems);
    char **av = (char **)malloc((size_t)(argn + 1) * sizeof *av);
    if (!av) su_error(1, ENOMEM, "memory exhausted");
    int k = 0;
    for (int i = 0; i < x->ninit; i++)
        av[k++] = item && i > 0 ? xa_replace(x->init[i], x->repl, item) : x->init[i];
    if (!item) for (int i = 0; i < x->nitems; i++) av[k++] = x->items[i];
    av[k] = 0;
    if (x->trace || x->prompt) {
        fflush(stdout);
        for (int i = 0; i < k; i++)
            fprintf(stderr, "%s%s", i ? " " : "", su_quotef(av[i]));
        if (x->prompt) {
            fputs(" ?...", stderr);
            fflush(stderr);
            FILE *tty = fopen("/dev/tty", "r");
            char ans[64] = "";
            if (!tty || !fgets(ans, sizeof ans, tty)) ans[0] = 0;
            if (tty) fclose(tty);
            if (ans[0] != 'y' && ans[0] != 'Y') goto done;
        } else {
            fputc('\n', stderr);
        }
    }
    {
        int e = xa_exec_errno(av[0]);
        if (e) {
            su_error(0, e, "%s", av[0]);
            xa_die_status(x, e == ENOENT ? 127 : 126);
        }
    }
    while (x->nkids >= x->max_procs) xa_wait_one(x);
    int slot = 0;
    while (slot < XA_MAXPROCS - 1 && x->kids[slot]) slot++;
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) su_error(1, errno, "cannot fork");
    if (pid == 0) {
        if (x->slot_var) {
            char sv[32];
            snprintf(sv, sizeof sv, "%d", slot);
            setenv(x->slot_var, sv, 1);
        }
        if (x->open_tty) {
            int fd = open("/dev/tty", O_RDONLY);
            if (fd < 0) { su_error(0, errno, "failed to open /dev/tty for reading"); _exit(1); }
            dup2(fd, 0);
            if (fd > 0) close(fd);
        } else if (!x->from_file) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd > 0) { dup2(fd, 0); close(fd); }
        }
        execvp(av[0], av);
        int e = errno;
        su_error(0, e, "%s", av[0]);
        _exit(e == ENOENT ? 127 : 126);
    }
    x->ran = 1;
    x->kids[slot] = pid;
    x->nkids++;
    if (x->max_procs == 1) xa_wait_one(x);
done:
    if (item)
        for (int i = 1; i < x->ninit; i++) free(av[i]);
    free(av);
}

/* the gathered items as one command, and start again */
static void xa_flush(struct xa *x) {
    xa_run(x, 0);
    for (int i = 0; i < x->nitems; i++) free(x->items[i]);
    x->nitems = 0;
    x->size = x->init_size;
    x->lines = 0;
}

static void xa_add(struct xa *x, char *w) {
    size_t ws = strlen(w) + 1;
    if (x->init_size + ws > (size_t)x->max_size) {
        free(w);
        su_error(1, 0, "cannot fit single argument within argument list size limit");
    }
    if (x->nitems > 0 && x->size + ws > (size_t)x->max_size) {
        /* -x: a split that breaks -n's or -L's grouping is fatal */
        if (x->exit_size && (x->max_args || x->max_lines))
            su_error(1, 0, "argument list too long");
        xa_flush(x);
    }
    if (x->nitems == x->cap) {
        x->cap = x->cap ? x->cap * 2 : 64;
        x->items = (char **)realloc(x->items, (size_t)x->cap * sizeof *x->items);
        if (!x->items) su_error(1, ENOMEM, "memory exhausted");
    }
    x->items[x->nitems++] = w;
    x->size += ws;
    if (x->max_args && x->nitems >= x->max_args) xa_flush(x);
}

/* a growable word */
struct xa_word { char *s; size_t n, cap; };
static void xa_wput(struct xa_word *w, int c) {
    if (w->n + 2 > w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->s = (char *)realloc(w->s, w->cap);
        if (!w->s) su_error(1, ENOMEM, "memory exhausted");
    }
    w->s[w->n++] = (char)c;
    w->s[w->n] = 0;
}
static char *xa_wtake(struct xa_word *w) {
    char *s = w->s ? w->s : strdup("");
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    w->s = 0;
    w->n = w->cap = 0;
    return s;
}

/* -0 / -d: items end at the delimiter, taken as they are */
static void xa_read_delim(struct xa *x) {
    struct xa_word w = { 0, 0, 0 };
    int c, any = 0;
    while ((c = getc(x->in)) != EOF) {
        any = 1;
        if (c == x->delim) {
            char *s = xa_wtake(&w);
            if (x->eof_str && !x->nul && strcmp(s, x->eof_str) == 0) { free(s); return; }
            if (x->replace) { xa_run(x, s); free(s); }
            else {
                xa_add(x, s);
                if (x->max_lines && ++x->lines >= x->max_lines) xa_flush(x);
            }
            any = 0;
            continue;
        }
        xa_wput(&w, c);
    }
    if (any && w.n) {
        char *s = xa_wtake(&w);
        if (x->replace) { xa_run(x, s); free(s); } else xa_add(x, s);
    } else if (any) {
        char *s = xa_wtake(&w);
        if (x->replace) { xa_run(x, s); free(s); } else xa_add(x, s);
    }
    free(w.s);
}

/* the default reader: blanks, quotes and backslashes */
static void xa_read_words(struct xa *x) {
    struct xa_word w = { 0, 0, 0 };
    int c, inword = 0, quote = 0, lastblank = 0, linehas = 0;
    for (;;) {
        c = getc(x->in);
        if (quote) {
            if (c == EOF || c == '\n') {
                su_error(1, 0, "unmatched %s quote; by default quotes are special to "
                         "xargs unless you use the -0 option",
                         quote == '\'' ? "single" : "double");
            }
            if (c == quote) { quote = 0; continue; }
            xa_wput(&w, c);
            continue;
        }
        if (c == EOF) break;
        int blank = c == ' ' || c == '\t';
        if (c == '\n' || (blank && !x->replace)) {
            if (inword) {
                char *s = xa_wtake(&w);
                inword = 0;
                if (x->eof_str && strcmp(s, x->eof_str) == 0) { free(s); goto end; }
                if (x->replace) { xa_run(x, s); free(s); }
                else xa_add(x, s);
                linehas = 1;
            }
            if (c == '\n') {
                if (linehas && !lastblank && x->max_lines && ++x->lines >= x->max_lines)
                    xa_flush(x);
                if (!lastblank) linehas = 0;
            }
            lastblank = blank;
            continue;
        }
        if (blank && x->replace && !inword) { lastblank = 1; continue; }
        lastblank = blank;
        if (c == '\'' || c == '"') { quote = c; inword = 1; continue; }
        if (c == '\\') {
            c = getc(x->in);
            if (c == EOF) break;
        }
        xa_wput(&w, c);
        inword = 1;
    }
    if (inword) {
        char *s = xa_wtake(&w);
        if (x->eof_str && strcmp(s, x->eof_str) == 0) { free(s); goto end; }
        if (x->replace) { xa_run(x, s); free(s); }
        else xa_add(x, s);
    }
end:
    free(w.s);
}

/* -d's DELIM: one character, or an escape */
static int xa_delim(const char *s) {
    if (s[0] && !s[1]) return (unsigned char)s[0];
    if (s[0] == '\\' && s[1]) {
        const char *p = s + 1;
        if (!p[1]) {
            switch (*p) {
            case 'a': return '\a'; case 'b': return '\b'; case 'f': return '\f';
            case 'n': return '\n'; case 'r': return '\r'; case 't': return '\t';
            case 'v': return '\v'; case '\\': return '\\';
            case '0': return 0;
            }
        }
        if (*p == 'x' && isxdigit((unsigned char)p[1])) {
            char *e;
            long v = strtol(p + 1, &e, 16);
            if (!*e && v <= 255) return (int)v;
        }
        if (*p >= '0' && *p <= '7') {
            char *e;
            long v = strtol(p, &e, 8);
            if (!*e && v <= 255) return (int)v;
        }
        su_error(1, 0, "Invalid escape sequence %s in input delimiter specification.", s);
    }
    su_error(1, 0, "Invalid input delimiter specification %s: the delimiter must be "
             "either a single character or an escape sequence starting with \\.", s);
    return 0;
}

/* GNU's warning when -I, -n and -L meet: the later one wins */
static void xa_excl(const char *prev, const char *now) {
    su_error(0, 0, "warning: options %s and %s are mutually exclusive, "
             "ignoring previous %s value", prev, now, prev);
}

static long xa_num(const char *s, char opt, long min) {
    char *e;
    errno = 0;
    long v = strtol(s, &e, 10);
    if (e == s || *e || errno) {
        su_error(0, 0, "invalid number \"%s\" for -%c option", s, opt);
        su_try_help(1);
    }
    if (v < min) {
        su_error(0, 0, "value %s for -%c option should be >= %ld", s, opt, min);
        su_try_help(1);
    }
    return v;
}

static int do_xargs(int argc, char **argv) {
    enum { SLOT_OPT = 0x100, SHOW_OPT };
    static const struct su_option longs[] = {
        { "null", SU_NO_ARG, 0, '0' },
        { "arg-file", SU_REQ_ARG, 0, 'a' },
        { "delimiter", SU_REQ_ARG, 0, 'd' },
        { "eof", SU_OPT_ARG, 0, 'e' },
        { "replace", SU_OPT_ARG, 0, 'i' },
        { "max-lines", SU_OPT_ARG, 0, 'l' },
        { "max-args", SU_REQ_ARG, 0, 'n' },
        { "open-tty", SU_NO_ARG, 0, 'o' },
        { "interactive", SU_NO_ARG, 0, 'p' },
        { "no-run-if-empty", SU_NO_ARG, 0, 'r' },
        { "max-chars", SU_REQ_ARG, 0, 's' },
        { "verbose", SU_NO_ARG, 0, 't' },
        { "show-limits", SU_NO_ARG, 0, SHOW_OPT },
        { "exit", SU_NO_ARG, 0, 'x' },
        { "max-procs", SU_REQ_ARG, 0, 'P' },
        { "process-slot-var", SU_REQ_ARG, 0, SLOT_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    struct xa x;
    memset(&x, 0, sizeof x);
    x.max_size = XA_DEFAULT_SIZE;
    x.max_procs = 1;
    x.in = stdin;
    int c, show_limits = 0;
    const char *file = 0;
    while ((c = su_getopt_long(argc, argv, "+0a:E:e::i::I:l::L:n:oprs:txP:d:", longs, 0)) != -1) {
        switch (c) {
        case '0': x.nul = 1; x.have_delim = 1; x.delim = 0; break;
        case 'd': x.delim = xa_delim(su_optarg); x.have_delim = 1; x.nul = 0; break;
        case 'a': file = su_optarg; break;
        case 'E': x.eof_str = *su_optarg ? su_optarg : 0; break;
        case 'e': x.eof_str = su_optarg && *su_optarg ? su_optarg : 0; break;
        case 'I': case 'i':
            if (x.max_args) { xa_excl("--max-args", "--replace/-I/-i"); x.max_args = 0; }
            if (x.max_lines) { xa_excl("--max-lines", "--replace/-I/-i"); x.max_lines = 0; }
            x.replace = 1;
            x.repl = c == 'I' ? su_optarg : (su_optarg ? su_optarg : "{}");
            break;
        case 'L': case 'l': {
            const char *now = c == 'L' ? "-L" : "--max-lines/-l";
            if (x.max_args) { xa_excl("--max-args", now); x.max_args = 0; }
            if (x.replace) { xa_excl("--replace", now); x.replace = 0; }
            x.max_lines = c == 'L' ? (int)xa_num(su_optarg, 'L', 1)
                        : su_optarg ? (int)xa_num(su_optarg, 'l', 1) : 1;
            break;
        }
        case 'n': {
            int nv = (int)xa_num(su_optarg, 'n', 1);
            if (x.max_lines) { xa_excl("--max-lines", "--max-args/-n"); x.max_lines = 0; }
            /* -n 1 is what -I does anyway: ignored, said nothing of */
            if (x.replace && nv == 1) break;
            if (x.replace) { xa_excl("--replace", "--max-args/-n"); x.replace = 0; }
            x.max_args = nv;
            break;
        }
        case 's': x.max_size = xa_num(su_optarg, 's', 1); break;
        case 'P': x.max_procs = (int)xa_num(su_optarg, 'P', 0); break;
        case 'o': x.open_tty = 1; break;
        case 'p': x.prompt = 1; x.trace = 1; break;
        case 'r': x.norun_empty = 1; break;
        case 't': x.trace = 1; break;
        case 'x': x.exit_size = 1; break;
        case SLOT_OPT: x.slot_var = su_optarg; break;
        case SHOW_OPT: show_limits = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... COMMAND [INITIAL-ARGS]...\n"
                   "Run COMMAND with INITIAL-ARGS and the arguments read from\n"
                   "standard input, as many times as it takes.\n"
                   "\n"
                   "  -0, --null                 items end with a NUL; quotes and\n"
                   "                             backslashes are not special\n"
                   "  -a, --arg-file=FILE        read the items from FILE\n"
                   "  -d, --delimiter=CHAR       items end with CHAR, taken literally\n"
                   "  -E END                     stop at an item that is END\n"
                   "  -e, --eof[=END]            likewise; no END, no such item\n"
                   "  -I R                       one command per line, R replaced\n"
                   "                             by it in INITIAL-ARGS\n"
                   "  -i, --replace[=R]          likewise, R being {} by default\n"
                   "  -L, --max-lines=MAX        at most MAX input lines a command\n"
                   "  -l[MAX]                    likewise, MAX 1 by default\n"
                   "  -n, --max-args=MAX         at most MAX items a command\n"
                   "  -o, --open-tty             the command reads the terminal\n"
                   "  -P, --max-procs=MAX        run MAX commands at a time\n"
                   "  -p, --interactive          ask before each command\n"
                   "      --process-slot-var=VAR  give each command its slot in VAR\n"
                   "  -r, --no-run-if-empty      nothing to do: run nothing\n"
                   "  -s, --max-chars=MAX        at most MAX bytes a command line\n"
                   "      --show-limits          show the size limits\n"
                   "  -t, --verbose              print each command before it runs\n"
                   "  -x, --exit                 stop if a line would be too long\n"
                   "      --help                 show this help and exit\n"
                   "      --version              show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (x.replace && !*x.repl) x.repl = "{}";
    if (file) {
        x.in = fopen(file, "r");
        if (!x.in) su_error(1, errno, "Cannot open input file %s", su_quoteaf(file));
        x.from_file = 1;
    }
    static char *dflt[] = { (char *)"echo", 0 };
    x.init = su_optind < argc ? argv + su_optind : dflt;
    x.ninit = su_optind < argc ? argc - su_optind : 1;
    for (int i = 0; i < x.ninit; i++) x.init_size += strlen(x.init[i]) + 1;
    x.size = x.init_size;
    if (show_limits) {
        fprintf(stderr, "Your environment variables take up %zu bytes\n", (size_t)0);
        fprintf(stderr, "Maximum length of command we could actually use: %ld\n", x.max_size);
        fprintf(stderr, "Size of command buffer we are actually using: %ld\n", x.max_size);
        fprintf(stderr, "Maximum parallelism (--max-procs must be no greater): %d\n", XA_MAXPROCS);
    }
    if (x.max_procs == 0 || x.max_procs > XA_MAXPROCS) x.max_procs = XA_MAXPROCS;
    x.kids = (pid_t *)calloc(XA_MAXPROCS, sizeof *x.kids);
    if (!x.kids) su_error(1, ENOMEM, "memory exhausted");

    if (x.have_delim) xa_read_delim(&x);
    else xa_read_words(&x);

    if (!x.replace && (x.nitems > 0 || (!x.ran && !x.norun_empty))) xa_flush(&x);
    while (x.nkids > 0) xa_wait_one(&x);
    return x.status;
}
