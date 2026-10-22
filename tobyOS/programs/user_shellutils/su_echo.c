/* su_echo.c -- /bin/echo, as GNU coreutils 9.7's (logs/utilgate/staging/
 * echo.sh): the STRINGs, a space between each, then a newline.
 *
 * Measured on GNU's (gnuecho in WSL, run through env so the shell's own
 * echo is not the one asked):
 *   - The options are leading words made only of n, e and E after a '-'
 *     ("-n", "-neE"), taken in order -- -n no newline, -e backslash
 *     escapes, -E none (the default) -- up to the first word that is not
 *     one: "-x", "-", "--" and "-nx" are STRINGs, and so is everything
 *     after them.
 *   - --help and --version are options only as the one and only word;
 *     never abbreviated ("--hel" is printed).
 *   - With POSIXLY_CORRECT in the environment (set at all, even empty),
 *     escapes are always on, --help/--version are STRINGs, and options
 *     are looked for only when the first word is exactly "-n" -- then all
 *     the leading option words go, -E included, and changes nothing.
 *   - Escapes: \\ \a \b \e \f \n \r \t \v; \c ends the output there, the
 *     newline too; \0 and up to three octal digits, or \ and one to three
 *     octal digits (\101 is A), the value taken modulo 256; \x and one or
 *     two hex digits. Anything else -- \u, \q, a lone \x, a trailing \ --
 *     is printed as written.
 *   - A write error is "echo: write error: ERR", status 1; with nothing
 *     to write there is no error.
 * The old echo (programs/user_echo) took -n and -e only, and never with
 * the POSIX rules. */

#include "su_fileops.c"

struct ec_buf { char *s; size_t n, cap; };

static void ec_put(struct ec_buf *b, const char *s, size_t n) {
    if (b->n + n > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (nc < b->n + n) nc *= 2;
        char *ns = (char *)realloc(b->s, nc);
        if (!ns) su_error(1, ENOMEM, "memory exhausted");
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
}

static int ec_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* S with its escapes decoded into B; 1 when a \c ended the output */
static int ec_escapes(struct ec_buf *b, const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p != '\\' || !p[1]) { ec_put(b, p, 1); continue; }
        char c = *++p, out;
        switch (c) {
        case '\\': out = '\\'; break;
        case 'a': out = '\a'; break;
        case 'b': out = '\b'; break;
        case 'c': return 1;
        case 'e': out = 033; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        case 'v': out = '\v'; break;
        case 'x': {
            int h = ec_hex((unsigned char)p[1]);
            if (h < 0) { ec_put(b, "\\x", 2); continue; }
            p++;
            int h2 = ec_hex((unsigned char)p[1]);
            if (h2 >= 0) { h = h * 16 + h2; p++; }
            out = (char)h;
            break;
        }
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            /* \0NNN: the 0 is not one of the three digits; \NNN: it is */
            int v = 0, k = 0;
            if (c == '0') {
                while (k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (*++p - '0'); k++; }
            } else {
                v = c - '0';
                k = 1;
                while (k < 3 && p[1] >= '0' && p[1] <= '7') { v = v * 8 + (*++p - '0'); k++; }
            }
            out = (char)(v & 0xff);
            break;
        }
        default:
            ec_put(b, p - 1, 2);
            continue;
        }
        ec_put(b, &out, 1);
    }
    return 0;
}

/* an option word: '-' then one or more of n, e, E */
static int ec_is_option(const char *a) {
    if (a[0] != '-' || !a[1]) return 0;
    for (const char *p = a + 1; *p; p++)
        if (*p != 'n' && *p != 'e' && *p != 'E') return 0;
    return 1;
}

static int do_echo(int argc, char **argv) {
    fo_stdout_guard();
    int posix = getenv("POSIXLY_CORRECT") != 0;
    int newline = 1, escapes = posix;
    int i = 1;
    if (!posix && argc == 2) {
        if (!strcmp(argv[1], "--help")) {
            printf("Usage: %s [SHORT-OPTION]... [STRING]...\n"
                   "  or:  %s LONG-OPTION\n"
                   "Write the STRINGs to standard output, a space between each, and a\n"
                   "newline after the last.\n"
                   "\n"
                   "  -n             leave the newline off\n"
                   "  -e             decode the backslash escapes listed below\n"
                   "  -E             print backslashes as they are (the default)\n"
                   "      --help     show this help and exit (as the only word)\n"
                   "      --version  show the version and exit (as the only word)\n"
                   "\n"
                   "The escapes -e decodes:\n"
                   "\n"
                   "  \\\\      a backslash\n"
                   "  \\a      the bell (BEL)\n"
                   "  \\b      a backspace\n"
                   "  \\c      nothing more: the rest, and the newline, are dropped\n"
                   "  \\e      an escape (ESC)\n"
                   "  \\f      a form feed\n"
                   "  \\n      a newline\n"
                   "  \\r      a carriage return\n"
                   "  \\t      a tab\n"
                   "  \\v      a vertical tab\n"
                   "  \\0NNN   the byte whose value is octal NNN (one to three digits)\n"
                   "  \\xHH    the byte whose value is hexadecimal HH (one or two digits)\n"
                   "\n"
                   "A shell usually has an echo of its own, which is the one a bare\n"
                   "`echo' runs; this one is reached by its path.  With POSIXLY_CORRECT\n"
                   "set, escapes are always decoded and only a first word of -n is an\n"
                   "option.  printf(1) prints option-like words more predictably.\n",
                   su_name, su_name);
            return 0;
        }
        if (!strcmp(argv[1], "--version")) return su_version();
    }
    if (!posix || (argc > 1 && !strcmp(argv[1], "-n"))) {
        for (; i < argc && ec_is_option(argv[i]); i++) {
            for (const char *p = argv[i] + 1; *p; p++) {
                if (*p == 'n') newline = 0;
                else if (*p == 'e') escapes = 1;
                else if (!posix) escapes = 0;
            }
        }
    }
    struct ec_buf b = { 0, 0, 0 };
    int stopped = 0;
    for (int first = i; i < argc && !stopped; i++) {
        if (i > first) ec_put(&b, " ", 1);
        if (escapes) stopped = ec_escapes(&b, argv[i]);
        else ec_put(&b, argv[i], strlen(argv[i]));
    }
    if (newline && !stopped) ec_put(&b, "\n", 1);
    if (b.n && fo_write_all(1, b.s, b.n) != 0) su_error(1, errno, "write error");
    free(b.s);
    return 0;
}
