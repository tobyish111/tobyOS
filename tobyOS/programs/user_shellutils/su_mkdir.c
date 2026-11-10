/* su_mkdir.c -- mkdir, as GNU coreutils 9.7's (logs/utilgate/staging/
 * mkdir.sh): each DIRECTORY made, mode a=rwx less the umask.
 *
 * Measured on GNU's (gnumkdir in WSL, strace for the calls):
 *   - With no -m, mkdir(DIR, 0777): the kernel takes the umask off.
 *   - -m MODE is read as chmod reads it, starting from a=rwx (a clause
 *     with no [ugoa] masked by the umask), and the directory is made with
 *     exactly that, umask or not -- the umask is narrowed for the one
 *     call to what MODE does not set. When MODE has a set-id or sticky
 *     bit the directory is first made without group and other write, and
 *     afterwards it is chmodded to MODE if a bit MODE names came out
 *     different: the kernel drops set-id bits from mkdir, so `-m g+s` is
 *     2777 in the end, while `-m +t` names only the sticky bit and stays
 *     1755 (as GNU's does -- `-m 1777` is 1777).
 *   - -p makes each missing parent with 0777 less the umask, the umask
 *     first narrowed so the owner keeps write and search (umask 777 makes
 *     them 0300), and is quiet when DIR is already a directory; "." and
 *     ".." parts are walked through, not made. A parent that cannot be
 *     made or entered is "cannot create directory 'PREFIX': ERR" -- the
 *     error of entering it, unless that is ENOENT, then the error making
 *     it (a dangling symbolic link is "File exists", a file "Not a
 *     directory").
 *   - -v: "mkdir: created directory 'D'" on standard output for each one
 *     made, parents too. -Z and --context do nothing (no SELinux here);
 *     --context=CTX warns that it is ignored, as GNU on such a kernel.
 *
 * Linux's calls are made as Linux makes them: mkdir(DIR, 0) is asked for
 * mode 0 (the tobyOS kernel made 0755 of that; batch 32 changes it). The
 * old mkdir (programs/user_mkdir) had -p -m -v, but -m's symbolic modes
 * were its own, and a mode went in through chmod alone. */

#include "su_fileops.c"

static struct {
    int parents, verbose, have_mode;
    mode_t umask_v, mode, bits;
} mkd;

static void mkd_said(const char *dir) {
    if (!mkd.verbose) return;
    printf("%s: created directory %s\n", su_short, su_quoteaf(dir));
}

/* what chdir(DIR) would say -- 0 or an errno -- without moving */
static int mkd_enter_errno(const char *dir) {
    struct stat st;
    if (stat(dir, &st) != 0) return errno;
    if (!S_ISDIR(st.st_mode)) return ENOTDIR;
    if (access(dir, X_OK) != 0) return errno;
    return 0;
}

/* DIR itself: the last part of an operand */
static int mkd_final(const char *dir) {
    int r, e;
    if (mkd.have_mode) {
        mode_t narrow = mkd.umask_v & ~mkd.mode;
        mode_t arg = (mkd.mode & 07000) ? (mkd.mode & ~(mode_t)022) : mkd.mode;
        if (narrow != mkd.umask_v) umask(narrow);
        r = mkdir(dir, arg);
        e = errno;
        if (narrow != mkd.umask_v) umask(mkd.umask_v);
    } else {
        r = mkdir(dir, 0777);
        e = errno;
    }
    if (r != 0) {
        struct stat st;
        if (mkd.parents && stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
        su_error(0, e, "cannot create directory %s", su_quote(dir));
        return 1;
    }
    mkd_said(dir);
    if (mkd.have_mode) {
        struct stat st;
        if (lstat(dir, &st) == 0 && ((st.st_mode ^ mkd.mode) & mkd.bits & 07777) &&
            chmod(dir, mkd.mode) != 0) {
            su_error(0, errno, "cannot set permissions of %s", su_quote(dir));
            return 1;
        }
    }
    return 0;
}

/* DIR and, with -p, each missing parent of it */
static int mkd_one(const char *dir) {
    if (!mkd.parents) return mkd_final(dir);
    size_t len = strlen(dir);
    char *pre = (char *)malloc(len + 1);
    if (!pre) su_error(1, ENOMEM, "memory exhausted");
    size_t i = 0;
    while (i < len && dir[i] == '/') i++;
    for (;;) {
        size_t start = i;
        while (i < len && dir[i] != '/') i++;
        size_t end = i;
        while (i < len && dir[i] == '/') i++;
        if (start == end || i >= len) break;       /* the last part: below */
        memcpy(pre, dir, end);
        pre[end] = 0;
        const char *part = dir + start;
        size_t plen = end - start;
        int dot = (plen == 1 && part[0] == '.') || (plen == 2 && part[0] == '.' && part[1] == '.');
        int made = -1, e1 = 0;
        if (!dot) {
            mode_t narrow = mkd.umask_v & ~(mode_t)0300;
            if (narrow != mkd.umask_v) umask(narrow);
            made = mkdir(pre, 0777);
            e1 = errno;
            if (narrow != mkd.umask_v) umask(mkd.umask_v);
            if (made == 0) mkd_said(pre);
        }
        int e2 = mkd_enter_errno(pre);
        if (e2) {
            su_error(0, (e2 == ENOENT && made != 0 && !dot) ? e1 : e2,
                     "cannot create directory %s", su_quote(pre));
            free(pre);
            return 1;
        }
    }
    free(pre);
    return mkd_final(dir);
}

static int do_mkdir(int argc, char **argv) {
    fo_stdout_guard();
    enum { CONTEXT_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "context", SU_OPT_ARG, 0, CONTEXT_OPT },
        { "mode", SU_REQ_ARG, 0, 'm' },
        { "parents", SU_NO_ARG, 0, 'p' },
        { "verbose", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *mstr = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "pm:vZ", longs, 0)) != -1) {
        switch (c) {
        case 'p': mkd.parents = 1; break;
        case 'm': mstr = su_optarg; break;
        case 'v': mkd.verbose = 1; break;
        case 'Z': break;
        case CONTEXT_OPT:
            if (su_optarg)
                su_error(0, 0, "warning: ignoring --context; "
                         "it requires an SELinux/SMACK-enabled kernel");
            break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... DIRECTORY...\n"
                   "Make each DIRECTORY that does not exist yet.\n"
                   "\n"
                   "An option's argument is required in its short form too.\n"
                   "  -m, --mode=MODE   give it MODE (read as chmod reads one) instead of\n"
                   "                    a=rwx less the umask\n"
                   "  -p, --parents     make missing parent directories too (their mode is\n"
                   "                    not -m's), and say nothing of one that exists\n"
                   "  -v, --verbose     say so for each directory made\n"
                   "  -Z                its SELinux context: the default (there is no\n"
                   "                    SELinux here)\n"
                   "      --context[=CTX]  likewise; a CTX is warned about and ignored\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind == argc) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    mkd.umask_v = fo_get_umask();
    if (mstr) {
        struct fo_mode m;
        if (fo_mode_compile(mstr, &m) != 0) su_error(1, 0, "invalid mode %s", su_quote(mstr));
        mkd.mode = fo_mode_adjust(0777, 1, mkd.umask_v, &m, &mkd.bits);
        mkd.have_mode = 1;
    }
    int rc = 0;
    for (int i = su_optind; i < argc; i++) rc |= mkd_one(argv[i]);
    return rc;
}
