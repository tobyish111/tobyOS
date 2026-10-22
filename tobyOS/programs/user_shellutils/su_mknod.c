/* su_mknod.c -- mknod, as GNU coreutils 9.7's (logs/utilgate/staging/
 * mknod.sh): NAME made a block (b) or character (c, u) special file, or a
 * FIFO (p). Only TYPE's first character counts ("pp" is p).
 *
 * Measured on GNU's (gnumknod in WSL, as an unprivileged user and with
 * strace):
 *   - MODE first: "invalid mode" (no name), and one with a set-id or
 *     sticky bit is "mode must specify only file permission bits" --
 *     both before the operands are counted. MODE is chmod's reading of
 *     it from a=rw (a clause with no [ugoa] masked by the umask).
 *   - Then the operands: a p takes NAME TYPE, the others NAME TYPE MAJOR
 *     MINOR. Too few is "missing operand after 'LAST'" (with "Special
 *     files require major and minor device numbers." when only NAME and
 *     TYPE came), too many "extra operand 'X'" (with "Fifos do not have
 *     major and minor device numbers." when a p came with two); then
 *     "invalid device type 'T'". All of these add the --help line.
 *   - MAJOR and MINOR: 0x/0X hexadecimal, 0 octal, else decimal; blanks
 *     and a '+' before it allowed, nothing after; at most 4294967295 --
 *     else "invalid major device number 'X'" (or minor), status 1. A pair
 *     too big for a 32-bit device number is EINVAL ("NAME: Invalid
 *     argument"), as glibc's mknod says before asking the kernel.
 *   - mknod(NAME, TYPE|mode), 0666 less the umask without -m; with -m the
 *     mode is set again afterwards (lchmod), so the umask does not apply.
 *     A failure is "mknod: NAME: ERR".
 *   - -Z and --context as mkdir's: nothing, or a warning for a CTX.
 *
 * tobyOS: libtoby has no mknod and the native ABI no device-node call,
 * so a b or c is what Linux says to anyone without CAP_MKNOD -- "Operation
 * not permitted" -- unless a later ABI defines ABI_SYS_MKNOD; a p is
 * mkfifo(). There was no mknod before. */

#include "su_fileops.c"

#ifndef SU_HOST
#ifdef ABI_SYS_MKNOD
static long mkn_sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}
#endif
#endif

/* MAJOR or MINOR as GNU reads one: 0, or -1 when it is not a number */
static int mkn_number(const char *s, unsigned long long *out) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    if (!isdigit((unsigned char)*p)) return -1;
    int base = 10;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
        if (!isxdigit((unsigned char)*p)) return -1;
    } else if (p[0] == '0') {
        base = 8;
    }
    unsigned long long v = 0;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else return -1;
        if (d >= base) return -1;
        if (v > (0xffffffffULL - (unsigned)d) / (unsigned)base) return -1;
        v = v * (unsigned)base + (unsigned)d;
    }
    *out = v;
    return 0;
}

/* mknod(2) for a device: 0 or -1 with errno */
static int mkn_device(const char *name, mode_t mode, unsigned long long maj,
                      unsigned long long min) {
    /* glibc refuses a number that does not fit the kernel's 32 bits */
    unsigned long long dev = ((maj & 0xfffff000ULL) << 32) | ((maj & 0xfffULL) << 8) |
                             ((min & 0xffffff00ULL) << 12) | (min & 0xffULL);
    if (dev > 0xffffffffULL) { errno = EINVAL; return -1; }
#ifdef SU_HOST
    return mknod(name, mode, (dev_t)dev);
#elif defined(ABI_SYS_MKNOD)
    long r = mkn_sc3(ABI_SYS_MKNOD, (long)name, (long)mode, (long)dev);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
#else
    /* no device nodes natively: what Linux says to an unprivileged caller,
     * after the checks of the name a creation makes */
    struct stat st;
    if (lstat(name, &st) == 0) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    /* the directory it would go in: there, and writable, else its error */
    size_t n = strlen(name);
    while (n > 1 && name[n - 1] == '/') n--;
    while (n > 0 && name[n - 1] != '/') n--;
    char *dir = (char *)malloc(n + 2);
    if (!dir) su_error(1, ENOMEM, "memory exhausted");
    if (n == 0) strcpy(dir, ".");
    else { memcpy(dir, name, n); dir[n] = 0; }
    int rc = stat(dir, &st) != 0 ? -1 : !S_ISDIR(st.st_mode) ? (errno = ENOTDIR, -1)
           : access(dir, W_OK | X_OK) != 0 ? -1 : (errno = EPERM, -1);
    int e = errno;
    free(dir);
    errno = e;
    (void)mode;
    return rc;
#endif
}

