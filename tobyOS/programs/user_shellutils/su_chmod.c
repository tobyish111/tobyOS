/* su_chmod.c -- chmod, as GNU coreutils 9.7's (logs/utilgate/staging/
 * chmod.sh): chmod [OPTION]... MODE[,MODE]... FILE..., or OCTAL-MODE, or
 * --reference=RFILE.
 *
 * Measured on GNU's (gnuchmod in WSL), case by case in the fixture:
 *   - MODE as fo_mode_compile reads it (su_fileops.c: octal, [-+=]OCTAL,
 *     clauses; a directory's set-id bits kept by octal of fewer than five
 *     digits and by a symbolic '='). "invalid mode: 'M'" otherwise.
 *   - A MODE may be given as options: -w, -rx, -7, -=w, -u ... -- the
 *     whole word is the MODE, so "-Rw" and "-wv" are invalid modes and
 *     -R and -v must be words of their own; several are joined with ','.
 *     A MODE given so, and only so, is checked against the umask: when a
 *     clause with no [ugoa] leaves a bit set that it would have cleared
 *     with no umask, "chmod: F: new permissions are NEW, not NAIVE",
 *     status 1 (-f does not quiet it).
 *   - chmod(2) is called for every FILE, changed or not. -v: "mode of 'F'
 *     changed from 0777 (rwxrwxrwx) to 0755 (rwxr-xr-x)", "... retained
 *     as ...", "failed to change mode of 'F' from ... to ...", "'F' could
 *     not be accessed", "neither symbolic link 'F' nor referent has been
 *     changed"; -c only the "changed" lines; the last of -c and -v wins.
 *     -f drops "changing permissions of 'F': ERR" and "cannot access", not
 *     the -v lines.
 *   - Symbolic links: a FILE that is one is followed (-L, -H, or no -R);
 *     a dangling one is "cannot operate on dangling symlink 'F'". -h
 *     changes no link and follows none ("neither ..."). With -R, -P
 *     follows none, -H (the default) only the FILEs, -L every one, into
 *     directories too (a directory met again is not entered twice); one
 *     met inside a tree is left alone unless -L or --dereference ("cannot
 *     dereference 'F': ERR" when it dangles).
 *   - -R: a directory is changed, then what is in it, in readdir order;
 *     one that will not open is "cannot read directory 'D': ERR". A FILE
 *     is shown with its trailing slashes made one ("d//" is 'd/'), and an
 *     entry below it as FILE/NAME.
 *   - --preserve-root refuses -R on the root: "it is dangerous to operate
 *     recursively on '/'" (or "on 'X' (same as '/')"), then "use
 *     --no-preserve-root to override this failsafe".
 *   - --reference=RFILE takes RFILE's mode (followed): "failed to get
 *     attributes of 'R': ERR"; with a MODE too, "cannot combine mode and
 *     --reference options".
 *
 * The old chmod (programs/user_chmod) had -R -v -c -f and modes of its
 * own reading; no -h -H -L -P, no --reference, no umask check. */

#include "su_fileops.c"

enum { CHM_TRAV_P, CHM_TRAV_H, CHM_TRAV_L };
enum { CHM_V_NONE, CHM_V_CHANGES, CHM_V_ALL };

static struct {
    int recursive, verbose, force, deref, no_deref, trav, preserve_root, diag;
    int use_ref;
    mode_t ref_mode, umask_v;
    struct fo_mode mode;
    dev_t root_dev;
    ino_t root_ino;
    int have_root;
    /* the directories being walked, for -L's loops */
    dev_t *sdev;
    ino_t *sino;
    size_t sn, scap;
} chm;

