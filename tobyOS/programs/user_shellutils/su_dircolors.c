/* su_dircolors.c -- dircolors, as GNU coreutils 9.7's
 * (logs/utilgate/staging/dircolors.sh): shell code that sets LS_COLORS,
 * from the built-in database or a FILE ("-" is standard input).
 *
 * THE DATABASE: lines of KEYWORD VALUE. Read as GNU's reads it,
 * measured: blanks (isspace) before the keyword; a line that is empty
 * or starts with '#' after them says nothing; the keyword runs to the
 * next blank ('#' is part of it: "*#" is a pattern); the value is what
 * follows the blanks, up to a '#', its trailing blanks dropped; a NUL
 * ends the line. No value -- "FILE:N: invalid line;  missing second
 * token" (two blanks, GNU's) -- is an error wherever it is, and any
 * error means nothing is printed and the status is 1. Keywords are
 * matched in any case; ones it does not know are passed over in silence
 * (OPTIONS, COLOR and EIGHTBIT are known and mean nothing).
 *
 * TERM and COLORTERM lines take a glob: a run of them opens a section,
 * which applies when any of them matches $TERM or $COLORTERM (an unset
 * or empty TERM is "none", an unset COLORTERM empty); the lines before
 * the first such run always apply.
 * ".EXT" stands for "*.EXT"; "*PATTERN" is kept as it is.
 *
 * OUTPUT: -b "LS_COLORS='KEY=VALUE:...';" and "export LS_COLORS", -c
 * "setenv LS_COLORS '...'", ':' and '=' escaped with a backslash and a
 * quote as '\''; with neither, $SHELL's last component decides (csh or
 * tcsh: -c), and no $SHELL is an error. --print-ls-colors shows each
 * entry in its own colour: ESC[VALUEm KEY TAB VALUE ESC[0m. -p prints
 * the database.
 *
 * THE BUILT-IN DATABASE's entries are GNU's, in GNU's order, so the
 * LS_COLORS made from it is GNU's; its comments are this file's own
 * words (GNU's commentary is not copied), so `dircolors -p` differs from
 * GNU's in its comment lines only. The old dircolors
 * (programs/user_coreutil) printed a fixed line and read no file. */

#include "su_sm_common.c"   /* sm_quotef, sm_quote: GNU's quoting */