static int do_mknod(int argc, char **argv) {
    fo_stdout_guard();
    enum { CONTEXT_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "context", SU_OPT_ARG, 0, CONTEXT_OPT },
        { "mode", SU_REQ_ARG, 0, 'm' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    const char *mstr = 0;
    int c;
    while ((c = su_getopt_long(argc, argv, "m:Z", longs, 0)) != -1) {
        switch (c) {
        case 'm': mstr = su_optarg; break;
        case 'Z': break;
        case CONTEXT_OPT:
            if (su_optarg)
                su_error(0, 0, "warning: ignoring --context; "
                         "it requires an SELinux/SMACK-enabled kernel");
            break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... NAME TYPE [MAJOR MINOR]\n"
                   "Make NAME a special file of the given TYPE.\n"
                   "\n"
                   "An option's argument is required in its short form too.\n"
                   "  -m, --mode=MODE    give it the permissions MODE instead of a=rw less\n"
                   "                     the umask\n"
                   "  -Z                 its SELinux context: the default (there is no\n"
                   "                     SELinux here)\n"
                   "      --context[=CTX]  likewise; a CTX is warned about and ignored\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "TYPE b, c and u need MAJOR and MINOR, p must not have them.  Each is\n"
                   "hexadecimal after 0x or 0X, octal after a leading 0, else decimal.\n"
                   "TYPE is one of:\n"
                   "\n"
                   "  b      a block special file\n"
                   "  c, u   a character special file\n"
                   "  p      a FIFO (named pipe)\n"
                   "\n"
                   "A shell may have a mknod of its own, which a bare `mknod' then runs.\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    mode_t mode = 0666;
    if (mstr) {
        struct fo_mode m;
        if (fo_mode_compile(mstr, &m) != 0) su_error(1, 0, "invalid mode");
        mode = fo_mode_adjust(0666, 0, fo_get_umask(), &m, 0);
        if (mode & ~(mode_t)0777)
            su_error(1, 0, "mode must specify only file permission bits");
    }
    int n = argc - su_optind;
    char **op = argv + su_optind;
    if (n == 0) {
        su_error(0, 0, "missing operand");
        su_try_help(1);
    }
    int expected = (n >= 2 && op[1][0] == 'p') ? 2 : 4;
    if (n < expected) {
        su_error(0, 0, "missing operand after %s", su_quote(op[n - 1]));
        if (n == 2 && expected == 4)
            fputs("Special files require major and minor device numbers.\n", stderr);
        su_try_help(1);
    }
    if (n > expected) {
        su_error(0, 0, "extra operand %s", su_quote(op[expected]));
        if (expected == 2 && n == 4)
            fputs("Fifos do not have major and minor device numbers.\n", stderr);
        su_try_help(1);
    }
    const char *name = op[0];
    mode_t type;
    switch (op[1][0]) {
    case 'b': type = S_IFBLK; break;
    case 'c': case 'u': type = S_IFCHR; break;
    case 'p': type = S_IFIFO; break;
    default:
        su_error(0, 0, "invalid device type %s", su_quote(op[1]));
        su_try_help(1);
    }
    if (type == S_IFIFO) {
        if (mkfifo(name, mode) != 0) su_error(1, errno, "%s", su_quotef(name));
    } else {
        unsigned long long maj, min;
        if (mkn_number(op[2], &maj) != 0)
            su_error(1, 0, "invalid major device number %s", su_quote(op[2]));
        if (mkn_number(op[3], &min) != 0)
            su_error(1, 0, "invalid minor device number %s", su_quote(op[3]));
        if (mkn_device(name, type | mode, maj, min) != 0)
            su_error(1, errno, "%s", su_quotef(name));
    }
    if (mstr && chmod(name, mode) != 0)
        su_error(1, errno, "cannot set permissions of %s", su_quoteaf(name));
    return 0;
}