static void chm_verbose_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void chm_verbose_line(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void chm_unreached(const char *name) {
    if (chm.verbose == CHM_V_ALL) chm_verbose_line("%s could not be accessed", su_quoteaf(name));
}

static int chm_walk(const char *name, int top);

/* change NAME, whose stat (followed where it should be) is ST */
static int chm_change(const char *name, const struct stat *st) {
    mode_t old = st->st_mode & 07777;
    int is_dir = S_ISDIR(st->st_mode);
    mode_t nm = chm.use_ref ? chm.ref_mode
                            : fo_mode_adjust(old, is_dir, chm.umask_v, &chm.mode, 0);
    char ob[10], nb[10];
    if (chmod(name, nm) != 0) {
        int e = errno;
        if (!chm.force) su_error(0, e, "changing permissions of %s", su_quoteaf(name));
        if (chm.verbose == CHM_V_ALL)
            chm_verbose_line("failed to change mode of %s from %04lo (%s) to %04lo (%s)",
                             su_quoteaf(name), (unsigned long)old, fo_perm_string(old, ob),
                             (unsigned long)nm, fo_perm_string(nm, nb));
        return 1;
    }
    if (chm.verbose == CHM_V_ALL || (chm.verbose == CHM_V_CHANGES && nm != old)) {
        if (nm != old)
            chm_verbose_line("mode of %s changed from %04lo (%s) to %04lo (%s)",
                             su_quoteaf(name), (unsigned long)old, fo_perm_string(old, ob),
                             (unsigned long)nm, fo_perm_string(nm, nb));
        else
            chm_verbose_line("mode of %s retained as %04lo (%s)",
                             su_quoteaf(name), (unsigned long)nm, fo_perm_string(nm, nb));
    }
    if (chm.diag && !chm.use_ref) {
        mode_t naive = fo_mode_adjust(old, is_dir, 0, &chm.mode, 0);
        if (nm & ~naive) {
            su_error(0, 0, "%s: new permissions are %s, not %s", su_quotef(name),
                     fo_perm_string(nm, nb), fo_perm_string(naive, ob));
            return 1;
        }
    }
    return 0;
}

static int chm_on_stack(const struct stat *st) {
    for (size_t i = 0; i < chm.sn; i++)
        if (chm.sdev[i] == st->st_dev && chm.sino[i] == st->st_ino) return 1;
    return 0;
}

/* the entries of directory NAME (whose stat is ST), each walked */
static int chm_descend(const char *name, const struct stat *st) {
    DIR *d = opendir(name);
    if (!d) {
        if (!chm.force) su_error(0, errno, "cannot read directory %s", su_quoteaf(name));
        chm_unreached(name);
        return 1;
    }
    if (chm.sn == chm.scap) {
        chm.scap = chm.scap ? chm.scap * 2 : 16;
        chm.sdev = (dev_t *)realloc(chm.sdev, chm.scap * sizeof *chm.sdev);
        chm.sino = (ino_t *)realloc(chm.sino, chm.scap * sizeof *chm.sino);
        if (!chm.sdev || !chm.sino) su_error(1, ENOMEM, "memory exhausted");
    }
    chm.sdev[chm.sn] = st->st_dev;
    chm.sino[chm.sn] = st->st_ino;
    chm.sn++;
    /* the names first: the walk below may change what opendir can see */
    char **names = 0;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char **)realloc(names, cap * sizeof *names);
            if (!names) su_error(1, ENOMEM, "memory exhausted");
        }
        names[n] = strdup(e->d_name);
        if (!names[n]) su_error(1, ENOMEM, "memory exhausted");
        n++;
    }
    closedir(d);
    int rc = 0;
    size_t nl = strlen(name);
    int slash = nl > 0 && name[nl - 1] == '/';
    for (size_t i = 0; i < n; i++) {
        char *path = (char *)malloc(nl + strlen(names[i]) + 2);
        if (!path) su_error(1, ENOMEM, "memory exhausted");
        sprintf(path, "%s%s%s", name, slash ? "" : "/", names[i]);
        rc |= chm_walk(path, 0);
        free(path);
        free(names[i]);
    }
    free(names);
    chm.sn--;
    return rc;
}