static const char *const sm_dc_db[] = {
    "# The database dircolors reads when given no FILE, printed by",
    "# dircolors --print-database. Each line is a KEYWORD and a VALUE;",
    "# '#' starts a comment.",
    "#",
    "# TERM and COLORTERM lines hold a glob. Where one or more of them",
    "# stand together, the lines after them count only if one matches",
    "# the TERM (or COLORTERM) environment variable.",
    "COLORTERM ?*",
    "TERM Eterm",
    "TERM ansi",
    "TERM *color*",
    "TERM con[0-9]*x[0-9]*",
    "TERM cons25",
    "TERM console",
    "TERM cygwin",
    "TERM *direct*",
    "TERM dtterm",
    "TERM gnome",
    "TERM hurd",
    "TERM jfbterm",
    "TERM konsole",
    "TERM kterm",
    "TERM linux",
    "TERM linux-c",
    "TERM mlterm",
    "TERM putty",
    "TERM rxvt*",
    "TERM screen*",
    "TERM st",
    "TERM terminator",
    "TERM tmux*",
    "TERM vt100",
    "TERM vt220",
    "TERM xterm*",
    "#",
    "# Kinds of file. A VALUE is an SGR sequence's parameters: 00 none,",
    "# 01 bold, 04 underline, 05 blink, 07 reverse, 08 hidden; 30-37 the",
    "# text colour and 40-47 the background (black red green yellow blue",
    "# magenta cyan white).",
    "#NORMAL 00",
    "#FILE 00",
    "RESET 0",
    "DIR 01;34",
    "LINK 01;36 # 'target' colours a link as what it points to",
    "MULTIHARDLINK 00",
    "FIFO 40;33",
    "SOCK 01;35",
    "DOOR 01;35",
    "BLK 40;33;01",
    "CHR 40;33;01",
    "ORPHAN 40;31;01",
    "MISSING 00",
    "SETUID 37;41",
    "SETGID 30;43",
    "CAPABILITY 00",
    "STICKY_OTHER_WRITABLE 30;42",
    "OTHER_WRITABLE 34;42",
    "STICKY 37;44",
    "# executable regular files",
    "EXEC 01;32",
    "#",
    "# Names by their ending: .EXT (or *PATTERN) and a VALUE. Endings match",
    "# in either case unless both cases are listed.",
    "#.cmd 01;32",
    "#.exe 01;32",
    "#.com 01;32",
    "#.btm 01;32",
    "#.bat 01;32",
    "#.sh 01;32",
    "#.csh 01;32",
    "# archives",
    ".7z 01;31", ".ace 01;31", ".alz 01;31", ".apk 01;31", ".arc 01;31", ".arj 01;31",
    ".bz 01;31", ".bz2 01;31", ".cab 01;31", ".cpio 01;31", ".crate 01;31", ".deb 01;31",
    ".drpm 01;31", ".dwm 01;31", ".dz 01;31", ".ear 01;31", ".egg 01;31", ".esd 01;31",
    ".gz 01;31", ".jar 01;31", ".lha 01;31", ".lrz 01;31", ".lz 01;31", ".lz4 01;31",
    ".lzh 01;31", ".lzma 01;31", ".lzo 01;31", ".pyz 01;31", ".rar 01;31", ".rpm 01;31",
    ".rz 01;31", ".sar 01;31", ".swm 01;31", ".t7z 01;31", ".tar 01;31", ".taz 01;31",
    ".tbz 01;31", ".tbz2 01;31", ".tgz 01;31", ".tlz 01;31", ".txz 01;31", ".tz 01;31",
    ".tzo 01;31", ".tzst 01;31", ".udeb 01;31", ".war 01;31", ".whl 01;31", ".wim 01;31",
    ".xz 01;31", ".z 01;31", ".zip 01;31", ".zoo 01;31", ".zst 01;31",
    "# pictures and video",
    ".avif 01;35", ".jpg 01;35", ".jpeg 01;35", ".jxl 01;35", ".mjpg 01;35", ".mjpeg 01;35",
    ".gif 01;35", ".bmp 01;35", ".pbm 01;35", ".pgm 01;35", ".ppm 01;35", ".tga 01;35",
    ".xbm 01;35", ".xpm 01;35", ".tif 01;35", ".tiff 01;35", ".png 01;35", ".svg 01;35",
    ".svgz 01;35", ".mng 01;35", ".pcx 01;35", ".mov 01;35", ".mpg 01;35", ".mpeg 01;35",
    ".m2v 01;35", ".mkv 01;35", ".webm 01;35", ".webp 01;35", ".ogm 01;35", ".mp4 01;35",
    ".m4v 01;35", ".mp4v 01;35", ".vob 01;35", ".qt 01;35", ".nuv 01;35", ".wmv 01;35",
    ".asf 01;35", ".rm 01;35", ".rmvb 01;35", ".flc 01;35", ".avi 01;35", ".fli 01;35",
    ".flv 01;35", ".gl 01;35", ".dl 01;35", ".xcf 01;35", ".xwd 01;35", ".yuv 01;35",
    ".cgm 01;35", ".emf 01;35",
    ".ogv 01;35", ".ogx 01;35",
    "# sound",
    ".aac 00;36", ".au 00;36", ".flac 00;36", ".m4a 00;36", ".mid 00;36", ".midi 00;36",
    ".mka 00;36", ".mp3 00;36", ".mpc 00;36", ".ogg 00;36", ".ra 00;36", ".wav 00;36",
    ".oga 00;36", ".opus 00;36", ".spx 00;36", ".xspf 00;36",
    "# leftovers: backups, partial downloads, package managers' copies",
    "*~ 00;90", "*# 00;90", ".bak 00;90", ".crdownload 00;90", ".dpkg-dist 00;90",
    ".dpkg-new 00;90", ".dpkg-old 00;90", ".dpkg-tmp 00;90", ".old 00;90", ".orig 00;90",
    ".part 00;90", ".rej 00;90", ".rpmnew 00;90", ".rpmorig 00;90", ".rpmsave 00;90",
    ".swp 00;90", ".tmp 00;90", ".ucf-dist 00;90", ".ucf-new 00;90", ".ucf-old 00;90",
    "#",
    "# More TERM or COLORTERM lines here would start sections of their own.",
    0
};

