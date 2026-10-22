/* su_basename.c -- basename and dirname, as GNU coreutils 9.7's
 * (logs/utilgate/staging/basename.sh, dirname.sh).
 *
 * WHAT WAS MEASURED:
 *   - basename NAME [SUFFIX], or with -a/--multiple or -s/--suffix=SUFFIX
 *     (which implies -a) every operand a NAME. Trailing slashes go, then
 *     everything to the last slash; a name of slashes only is "/", an
 *     empty one stays empty. SUFFIX is cut off when the rest ends with it
 *     and is not all of it (basename .h .h is ".h"; basename / / is "/").
 *     The options stop at the first operand (`basename a -z` takes -z as
 *     the SUFFIX; `basename a b -z` is "extra operand '-z'"), the last
 *     -s wins. No NAME is "missing operand".
 *   - dirname NAME...: the trailing slashes, the last component and the
 *     slashes before it go; nothing left is "/" for an absolute name and
 *     "." for one that is not (dirname //a//b// is "//a"). Its options
 *     are permuted as getopt does (`dirname a -z` is -z), except under
 *     POSIXLY_CORRECT.
 *   - -z/--zero ends each line with a NUL instead of a newline.
 *   - A failed write is "write error" at exit, as close_stdout says it.
 *
 * The old basename and dirname (sbase's) had no -a, -s or -z, took a
 * single NAME and said "usage:" for anything else. */

#include "su_txio.c"

/* the last component of NAME, without trailing slashes, in a new string */
static char *txb_base(const char *name) {
    size_t n = strlen(name);
    char *s = (char *)malloc(n + 2);
    if (!s) tx_error(1, ENOMEM, "memory exhausted");
    if (n == 0) { s[0] = 0; return s; }
    size_t end = n;
    while (end > 0 && name[end - 1] == '/') end--;
    if (end == 0) { strcpy(s, "/"); return s; }
    size_t start = end;
    while (start > 0 && name[start - 1] != '/') start--;
    memcpy(s, name + start, end - start);
    s[end - start] = 0;
    return s;
}

static void txb_strip_suffix(char *base, const char *suffix) {
    size_t bl = strlen(base), sl = strlen(suffix);
    if (sl && bl > sl && !strcmp(base + bl - sl, suffix)) base[bl - sl] = 0;
}

static void txb_put(const char *s, int zero) {
    tx_puts(s);
    tx_putc(zero ? 0 : '\n');
}

static void txb_help(void) {
    tx_printf("Usage: %s NAME [SUFFIX]\n"
              "  or:  %s OPTION... NAME...\n"
              "Print NAME without the directories before its last component, and\n"
              "without a trailing SUFFIX when one is given.\n"
              "\n"
              "Mandatory arguments to long options are mandatory for short options too.\n"
              "  -a, --multiple       take every operand as a NAME\n"
              "  -s, --suffix=SUFFIX  take a trailing SUFFIX off too; implies -a\n"
              "  -z, --zero           end each line with NUL, not newline\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "Examples:\n"
              "  %s /usr/bin/sort          -> \"sort\"\n"
              "  %s include/stdio.h .h     -> \"stdio\"\n"
              "  %s -s .h include/stdio.h  -> \"stdio\"\n"
              "  %s -a any/str1 any/str2   -> \"str1\" then \"str2\"\n",
              su_name, su_name, su_name, su_name, su_name, su_name);
}

static int do_basename(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "multiple", SU_NO_ARG, 0, 'a' },
        { "suffix", SU_REQ_ARG, 0, 's' },
        { "zero", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    tx_begin(1);
    int multiple = 0, zero = 0;
    const char *suffix = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "+as:z", longs, 0)) != -1) {
        switch (c) {
        case 'a': multiple = 1; break;
        case 's': suffix = su_optarg; multiple = 1; break;
        case 'z': zero = 1; break;
        case SU_HELP_OPT: txb_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default: tx_try_help(1);
        }
    }
    int n = argc - su_optind;
    if (n < 1) {
        tx_error(0, 0, "missing operand");
        tx_try_help(1);
    }
    if (!multiple && n > 2) {
        tx_error(0, 0, "extra operand %s", tx_quote(argv[su_optind + 2]));
        tx_try_help(1);
    }
    if (multiple) {
        for (int i = su_optind; i < argc; i++) {
            char *b = txb_base(argv[i]);
            if (suffix) txb_strip_suffix(b, suffix);
            txb_put(b, zero);
            free(b);
        }
    } else {
        char *b = txb_base(argv[su_optind]);
        if (n == 2) txb_strip_suffix(b, argv[su_optind + 1]);
        txb_put(b, zero);
        free(b);
    }
    return 0;
}

/* NAME without its last component, as dirname prints it */
static void txb_dir(const char *name, int zero) {
    size_t n = strlen(name);
    size_t end = n;
    while (end > 0 && name[end - 1] == '/') end--;        /* trailing slashes */
    while (end > 0 && name[end - 1] != '/') end--;        /* the last component */
    while (end > 0 && name[end - 1] == '/') end--;        /* the slashes before it */
    if (end == 0) {
        txb_put(n && name[0] == '/' ? "/" : ".", zero);
        return;
    }
    tx_fwrite(name, end);
    tx_putc(zero ? 0 : '\n');
}

static void txb_dhelp(void) {
    tx_printf("Usage: %s [OPTION] NAME...\n"
              "Print each NAME without its last component and the slashes after the\n"
              "rest; a NAME with no slash gives '.' (the current directory).\n"
              "\n"
              "  -z, --zero     end each line with NUL, not newline\n"
              "      --help        show this help and exit\n"
              "      --version     show the version and exit\n"
              "\n"
              "Examples:\n"
              "  %s /usr/bin/          -> \"/usr\"\n"
              "  %s dir1/str dir2/str  -> \"dir1\" then \"dir2\"\n"
              "  %s stdio.h            -> \".\"\n", su_name, su_name, su_name, su_name);
}

static int do_dirname(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "zero", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    tx_begin(1);
    int zero = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "z", longs, 0)) != -1) {
        switch (c) {
        case 'z': zero = 1; break;
        case SU_HELP_OPT: txb_dhelp(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default: tx_try_help(1);
        }
    }
    if (su_optind == argc) {
        tx_error(0, 0, "missing operand");
        tx_try_help(1);
    }
    for (int i = su_optind; i < argc; i++) txb_dir(argv[i], zero);
    return 0;
}