/* NAME: a FILE operand (TOP) or an entry found by -R */
static int chm_walk(const char *name, int top) {
    struct stat lst, st;
    if (lstat(name, &lst) != 0) {
        if (!chm.force) su_error(0, errno, "cannot access %s", su_quoteaf(name));
        chm_unreached(name);
        return 1;
    }
    int follow;
    if (top) follow = !chm.no_deref && !(chm.recursive && chm.trav == CHM_TRAV_P);
    else follow = chm.trav == CHM_TRAV_L || chm.deref;
    if (S_ISLNK(lst.st_mode)) {
        if (!follow) {
            if (chm.verbose == CHM_V_ALL)
                chm_verbose_line("neither symbolic link %s nor referent has been changed",
                                 su_quoteaf(name));
            return 0;
        }
        if (stat(name, &st) != 0) {
            int e = errno;
            if (!chm.force) {
                if (top || chm.trav == CHM_TRAV_L)
                    su_error(0, 0, "cannot operate on dangling symlink %s", su_quoteaf(name));
                else
                    su_error(0, e, "cannot dereference %s", su_quoteaf(name));
            }
            chm_unreached(name);
            return 1;
        }
    } else {
        st = lst;
    }
    if (chm.recursive && chm.preserve_root && S_ISDIR(st.st_mode) && chm.have_root &&
        st.st_dev == chm.root_dev && st.st_ino == chm.root_ino) {
        if (!strcmp(name, "/"))
            su_error(0, 0, "it is dangerous to operate recursively on %s", su_quoteaf(name));
        else
            su_error(0, 0, "it is dangerous to operate recursively on %s (same as %s)",
                     su_quoteaf(name), su_quoteaf("/"));
        su_error(0, 0, "use --no-preserve-root to override this failsafe");
        return 1;
    }
    int rc = chm_change(name, &st);
    if (chm.recursive && S_ISDIR(st.st_mode)) {
        /* a link is entered only when the walk follows links into
         * directories: -L, or -H for a FILE */
        int enter = !S_ISLNK(lst.st_mode) || chm.trav == CHM_TRAV_L ||
                    (top && chm.trav == CHM_TRAV_H);
        if (enter && !chm_on_stack(&st)) rc |= chm_descend(name, &st);
    }
    return rc;
}

/* an operand as GNU shows it: trailing slashes made one -- but "//", which
 * POSIX lets mean something else than "/", is kept ("///" is "/") */
static char *chm_display(const char *arg) {
    size_t n = strlen(arg);
    char *s = strdup(arg);
    if (!s) su_error(1, ENOMEM, "memory exhausted");
    if (n == 2 && !strcmp(s, "//")) return s;
    while (n > 1 && s[n - 1] == '/' && s[n - 2] == '/') s[--n] = 0;
    return s;
}