/* the keywords, and the two letters ls knows them by */
static const char *const sm_dc_keys[][2] = {
    { "NORMAL", "no" }, { "NORM", "no" }, { "FILE", "fi" }, { "RESET", "rs" },
    { "DIR", "di" }, { "LNK", "ln" }, { "LINK", "ln" }, { "SYMLINK", "ln" },
    { "ORPHAN", "or" }, { "MISSING", "mi" }, { "FIFO", "pi" }, { "PIPE", "pi" },
    { "SOCK", "so" }, { "BLK", "bd" }, { "BLOCK", "bd" }, { "CHR", "cd" },
    { "CHAR", "cd" }, { "DOOR", "do" }, { "EXEC", "ex" }, { "LEFT", "lc" },
    { "LEFTCODE", "lc" }, { "RIGHT", "rc" }, { "RIGHTCODE", "rc" }, { "END", "ec" },
    { "ENDCODE", "ec" }, { "SUID", "su" }, { "SETUID", "su" }, { "SGID", "sg" },
    { "SETGID", "sg" }, { "STICKY", "st" }, { "OTHER_WRITABLE", "ow" }, { "OWR", "ow" },
    { "STICKY_OTHER_WRITABLE", "tw" }, { "OWT", "tw" }, { "CAPABILITY", "ca" },
    { "MULTIHARDLINK", "mh" }, { "CLRTOEOL", "cl" },
    { 0, 0 }
};

struct sm_dc_ent { char *key, *val; };

static struct {
    struct sm_dc_ent *v;
    size_t n, cap;
    int errors;
    int in_term;        /* the last line was TERM or COLORTERM */
    int applies;        /* the lines now read count */
    const char *term, *colorterm;
} sm_dc;

static int sm_dc_space(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static int sm_dc_casecmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        int x = toupper((unsigned char)*a), y = toupper((unsigned char)*b);
        if (x != y) return x - y;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* fnmatch(PAT, S, 0): * ? [...] with ranges, '!' or '^' to negate, and
 * a backslash quoting the next character */
static int sm_dc_match(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (;; s++) {
                if (sm_dc_match(p, s)) return 1;
                if (!*s) return 0;
            }
        }
        if (!*s) return 0;
        if (*p == '?') continue;
        if (*p == '[') {
            const char *q = p + 1;
            int neg = 0, hit = 0;
            if (*q == '!' || *q == '^') { neg = 1; q++; }
            int first = 1;
            while (*q && (first || *q != ']')) {
                first = 0;
                unsigned char lo = (unsigned char)*q, hi;
                if (*q == '\\' && q[1]) lo = (unsigned char)*++q;
                q++;
                hi = lo;
                if (*q == '-' && q[1] && q[1] != ']') {
                    q++;
                    if (*q == '\\' && q[1]) q++;
                    hi = (unsigned char)*q++;
                }
                if (lo <= (unsigned char)*s && (unsigned char)*s <= hi) hit = 1;
            }
            if (*q == ']') {            /* else a '[' that stands for itself */
                if (hit == neg) return 0;
                p = q;
                continue;
            }
        }
        if (*p == '\\' && p[1]) p++;
        if (*p != *s) return 0;
    }
    return !*s;
}

static void sm_dc_add(const char *key, const char *val) {
    if (sm_dc.n == sm_dc.cap) {
        sm_dc.cap = sm_dc.cap ? sm_dc.cap * 2 : 256;
        sm_dc.v = (struct sm_dc_ent *)realloc(sm_dc.v, sm_dc.cap * sizeof *sm_dc.v);
        if (!sm_dc.v) su_error(1, ENOMEM, "memory exhausted");
    }
    sm_dc.v[sm_dc.n].key = strdup(key);
    sm_dc.v[sm_dc.n].val = strdup(val);
    if (!sm_dc.v[sm_dc.n].key || !sm_dc.v[sm_dc.n].val) su_error(1, ENOMEM, "memory exhausted");
    sm_dc.n++;
}

/* one line of the database named NAME (its NUL-free text in LINE, which
 * is changed) */
