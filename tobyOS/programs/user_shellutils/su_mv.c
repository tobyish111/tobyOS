/* su_mv.c -- mv, as GNU coreutils 9.7's (logs/utilgate/mv.sh).
 *
 *   mv [OPTION]... [-T] SOURCE DEST
 *   mv [OPTION]... SOURCE... DIRECTORY
 *   mv [OPTION]... -t DIRECTORY SOURCE...
 *
 * A rename, carrying the inode. Measured on GNU's mv (gnumv, strace'd):
 *
 *   - With two operands it first tries the rename outright without
 *     replacing anything; only if that fails does it look at whether DEST
 *     is a directory to move into -- and a failure it need not retry is
 *     reported as it came ("cannot move 'ld' to 'x': Not a directory").
 *   - Then the checks, in GNU's order: "cannot stat", "'A' and 'B' are
 *     the same file" (a link onto what it names is the same file; a file
 *     onto a link to it is not), "cannot overwrite directory 'D' with
 *     non-directory 'S'" and the reverse (not with a backup, which moves
 *     DEST aside first), -n/--update, the prompt ("overwrite 'D'?", or
 *     "replace 'D', overriding mode 0444 (r--r--r--)?" for one not
 *     writable -- asked without -i too when stdin is a terminal, never
 *     with -f), "will not overwrite just-created" (not for a directory),
 *     the backup -- of a directory too, where cp makes none.
 *   - -u skips only what is not a directory: a directory is moved over
 *     an empty one whatever its time. Into a directory, mv names "e/.."
 *     after ".." (cp says "e/.").
 *   - -i, -f, -n: the last one counts. A "no" is status 1. -n says
 *     nothing (cp warns; mv does not). --update=none-fail: "not
 *     replacing 'D'". --backup with -n, --update=none(-fail) or
 *     --exchange: "cannot combine --backup with --exchange, -n, or
 *     --update=none-fail".
 *   - Rename errors: "cannot move 'D' to a subdirectory of itself,
 *     'D/D'", "cannot overwrite 'D': Directory not empty", "cannot move
 *     'S' to 'D': ERR" ('.' is "Device or resource busy").
 *   - Across filesystems (EXDEV) the source is copied as cp -a would
 *     (hard links among the sources kept, "copied 'S' -> 'D'", "created
 *     directory 'D'" with -v) and then removed as rm -rf would ("removed
 *     'S'"); a copy that fails leaves the source. A directory onto one
 *     that is not empty: "inter-device move failed: 'S' to 'D'; unable to
 *     remove target: Directory not empty". --no-copy refuses instead.
 *   - --exchange swaps SOURCE and DEST atomically (renameat2's
 *     RENAME_EXCHANGE); a DEST that does not exist is "cannot exchange
 *     'S' and 'D': Unknown error -1" -- GNU 9.7's own words. -Z and
 *     --context do nothing without SELinux; --context takes no argument.
 *
 * Needs su_rm.c and su_cp.c included before it.
 *
 * On tobyOS RENAME_NOREPLACE is a look before the rename, and --exchange
 * goes to the kernel's renameat2 (ABI_SYS_RENAMEAT2, batch 31); without
 * that call it fails as GNU fails on a filesystem that cannot do it:
 * "cannot exchange 'S' and 'D': Operation not
 * supported". rename(2) there is also kept from what Linux refuses -- and
 * a native filesystem might not (tmpfs drops a directory that is not
 * empty, ramfs says EINVAL for one): a directory into itself (EINVAL),
 * '.' or '..' (EBUSY), "link/" or a non-directory with a trailing slash
 * (ENOTDIR), a file onto a directory (EISDIR) and the reverse (ENOTDIR),
 * a directory onto one that is not empty (ENOTEMPTY).
 *
 * It was SRC DST or SRC... DIRECTORY with no options at all ("--" only),
 * "cannot move directory across filesystems", and copies that lost the
 * owner and times. */

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1
#endif
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE 2
#endif
/* Linux's EOPNOTSUPP: its message is written out as GNU prints it, since
 * this libc words errno 95 differently ("Not supported") */