static int do_chmod(int argc, char **argv) {
    fo_stdout_guard();
    enum { DEREF_OPT = 0x100, NO_PRESERVE_OPT, PRESERVE_OPT, REFERENCE_OPT };
    /* GNU's order: the ambiguity messages list them so */
    static const struct su_option longs[] = {
        { "changes", SU_NO_ARG, 0, 'c' },
        { "recursive", SU_NO_ARG, 0, 'R' },
        { "dereference", SU_NO_ARG, 0, DEREF_OPT },
        { "no-dereference", SU_NO_ARG, 0, 'h' },
        { "no-preserve-root", SU_NO_ARG, 0, NO_PRESERVE_OPT },
        { "preserve-root", SU_NO_ARG, 0, PRESERVE_OPT },
        { "quiet", SU_NO_ARG, 0, 'f' },
        { "reference", SU_REQ_ARG, 0, REFERENCE_OPT },
        { "silent", SU_NO_ARG, 0, 'f' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    /* the MODE letters are options too, each taking the rest of its word */
    static const char shorts[] = "HLPRcfhvr::w::x::X::s::t::u::g::o::a::,::+::=::"
                                 "0::1::2::3::4::5::6::7::";
    char *mode_opt = 0;
    size_t mode_len = 0;
    const char *ref = 0;
    int c;
    chm.trav = CHM_TRAV_H;
    while ((c = su_getopt_long(argc, argv, shorts, longs, 0)) != -1) {
        switch (c) {
        case 'H': chm.trav = CHM_TRAV_H; break;
        case 'L': chm.trav = CHM_TRAV_L; break;
        case 'P': chm.trav = CHM_TRAV_P; break;
        case 'R': chm.recursive = 1; break;
        case 'c': chm.verbose = CHM_V_CHANGES; break;
        case 'f': chm.force = 1; break;
        case 'h': chm.no_deref = 1; chm.deref = 0; break;
        case 'v': chm.verbose = CHM_V_ALL; break;
        case DEREF_OPT: chm.deref = 1; chm.no_deref = 0; break;
        case NO_PRESERVE_OPT: chm.preserve_root = 0; break;
        case PRESERVE_OPT: chm.preserve_root = 1; break;
        case REFERENCE_OPT: ref = su_optarg; break;
        case 'r': case 'w': case 'x': case 'X': case 's': case 't':
        case 'u': case 'g': case 'o': case 'a': case ',': case '+': case '=':
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            /* the word this letter began: what GNU takes as the MODE */
            const char *word = argv[su_optind - 1];
            size_t wl = strlen(word);
            char *nm = (char *)realloc(mode_opt, mode_len + wl + 2);
            if (!nm) su_error(1, ENOMEM, "memory exhausted");
            mode_opt = nm;
            if (mode_len) mode_opt[mode_len++] = ',';
            memcpy(mode_opt + mode_len, word, wl + 1);
            mode_len += wl;
            chm.diag = 1;
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... MODE[,MODE]... FILE...\n"
                   "  or:  %s [OPTION]... OCTAL-MODE FILE...\n"
                   "  or:  %s [OPTION]... --reference=RFILE FILE...\n"
                   "Set the mode of each FILE to MODE, or with --reference to RFILE's.\n"
                   "\n"
                   "  -c, --changes          like -v, but only for a mode that changed\n"
                   "  -f, --silent, --quiet  leave out most error messages\n"
                   "  -v, --verbose          say what was done to each FILE\n"
                   "      --dereference      change what a symbolic link names, not the\n"
                   "                           link\n"
                   "  -h, --no-dereference   change symbolic links themselves (which have\n"
                   "                           no mode here), not what they name\n"
                   "      --no-preserve-root  treat '/' like any directory (the default)\n"
                   "      --preserve-root    refuse to work recursively on '/'\n"
                   "      --reference=RFILE  take the mode from RFILE (followed if it is a\n"
                   "                           symbolic link) instead of from a MODE\n"
                   "  -R, --recursive        change directories and what is in them\n"
                   "\n"
                   "With -R, these say which symbolic links the walk follows (the last one\n"
                   "given counts; -H when none is):\n"
                   "\n"
                   "  -H                     a FILE that is a link to a directory\n"
                   "  -L                     every link to a directory\n"
                   "  -P                     none\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "A MODE is '[ugoa]*([-+=]([rwxXst]*|[ugo]))+|[-+=][0-7]+', or several of\n"
                   "them separated by commas.\n",
                   su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    const char *mode_str = mode_opt;
    if (ref) {
        if (mode_opt) {
            su_error(0, 0, "cannot combine mode and --reference options");
            su_try_help(1);
        }
    } else if (!mode_opt) {
        if (su_optind < argc) mode_str = argv[su_optind++];
    }
    if (su_optind == argc) {
        if (!mode_str || ref || mode_opt) su_error(0, 0, "missing operand");
        else su_error(0, 0, "missing operand after %s", su_quote(mode_str));
        su_try_help(1);
    }
    if (ref) {
        struct stat rs;
        if (stat(ref, &rs) != 0)
            su_error(1, errno, "failed to get attributes of %s", su_quoteaf(ref));
        chm.ref_mode = rs.st_mode & 07777;
        chm.use_ref = 1;
    } else {
        if (fo_mode_compile(mode_str, &chm.mode) != 0) {
            su_error(0, 0, "invalid mode: %s", su_quote(mode_str));
            su_try_help(1);
        }
        chm.umask_v = fo_get_umask();
    }
    if (chm.recursive && chm.preserve_root) {
        struct stat rs;
        if (stat("/", &rs) != 0)
            su_error(1, errno, "failed to get attributes of %s", su_quoteaf("/"));
        chm.root_dev = rs.st_dev;
        chm.root_ino = rs.st_ino;
        chm.have_root = 1;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) {
        char *name = chm_display(argv[i]);
        rc |= chm_walk(name, 1);
        free(name);
    }
    return rc;
}