static void sm_dc_line(char *line, const char *name, long lineno) {
    char *p = line;
    while (sm_dc_space((unsigned char)*p)) p++;
    if (!*p || *p == '#') return;
    char *kw = p;
    while (*p && !sm_dc_space((unsigned char)*p)) p++;
    if (*p) *p++ = 0;
    while (sm_dc_space((unsigned char)*p)) p++;
    char *val = p;
    char *hash = strchr(val, '#');
    if (hash) *hash = 0;
    size_t vl = strlen(val);
    while (vl && sm_dc_space((unsigned char)val[vl - 1])) val[--vl] = 0;
    if (!vl) {
        su_error(0, 0, "%s:%ld: invalid line;  missing second token", sm_quotef(name), lineno);
        sm_dc.errors++;
        return;
    }
    int is_term = !sm_dc_casecmp(kw, "TERM"), is_colorterm = !sm_dc_casecmp(kw, "COLORTERM");
    if (is_term || is_colorterm) {
        if (!sm_dc.in_term) sm_dc.applies = 0;
        sm_dc.in_term = 1;
        if (sm_dc_match(val, is_term ? sm_dc.term : sm_dc.colorterm)) sm_dc.applies = 1;
        return;
    }
    sm_dc.in_term = 0;
    if (!sm_dc.applies) return;
    if (kw[0] == '.') {
        size_t kl = strlen(kw);
        char *k = (char *)malloc(kl + 2);
        if (!k) su_error(1, ENOMEM, "memory exhausted");
        k[0] = '*';
        memcpy(k + 1, kw, kl + 1);
        sm_dc_add(k, val);
        free(k);
        return;
    }
    if (kw[0] == '*') {
        sm_dc_add(kw, val);
        return;
    }
    if (!sm_dc_casecmp(kw, "OPTIONS") || !sm_dc_casecmp(kw, "COLOR") || !sm_dc_casecmp(kw, "EIGHTBIT"))
        return;
    for (int i = 0; sm_dc_keys[i][0]; i++)
        if (!sm_dc_casecmp(kw, sm_dc_keys[i][0])) {
            sm_dc_add(sm_dc_keys[i][1], val);
            return;
        }
    /* a keyword it does not know: GNU's 9.7 says nothing of it */
}

/* the database in file NAME ("-": standard input): 0, or 1 when it
 * could not be read. Read whole -- a database is small -- through
 * su_open_read, which opens a directory as Linux does, so that reading
 * it fails as GNU's does ("read error: Is a directory"). */
static int sm_dc_read_file(const char *name) {
    int fd = strcmp(name, "-") ? su_open_read(name, 0) : 0;
    if (fd < 0) {
        su_error(0, errno, "%s", sm_quotef(name));
        return 1;
    }
    size_t len = 0, cap = 4096;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) su_error(1, ENOMEM, "memory exhausted");
    for (;;) {
        if (len == cap) {
            cap *= 2;
            buf = (char *)realloc(buf, cap + 1);
            if (!buf) su_error(1, ENOMEM, "memory exhausted");
        }
        ssize_t k = read(fd, buf + len, cap - len);
        if (k < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            if (fd > 0) close(fd);
            free(buf);
            su_error(0, e, "%s: read error", sm_quotef(name));
            return 1;
        }
        if (k == 0) break;
        len += (size_t)k;
    }
    if (fd > 0) close(fd);
    buf[len] = 0;
    long lineno = 0;
    /* line by line; a NUL in one ends it there, as a C string */
    for (size_t i = 0; i < len; ) {
        char *nl = (char *)memchr(buf + i, '\n', len - i);
        size_t e = nl ? (size_t)(nl - buf) : len;
        buf[e] = 0;
        sm_dc_line(buf + i, name, ++lineno);
        i = e + 1;
    }
    free(buf);
    return 0;
}

/* S as it stands inside the quotes of the shell code: ':' and '=' with a
 * backslash before them, a quote closed, escaped and reopened */
static void sm_dc_put_quoted(const char *s) {
    for (; *s; s++) {
        if (*s == ':' || *s == '=') { putchar('\\'); putchar(*s); }
        else if (*s == '\'') fputs("'\\''", stdout);
        else putchar(*s);
    }
}