#define MVX_EOPNOTSUPP 95

struct mvx_opts {
    int interactive;        /* CPX_I_ASK, CPX_I_NO, CPX_I_FORCE or unset */
    int update, backup, verbose, exchange, no_copy, strip, stdin_tty;
    const char *suffix;
    mode_t umask_v;
};

#ifndef SU_HOST
/* Would DST be inside directory S (or be S)? Walks up from DST's
 * directory through ".." to the root. tobyOS only: there, rename itself
 * cannot be trusted to refuse moving a directory into itself. */
static int mvx_inside(const struct stat *s, const char *src, const char *dst) {
    struct stat st, root;
    if (stat("/", &root) != 0) return 0;
    char *p = cpx_dirname(dst);
    int inside = 0;
    for (int depth = 0; depth < 4096; depth++) {
        if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) break;
        if (cpx_same(&st, s, p, src)) { inside = 1; break; }
        if (st.st_dev == root.st_dev && st.st_ino == root.st_ino) break;
        char *q = rmf_join(p, "..");
        free(p);
        p = q;
    }
    free(p);
    return inside;
}
#endif

#if !defined(SU_HOST) && defined(ABI_SYS_RENAMEAT2)
/* the kernel's renameat2, for --exchange (batch 31) */
static long mvx_sc5(long n, long a1, long a2, long a3, long a4, long a5) {
    long r;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    __asm__ volatile("syscall" : "=a"(r)
                     : "0"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}
#endif

/* rename with Linux's answers wherever the native call could differ, and
 * renameat2's flags: NOREPLACE (EEXIST rather than replace), EXCHANGE */
static int mvx_rename(const char *src, const char *dst, int flags) {
#ifdef SU_HOST
    if (flags) return (int)syscall(SYS_renameat2, AT_FDCWD, src, AT_FDCWD, dst, flags);
    return rename(src, dst);
#else
    struct stat s;
    if (flags & RENAME_EXCHANGE) {
#ifdef ABI_SYS_RENAMEAT2
        long r = mvx_sc5(ABI_SYS_RENAMEAT2, -100, (long)src, -100, (long)dst, flags);
        if (r < 0) { errno = (int)-r; return -1; }
        return 0;
#else
        errno = MVX_EOPNOTSUPP;
        return -1;
#endif
    }
    /* What the kernel does not judge: a name ending in "." or ".." (its
     * walk would rename the directory the name leads to), a trailing
     * slash on what is not a directory (the walk cleans the slash away),
     * a directory moved under itself through a link. The rules of kind,
     * NOREPLACE, and EXDEV before both are the kernel's renameat2's, as
     * Linux's are (batch 31): judged here first, they came before EXDEV --
     * `mv t X` onto another disk's non-empty X/t said "cannot overwrite"
     * where GNU's copies and then says "unable to remove target". */
    if (rmf_dotname(src) || rmf_dotname(dst)) { errno = EBUSY; return -1; }
    char *base = rmf_noslash(src);
    if (lstat(base ? base : src, &s) != 0) { free(base); return -1; }
    size_t dn = strlen(dst);
    int slash = base != 0 || (dn > 1 && dst[dn - 1] == '/');
    free(base);
    if (slash && !S_ISDIR(s.st_mode)) { errno = ENOTDIR; return -1; }
    if (S_ISDIR(s.st_mode) && mvx_inside(&s, src, dst)) { errno = EINVAL; return -1; }
#ifdef ABI_SYS_RENAMEAT2
    long r = mvx_sc5(ABI_SYS_RENAMEAT2, -100, (long)src, -100, (long)dst, flags);
    if (r < 0) { errno = (int)-r; return -1; }
    return 0;
#else
    if (flags & RENAME_NOREPLACE) {
        struct stat d;
        if (lstat(dst, &d) == 0) { errno = EEXIST; return -1; }
    }
    return rename(src, dst);
#endif
#endif
}

/* TARGET/the last component of SRC, trailing slashes aside -- as it is:
 * mv names "e/.." where cp names "e/." */
static char *mvx_into(const char *target, const char *src) {
    size_t n = strlen(src);
    while (n > 1 && src[n - 1] == '/') n--;
    size_t b = n;
    while (b > 0 && src[b - 1] != '/') b--;
    char *base = strndup(src + b, n - b);
    if (!base) su_error(1, ENOMEM, "memory exhausted");
    char *d = rmf_join(target, base);
    free(base);
    return d;
}

static void mvx_prompt_mode(const char *dst, const struct stat *dl) {
    char m9[10];
    cpx_mode9(dl->st_mode, m9);
    fprintf(stderr, "%s: replace %s, overriding mode %04o (%s)? ", su_short,
            su_quoteaf(dst), (unsigned)(dl->st_mode & 07777), m9);
}

/* the copy-then-remove of a move across filesystems; DEXISTS: DST is
 * still there to be removed first, BK the backup already made */
static int mvx_xdev(const struct mvx_opts *mo, const char *src, const char *dst,
                    int dexists, const struct stat *dl, const char *bk) {
    if (dexists) {
        if ((S_ISDIR(dl->st_mode) ? rmdir(dst) : unlink(dst)) != 0) {
            su_error(0, errno, "inter-device move failed: %s to %s; unable to remove target",
                     su_quoteaf(src), su_quoteaf(dst));
            return 1;
        }
    }
    struct cpx_opts c;
    memset(&c, 0, sizeof c);
    c.move = 1;
    c.recursive = 1;
    c.deref = CPX_DEREF_NEVER;
    c.p_mode = c.p_owner = c.p_time = c.p_links = 1;
    c.verbose = mo->verbose;
    c.sparse = CPX_SP_AUTO;
    c.reflink = CPX_RL_AUTO;
    c.umask_v = mo->umask_v;
    c.top_bk = bk;
    if (cpx_copy(&c, src, dst, 1, 0, 0)) return 1;
    struct rmf_opts r;
    memset(&r, 0, sizeof r);
    r.interactive = RMF_NEVER;
    r.recursive = 1;
    r.verbose = mo->verbose;
    r.preserve_root = 1;
    rmf_get_root(&r);
    return rmf_remove(&r, src) == 1;
}

/* Move SRC to DST. PREV: the errno of a no-replace rename already tried
 * (0 for none); with DST not there, it is the answer -- not retried. */
static int mvx_move(const struct mvx_opts *o, const char *src_arg, const char *dst, int prev,
                    int top) {
    char *src = cpx_dup(src_arg);
    if (o->strip) {
        size_t l = strlen(src);
        while (l > 1 && src[l - 1] == '/') src[--l] = 0;
    }
    int rc = 1;
    char *bk = 0;
    struct stat s, dl;
    if (lstat(src, &s) != 0) {
        su_error(0, errno, "cannot stat %s", su_quoteaf(src));
        goto out;
    }
    /* an empty DEST is looked at as GNU looks at it: as "." */
    int dexists = lstat(*dst ? dst : ".", &dl) == 0;

    if (o->exchange) {
        if (!dexists) {
            su_error(0, 0, "cannot exchange %s and %s: Unknown error -1",
                     su_quoteaf(src), su_quoteaf(dst));
            goto out;
        }
    }
    if (dexists) {
        struct stat sf = s;
        if (S_ISLNK(s.st_mode) && !S_ISLNK(dl.st_mode) && stat(src, &sf) != 0) sf = s;
        if (cpx_same(&sf, &dl, src, dst)) {
            su_error(0, 0, "%s and %s are the same file", su_quoteaf(src), su_quoteaf(dst));
            goto out;
        }
        if (!o->exchange && o->backup == CPX_BK_NONE) {
            if (S_ISDIR(dl.st_mode) && !S_ISDIR(s.st_mode)) {
                su_error(0, 0, "cannot overwrite directory %s with non-directory %s",
                         su_quoteaf(dst), su_quoteaf(src));
                goto out;
            }
            if (!S_ISDIR(dl.st_mode) && S_ISDIR(s.st_mode)) {
                su_error(0, 0, "cannot overwrite non-directory %s with directory %s",
                         su_quoteaf(dst), su_quoteaf(src));
                goto out;
            }
        }
        if (o->interactive == CPX_I_NO || o->update == CPX_UP_NONE) { rc = 0; goto out; }
        if (o->update == CPX_UP_NONE_FAIL) {
            su_error(0, 0, "not replacing %s", su_quoteaf(dst));
            goto out;
        }
        if (o->update == CPX_UP_OLDER && !S_ISDIR(s.st_mode) && !cpx_older(&dl, &s)) { rc = 0; goto out; }
        int wp = !S_ISLNK(dl.st_mode) && access(dst, W_OK) != 0 && errno == EACCES;
        if (o->interactive == CPX_I_ASK ||
            (o->interactive == CPX_I_UNSPEC && o->stdin_tty && wp)) {
            fflush(stdout);
            if (wp) mvx_prompt_mode(dst, &dl);
            else fprintf(stderr, "%s: overwrite %s? ", su_short, su_quoteaf(dst));
            if (!rmf_yesno()) goto out;
        }
        if (top && !o->exchange && o->backup != CPX_BK_NUMBERED && !S_ISDIR(dl.st_mode) &&
            cpx_tab_find(&cpx_made, dl.st_dev, dl.st_ino, dst)) {
            su_error(0, 0, "will not overwrite just-created %s with %s",
                     su_quoteaf(dst), su_quoteaf(src));
            goto out;
        }
    }
    if (o->exchange) {
        if (mvx_rename(src, dst, RENAME_EXCHANGE) != 0) {
            if (errno == MVX_EOPNOTSUPP)
                su_error(0, 0, "cannot exchange %s and %s: Operation not supported",
                         su_quoteaf(src), su_quoteaf(dst));
            else
                su_error(0, errno, "cannot exchange %s and %s", su_quoteaf(src),
                         su_quoteaf(dst));
            goto out;
        }
        if (o->verbose) printf("exchanged %s <-> %s\n", su_quoteaf(src), su_quoteaf(dst));
        rc = 0;
        goto out;
    }
    if (dexists && o->backup != CPX_BK_NONE) {
        struct cpx_opts b;
        memset(&b, 0, sizeof b);
        b.backup = o->backup;
        b.suffix = o->suffix;
        bk = cpx_backup(&b, dst);
        if (!bk) goto out;
        dexists = 0;
    }
    int r, e;
    if (prev && !dexists) {
        r = -1;
        e = prev;
    } else {
        r = mvx_rename(src, dst, 0);
        e = errno;
    }
    if (r == 0) {
        if (o->verbose) {
            printf("renamed %s -> %s", su_quoteaf(src), su_quoteaf(dst));
            if (bk) printf(" (backup: %s)", su_quoteaf(bk));
            putchar('\n');
        }
        rc = 0;
    } else if (e == EXDEV && !o->no_copy) {
        rc = mvx_xdev(o, src, dst, dexists, &dl, bk);
    } else if (e == EINVAL && S_ISDIR(s.st_mode)) {
        su_error(0, 0, "cannot move %s to a subdirectory of itself, %s",
                 su_quoteaf(src), su_quoteaf(dst));
    } else if ((e == ENOTEMPTY || e == EEXIST) && dexists && S_ISDIR(dl.st_mode)) {
        su_error(0, ENOTEMPTY, "cannot overwrite %s", su_quoteaf(dst));
    } else {
        su_error(0, e, "cannot move %s to %s", su_quoteaf(src), su_quoteaf(dst));
    }
    if (top && rc == 0) {
        struct stat now;
        if (lstat(dst, &now) == 0) cpx_tab_add(&cpx_made, now.st_dev, now.st_ino, dst);
    }
out:
    free(bk);
    free(src);
    return rc;
}

static int do_mv(int argc, char **argv) {
    enum { BACKUP_OPT = 0x100, CONTEXT_OPT, DEBUG_OPT, EXCHANGE_OPT, NO_COPY_OPT, STRIP_OPT,
           UPDATE_OPT };
    static const struct su_option longs[] = {
        { "backup", SU_OPT_ARG, 0, BACKUP_OPT },
        { "context", SU_NO_ARG, 0, CONTEXT_OPT },
        { "debug", SU_NO_ARG, 0, DEBUG_OPT },
        { "exchange", SU_NO_ARG, 0, EXCHANGE_OPT },
        { "force", SU_NO_ARG, 0, 'f' },
        { "interactive", SU_NO_ARG, 0, 'i' },
        { "no-clobber", SU_NO_ARG, 0, 'n' },
        { "no-copy", SU_NO_ARG, 0, NO_COPY_OPT },
        { "no-target-directory", SU_NO_ARG, 0, 'T' },
        { "strip-trailing-slashes", SU_NO_ARG, 0, STRIP_OPT },
        { "suffix", SU_REQ_ARG, 0, 'S' },
        { "target-directory", SU_REQ_ARG, 0, 't' },
        { "update", SU_OPT_ARG, 0, UPDATE_OPT },
        { "verbose", SU_NO_ARG, 0, 'v' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    struct mvx_opts o;
    memset(&o, 0, sizeof o);
    int want_backup = 0, no_target = 0, c;
    const char *backup_arg = 0, *suffix = 0, *target = 0;
    while ((c = su_getopt_long(argc, argv, "bfinS:t:TuvZ", longs, 0)) != -1) {
        switch (c) {
        case 'b': want_backup = 1; break;
        case BACKUP_OPT: want_backup = 1; backup_arg = su_optarg; break;
        case CONTEXT_OPT: case 'Z': break;
        case DEBUG_OPT: o.verbose = 1; break;
        case EXCHANGE_OPT: o.exchange = 1; break;
        case 'f': o.interactive = CPX_I_FORCE; break;
        case 'i': o.interactive = CPX_I_ASK; break;
        case 'n': o.interactive = CPX_I_NO; break;
        case NO_COPY_OPT: o.no_copy = 1; break;
        case 'T': no_target = 1; break;
        case STRIP_OPT: o.strip = 1; break;
        case 'S': want_backup = 1; suffix = su_optarg; break;
        case 't':
            if (target) su_error(1, 0, "multiple target directories specified");
            target = su_optarg;
            break;
        case 'u': o.update = CPX_UP_OLDER; break;
        case UPDATE_OPT:
            o.update = su_optarg ? rmf_argmatch("--update", su_optarg, cpx_update_names,
                                                cpx_update_vals)
                                 : CPX_UP_OLDER;
            break;
        case 'v': o.verbose = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [-T] SOURCE DEST\n"
                   "  or:  %s [OPTION]... SOURCE... DIRECTORY\n"
                   "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n"
                   "Rename SOURCE to DEST, or move each SOURCE into DIRECTORY.\n"
                   "\n"
                   "An option's argument, when it must have one, is needed by its short\n"
                   "form too.\n"
                   "      --backup[=CONTROL]       keep a backup of each DEST replaced\n"
                   "  -b                           as --backup, CONTROL from VERSION_CONTROL\n"
                   "      --debug                  say how each file is moved (and -v)\n"
                   "      --exchange               swap SOURCE and DEST\n"
                   "  -f, --force                  never ask before overwriting\n"
                   "  -i, --interactive            ask before overwriting\n"
                   "  -n, --no-clobber             never overwrite an existing file\n"
                   "Of -i, -f and -n, the last one given is the one that counts.\n"
                   "      --no-copy                fail, rather than copy, when a rename\n"
                   "                                 cannot be done\n"
                   "      --strip-trailing-slashes  drop trailing slashes from each SOURCE\n"
                   "  -S, --suffix=SUFFIX          the suffix of a simple backup\n"
                   "  -t, --target-directory=DIRECTORY  move every SOURCE into DIRECTORY\n"
                   "  -T, --no-target-directory    DEST is a file name, never a directory to\n"
                   "                                 move into\n"
                   "      --update[=UPDATE]        which existing files to replace:\n"
                   "                                 UPDATE={all,none,none-fail,older(default)}\n"
                   "  -u                           as --update[=older]\n"
                   "  -v, --verbose                say what is being done\n"
                   "  -Z, --context                give DEST the default SELinux context\n"
                   "                                 (there is no SELinux here: nothing)\n"
                   "      --help                   show this help and exit\n"
                   "      --version                show the version and exit\n"
                   "\n"
                   "UPDATE: all (the default without --update) replaces every existing DEST;\n"
                   "none replaces none and is not a failure; none-fail replaces none and is\n"
                   "one, saying so; older (the default of --update) replaces a DEST older\n"
                   "than its SOURCE.\n"
                   "\n"
                   "A simple backup's suffix is '~' unless --suffix or SIMPLE_BACKUP_SUFFIX\n"
                   "gives another. --backup's CONTROL, or VERSION_CONTROL, is one of:\n"
                   "  none, off       no backups at all, even with --backup\n"
                   "  numbered, t     numbered backups\n"
                   "  existing, nil   numbered where numbered backups exist already, else\n"
                   "                    simple\n"
                   "  simple, never   simple backups always\n",
                   su_name, su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    o.backup = cpx_backup_choice(want_backup, backup_arg);
    if (o.backup != CPX_BK_NONE && (o.exchange || o.interactive == CPX_I_NO ||
                                    o.update == CPX_UP_NONE || o.update == CPX_UP_NONE_FAIL)) {
        su_error(0, 0, "cannot combine --backup with --exchange, -n, or --update=none-fail");
        su_try_help(1);
    }
    o.suffix = cpx_suffix(suffix);
    if (o.interactive == CPX_I_NO) o.update = CPX_UP_ALL;
    else if (o.update == CPX_UP_NONE || o.update == CPX_UP_NONE_FAIL) o.interactive = CPX_I_UNSPEC;
    o.umask_v = umask(0);
    umask(o.umask_v);
    o.stdin_tty = isatty(0);

    int n = argc - su_optind;
    char **ops = argv + su_optind;
    if (n <= 0) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    if (n == 1 && !target) {
        su_error(0, 0, "missing destination file operand after %s", su_quoteaf(ops[0]));
        su_try_help(1);
    }
    if (target && no_target)
        su_error(1, 0, "cannot combine --target-directory (-t) and --no-target-directory (-T)");
    if (no_target && n > 2) {
        su_error(0, 0, "extra operand %s", su_quoteaf(ops[2]));
        su_try_help(1);
    }
    int prev = 0;
    if (target) {
        cpx_target_check(target, 1);
    } else if (!no_target) {
        struct stat st;
        if (n > 2) {
            cpx_target_check(ops[n - 1], 0);
            target = ops[--n];
        } else {
            /* GNU's first try: the rename itself, replacing nothing */
            if (!o.exchange) {
                if (mvx_rename(ops[0], ops[1], RENAME_NOREPLACE) == 0) {
                    if (o.verbose)
                        printf("renamed %s -> %s\n", su_quoteaf(ops[0]), su_quoteaf(ops[1]));
                    return 0;
                }
                prev = errno == EEXIST ? 0 : errno;
            }
            if (stat(ops[1], &st) == 0 && S_ISDIR(st.st_mode)) target = ops[--n];
        }
    }
    if (!target) return mvx_move(&o, ops[0], ops[1], prev, 1);
    int rc = 0;
    for (int i = 0; i < n; i++) {
        char *src = cpx_dup(ops[i]);
        if (o.strip) {
            size_t l = strlen(src);
            while (l > 1 && src[l - 1] == '/') src[--l] = 0;
        }
        char *dst = mvx_into(target, src);
        rc |= mvx_move(&o, src, dst, 0, 1);
        free(dst);
        free(src);
    }
    return rc;
}