static int do_dircolors(int argc, char **argv) {
    enum { O_PLS = 0x100 };
    static const struct su_option longs[] = {
        { "bourne-shell", SU_NO_ARG, 0, 'b' },
        { "sh", SU_NO_ARG, 0, 'b' },
        { "csh", SU_NO_ARG, 0, 'c' },
        { "c-shell", SU_NO_ARG, 0, 'c' },
        { "print-database", SU_NO_ARG, 0, 'p' },
        { "print-ls-colors", SU_NO_ARG, 0, O_PLS },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int syntax = -1;            /* 0 Bourne, 1 C shell */
    int print_db = 0, print_ls = 0, c;
    while ((c = su_getopt_long(argc, argv, "bcp", longs, 0)) != -1) {
        switch (c) {
        case 'b': syntax = 0; break;
        case 'c': syntax = 1; break;
        case 'p': print_db = 1; break;
        case O_PLS: print_ls = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]\n"
                   "Print shell commands that set LS_COLORS, the colours ls --color uses.\n"
                   "\n"
                   "The kind of output:\n"
                   "  -b, --sh, --bourne-shell    Bourne shell commands\n"
                   "  -c, --csh, --c-shell        C shell commands\n"
                   "  -p, --print-database        the built-in database\n"
                   "      --print-ls-colors       each entry shown in its own colour\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n"
                   "\n"
                   "The colours come from FILE when one is given, else from the built-in\n"
                   "database; --print-database shows that database and its format. With\n"
                   "neither -b nor -c, the SHELL variable decides.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int nops = argc - su_optind;
    if ((print_db || print_ls) && syntax >= 0) {
        su_error(0, 0, "the options to output non shell syntax,\n"
                       "and to select a shell syntax are mutually exclusive");
        su_try_help(1);
    }
    if (print_db && print_ls) {
        su_error(0, 0, "options --print-database and --print-ls-colors are mutually exclusive");
        su_try_help(1);
    }
    if (print_db && nops > 0) {
        su_error(0, 0, "extra operand %s", sm_quote(argv[su_optind]));
        fprintf(stderr, "file operands cannot be combined with --print-database (-p)\n");
        su_try_help(1);
    }
    if (nops > 1) {
        su_error(0, 0, "extra operand %s", sm_quote(argv[su_optind + 1]));
        su_try_help(1);
    }
    if (print_db) {
        for (int i = 0; sm_dc_db[i]; i++) puts(sm_dc_db[i]);
        if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
        return 0;
    }
    if (!print_ls && syntax < 0) {
        const char *sh = getenv("SHELL");
        if (!sh || !*sh) su_error(1, 0, "no SHELL environment variable, and no shell type option given");
        const char *b = strrchr(sh, '/');
        b = b ? b + 1 : sh;
        syntax = (!strcmp(b, "csh") || !strcmp(b, "tcsh")) ? 1 : 0;
    }
    memset(&sm_dc, 0, sizeof sm_dc);
    sm_dc.applies = 1;
    sm_dc.term = getenv("TERM");
    sm_dc.colorterm = getenv("COLORTERM");
    /* an unset or empty TERM is "none" (TERM none matches it, TERM ?*
     * too); an unset COLORTERM is empty */
    if (!sm_dc.term || !*sm_dc.term) sm_dc.term = "none";
    if (!sm_dc.colorterm) sm_dc.colorterm = "";
    if (nops == 1) {
        if (sm_dc_read_file(argv[su_optind])) return 1;
    } else {
        for (int i = 0; sm_dc_db[i]; i++) {
            char *l = strdup(sm_dc_db[i]);
            if (!l) su_error(1, ENOMEM, "memory exhausted");
            sm_dc_line(l, "", i + 1);
            free(l);
        }
    }
    if (sm_dc.errors) return 1;
    if (print_ls) {
        for (size_t i = 0; i < sm_dc.n; i++)
            printf("\033[%sm%s\t%s\033[0m\n", sm_dc.v[i].val, sm_dc.v[i].key, sm_dc.v[i].val);
    } else {
        fputs(syntax ? "setenv LS_COLORS '" : "LS_COLORS='", stdout);
        for (size_t i = 0; i < sm_dc.n; i++) {
            sm_dc_put_quoted(sm_dc.v[i].key);
            putchar('=');
            sm_dc_put_quoted(sm_dc.v[i].val);
            putchar(':');
        }
        fputs(syntax ? "'\n" : "';\nexport LS_COLORS\n", stdout);
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}
