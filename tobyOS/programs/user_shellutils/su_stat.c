/* su_stat.c -- stat, as GNU coreutils 9.7's (logs/utilgate/stat.sh).
 *
 *   - The default report, field for field and space for space; -t the
 *     terse line; -c FORMAT (a newline after each) or --printf FORMAT
 *     (backslash escapes, no newline). Every directive of GNU's: %a %A %b
 *     %B %C %d %D %Hd %Ld %f %F %g %G %h %i %m %n %N %o %s %r %R %Hr %Lr
 *     %t %T %u %U %w %W %x %X %y %Y %z %Z, with printf's flags, width and
 *     precision (%#a 0644, %#f 0x81a4, %.3Y fractional seconds); an
 *     unknown one prints "?", a trailing % itself.
 *   - -f: the file system -- %a %b %c %d %f %i %l %n %s %S %t %T, its own
 *     default report and terse line.
 *   - -L follows a link; %N quotes the name shell style, and a link's
 *     target after " -> ".
 *   - "cannot statx 'F'" (GNU 9.7's word on Linux), "--cached" checked
 *     against GNU's list.
 *
 * tobyOS keeps whole seconds and no birth time, so times print with
 * .000000000 and %w is "-"; it has no time zones, so local time is UTC
 * (+0000). There is no SELinux: %C is GNU's "failed to get security
 * context" and "?".
 *
 * It knew %n %s %f %a %u %g %F only, said "0" for %i, printed a
 * three-line report of its own, and took -L --printf --cached as nothing. */

#ifdef SU_HOST
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#else
#include <sys/sysmacros.h>
#endif

struct st_fs {
    unsigned long long bsize, blocks, bfree, bavail, files, ffree;
    unsigned long type, namelen;
};

static int st_statfs(const char *path, struct st_fs *o) {
#ifdef SU_HOST
    struct statfs s;
    if (statfs(path, &s) != 0) return -1;
    o->bsize = (unsigned long long)s.f_bsize;
    o->blocks = s.f_blocks; o->bfree = s.f_bfree; o->bavail = s.f_bavail;
    o->files = s.f_files; o->ffree = s.f_ffree;
    o->type = (unsigned long)s.f_type; o->namelen = (unsigned long)s.f_namelen;
    return 0;
#else
    struct abi_statfs s;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"((long)ABI_SYS_STATFS), "D"(path), "S"(&s)
                     : "rcx", "r11", "memory");
    if (r < 0) { errno = (int)-r; return -1; }
    o->bsize = s.bsize; o->blocks = s.blocks; o->bfree = s.bfree; o->bavail = s.bavail;
    o->files = s.files; o->ffree = s.ffree;
    o->type = s.type_magic; o->namelen = s.namelen;
    return 0;
#endif
}

static const char *st_fstype(unsigned long t, char *buf, size_t cap) {
    static const struct { unsigned long m; const char *n; } k[] = {
        { 0x01021994, "tmpfs" }, { 0xEF53, "ext2/ext3" }, { 0x858458f6, "ramfs" },
        { 0x9fa0, "proc" }, { 0x62656572, "sysfs" }, { 0x1373, "devfs" },
        { 0x4d44, "msdos" }, { 0x5346544e, "ntfs" }, { 0x9123683e, "btrfs" },
        { 0x58465342, "xfs" }, { 0x137f, "minix" }, { 0x138f, "minix (30 char.)" },
        { 0x2468, "minix v2" }, { 0x2478, "minix v2 (30 char.)" }, { 0x4d5a, "minix3" },
        { 0x6969, "nfs" }, { 0x794c7630, "overlayfs" }, { 0x73717368, "squashfs" },
        { 0x65735546, "fuseblk" }, { 0x65735543, "fusectl" }, { 0x27e0eb, "cgroupfs" },
        { 0x63677270, "cgroup2fs" }, { 0x64626720, "debugfs" }, { 0x74726163, "tracefs" },
        { 0x1cd1, "devpts" }, { 0x9660, "isofs" }, { 0x15013346, "udf" },
        { 0x2fc12fc1, "zfs" }, { 0xcafe4a11, "bpf_fs" }, { 0x5dca2df5, "sdcardfs" },
        { 0x43415d53, "smackfs" }, { 0x73636673, "securityfs" }, { 0x6e736673, "nsfs" },
        { 0xde5e81e4, "efivarfs" }, { 0xf2f52010, "f2fs" }, { 0x2011bab0, "exfat" },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (k[i].m == t) return k[i].n;
    snprintf(buf, cap, "UNKNOWN (0x%lx)", t);
    return buf;
}

static const char *st_typename(const struct stat *st) {
    mode_t m = st->st_mode;
    if (S_ISREG(m)) return st->st_size == 0 ? "regular empty file" : "regular file";
    if (S_ISDIR(m)) return "directory";
    if (S_ISCHR(m)) return "character special file";
    if (S_ISBLK(m)) return "block special file";
    if (S_ISFIFO(m)) return "fifo";
    if (S_ISLNK(m)) return "symbolic link";
    if (S_ISSOCK(m)) return "socket";
    return "weird file";
}

static void st_modestr(mode_t m, char out[11]) {
    out[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISBLK(m) ? 'b' : S_ISFIFO(m) ? 'p'
           : S_ISLNK(m) ? 'l' : S_ISSOCK(m) ? 's' : S_ISREG(m) ? '-' : '?';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & 04000) out[3] = (m & 0100) ? 's' : 'S';
    if (m & 02000) out[6] = (m & 0010) ? 's' : 'S';
    if (m & 01000) out[9] = (m & 0001) ? 't' : 'T';
    out[10] = 0;
}

/* NAME of id ID in /etc/passwd (FIELD 2) or /etc/group, or "UNKNOWN" */
static const char *st_idname(const char *db, unsigned long id, char *out, size_t cap) {
    FILE *f = fopen(db, "r");
    snprintf(out, cap, "UNKNOWN");
    if (!f) return out;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':');
        if (!c1) continue;
        char *c2 = strchr(c1 + 1, ':');
        if (!c2) continue;
        char *e;
        unsigned long v = strtoul(c2 + 1, &e, 10);
        if (e == c2 + 1 || *e != ':' || v != id) continue;
        *c1 = 0;
        snprintf(out, cap, "%s", line);
        break;
    }
    fclose(f);
    return out;
}

/* "YYYY-MM-DD HH:MM:SS.000000000 +0000" for T, UTC */
static void st_time(time_t t, long ns, char *out, size_t cap) {
    long long s = (long long)t;
    long long days = s >= 0 ? s / 86400 : -((-s + 86399) / 86400);
    long long rem = s - days * 86400;
    /* civil from days (Howard Hinnant's algorithm) */
    long long z = days + 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153;
    long long d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    snprintf(out, cap, "%04lld-%02lld-%02lld %02lld:%02lld:%02lld.%09ld +0000",
             y, m, d, rem / 3600, rem / 60 % 60, rem % 60, ns);
}

struct st_file {
    const char *name;
    struct stat st;
    int follow;
    int is_fs;
    struct st_fs fs;
};

static int st_status;

/* one directive: CONV (and its H/L prefix) of F, with the printf SPEC
 * (flags, width, precision) it came with */
static void st_dir(struct sn_sb *out, const struct st_file *f, char pfx, char conv,
                   const char *spec, int prec) {
    char fmt[64], buf[512], tmp[128];
    const struct stat *st = &f->st;
    enum { STR, UNS, SGN, HEX, OCT } kind = STR;
    unsigned long long v = 0;
    const char *s = "?";
    if (f->is_fs) {
        const struct st_fs *fs = &f->fs;
        switch (conv) {
        case 'a': kind = UNS; v = fs->bavail; break;
        case 'b': kind = UNS; v = fs->blocks; break;
        case 'c': kind = UNS; v = fs->files; break;
        case 'd': kind = UNS; v = fs->ffree; break;
        case 'f': kind = UNS; v = fs->bfree; break;
        case 'i': kind = HEX; v = 0; break;
        case 'l': kind = UNS; v = fs->namelen; break;
        case 'n': s = f->name; break;
        case 's': case 'S': kind = UNS; v = fs->bsize; break;
        case 't': kind = HEX; v = fs->type; break;
        case 'T': s = st_fstype(fs->type, tmp, sizeof tmp); break;
        default: s = "?"; break;
        }
    } else switch (conv) {
    case 'a': kind = OCT; v = st->st_mode & 07777; break;
    case 'A': st_modestr(st->st_mode, tmp); s = tmp; break;
    case 'b': kind = UNS; v = (unsigned long long)st->st_blocks; break;
    case 'B': kind = UNS; v = 512; break;
    case 'C':
        su_error(0, ENODATA, "failed to get security context of %s", su_quoteaf(f->name));
        st_status = 1;
        s = "?";
        break;
    case 'd':
        kind = UNS;
        v = pfx == 'H' ? major(st->st_dev) : pfx == 'L' ? minor(st->st_dev) : (unsigned long long)st->st_dev;
        break;
    case 'D': kind = HEX; v = (unsigned long long)st->st_dev; break;
    case 'f': kind = HEX; v = st->st_mode; break;
    case 'F': s = st_typename(st); break;
    case 'g': kind = UNS; v = st->st_gid; break;
    case 'G': s = st_idname("/etc/group", st->st_gid, tmp, sizeof tmp); break;
    case 'h': kind = UNS; v = st->st_nlink; break;
    case 'i': kind = UNS; v = (unsigned long long)st->st_ino; break;
    case 'm': s = "/"; break;
    case 'n': s = f->name; break;
    case 'N': {
        snprintf(buf, sizeof buf, "%s", su_quoteaf(f->name));
        if (S_ISLNK(st->st_mode) && !f->follow) {
            char tgt[4096];
            long n = readlink(f->name, tgt, sizeof tgt - 1);
            if (n >= 0) {
                tgt[n] = 0;
                size_t l = strlen(buf);
                snprintf(buf + l, sizeof buf - l, " -> %s", su_quoteaf(tgt));
            }
        }
        s = buf;
        break;
    }
    case 'o': kind = UNS; v = (unsigned long long)st->st_blksize; break;
    case 's': kind = UNS; v = (unsigned long long)st->st_size; break;
    case 'r':
        kind = UNS;
        v = pfx == 'H' ? major(st->st_rdev) : pfx == 'L' ? minor(st->st_rdev) : (unsigned long long)st->st_rdev;
        break;
    case 'R': kind = HEX; v = (unsigned long long)st->st_rdev; break;
    case 't': kind = HEX; v = major(st->st_rdev); break;
    case 'T': kind = HEX; v = minor(st->st_rdev); break;
    case 'u': kind = UNS; v = st->st_uid; break;
    case 'U': s = st_idname("/etc/passwd", st->st_uid, tmp, sizeof tmp); break;
    case 'w': s = "-"; break;
    case 'W': kind = UNS; v = 0; break;
    /* to the nanosecond (finding 239: it printed .000000000 always) */
    case 'x': st_time(st->st_atime, st->st_atim.tv_nsec, tmp, sizeof tmp); s = tmp; break;
    case 'y': st_time(st->st_mtime, st->st_mtim.tv_nsec, tmp, sizeof tmp); s = tmp; break;
    case 'z': st_time(st->st_ctime, st->st_ctim.tv_nsec, tmp, sizeof tmp); s = tmp; break;
    case 'X': case 'Y': case 'Z': {
        time_t t = conv == 'X' ? st->st_atime : conv == 'Y' ? st->st_mtime : st->st_ctime;
        long tn = conv == 'X' ? st->st_atim.tv_nsec : conv == 'Y' ? st->st_mtim.tv_nsec
                                                     : st->st_ctim.tv_nsec;
        if (prec > 0) {
            char frac[16];
            snprintf(frac, sizeof frac, "%09ld", tn);
            snprintf(buf, sizeof buf, "%lld.%.*s", (long long)t, prec > 9 ? 9 : prec, frac);
            for (int i = 9; i < prec; i++) strcat(buf, "0");
            s = buf;
        } else {
            kind = SGN;             /* before the epoch is negative */
            v = (unsigned long long)(long long)t;
        }
        break;
    }
    default: s = "?"; break;
    }
    if (kind == STR) {
        /* a string takes '-' and the width; the other flags mean nothing */
        const char *q = spec;
        int left = 0;
        while (*q && strchr("-#+ 0'", *q)) { if (*q == '-') left = 1; q++; }
        snprintf(fmt, sizeof fmt, "%%%s%ss", left ? "-" : "", q);
        int len = snprintf(0, 0, fmt, s);
        char *bb = (char *)malloc((size_t)len + 1);
        if (!bb) su_error(1, ENOMEM, "memory exhausted");
        snprintf(bb, (size_t)len + 1, fmt, s);
        sn_puts(out, bb);
        free(bb);
        return;
    }
    const char *c = kind == UNS ? "llu" : kind == SGN ? "lld" : kind == HEX ? "llx" : "llo";
    char spec2[64];
    size_t n = 0;
    for (const char *q = spec; *q && n + 1 < sizeof spec2; q++) {
        /* '+' and ' ' mean nothing to an unsigned conversion, "'" to any */
        if (((*q == '+' || *q == ' ') && kind != SGN) || *q == '\'') continue;
        spec2[n++] = *q;
    }
    spec2[n] = 0;
    snprintf(fmt, sizeof fmt, "%%%s%s", spec2, c);
    if (kind == SGN) snprintf(buf, sizeof buf, fmt, (long long)v);
    else snprintf(buf, sizeof buf, fmt, v);
    sn_puts(out, buf);
}

/* FORMAT for F into OUT; ESC: --printf's backslash escapes */
static void st_format(struct sn_sb *out, const char *fmt, const struct st_file *f, int esc) {
    for (const char *p = fmt; *p;) {
        if (esc && *p == '\\') {
            p++;
            if (!*p) { sn_putc(out, '\\'); break; }
            char ch = *p;
            if (ch >= '0' && ch <= '7') {
                int v = 0, k = 0;
                while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); k++; }
                sn_putc(out, (char)v);
                continue;
            }
            if (ch == 'x' && isxdigit((unsigned char)p[1])) {
                int v = 0, k = 0;
                p++;
                while (k < 2 && isxdigit((unsigned char)*p)) {
                    v = v * 16 + (isdigit((unsigned char)*p) ? *p - '0' : tolower(*p) - 'a' + 10);
                    p++; k++;
                }
                sn_putc(out, (char)v);
                continue;
            }
            const char *from = "abefnrtv\\\"", *to = "\a\b\x1b\f\n\r\t\v\\\"";
            const char *hit = strchr(from, ch);
            if (hit) sn_putc(out, to[hit - from]);
            else {
                su_error(0, 0, "warning: unrecognized escape '\\%c'", ch);
                sn_putc(out, ch);
            }
            p++;
            continue;
        }
        if (*p != '%') { sn_putc(out, *p++); continue; }
        const char *start = ++p;
        if (*p == '%') { sn_putc(out, '%'); p++; continue; }
        while (*p && strchr("-#+ 0'", *p)) p++;
        while (isdigit((unsigned char)*p)) p++;
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            int any = 0;
            while (isdigit((unsigned char)*p)) { prec = prec * 10 + (*p++ - '0'); any = 1; }
            if (!any) prec = 9;
        }
        char spec[64];
        size_t sl = (size_t)(p - start);
        if (sl >= sizeof spec) sl = sizeof spec - 1;
        memcpy(spec, start, sl);
        spec[sl] = 0;
        if (!*p) { sn_putc(out, '%'); sn_puts(out, spec); break; }
        char pfx = 0;
        if ((*p == 'H' || *p == 'L') && (p[1] == 'd' || p[1] == 'r')) pfx = *p++;
        char conv = *p++;
        /* a precision is for the times (passed apart); dropped from the spec */
        char *dot = strchr(spec, '.');
        if (dot) *dot = 0;
        st_dir(out, f, pfx, conv, spec, strchr("XYZ", conv) ? prec : -1);
    }
}

static int do_stat(int argc, char **argv) {
    enum { PRINTF_OPT = 0x100, CACHED_OPT };
    static const struct su_option longs[] = {
        { "dereference", SU_NO_ARG, 0, 'L' },
        { "file-system", SU_NO_ARG, 0, 'f' },
        { "format", SU_REQ_ARG, 0, 'c' },
        { "printf", SU_REQ_ARG, 0, PRINTF_OPT },
        { "terse", SU_NO_ARG, 0, 't' },
        { "cached", SU_REQ_ARG, 0, CACHED_OPT },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int follow = 0, fs = 0, terse = 0, esc = 0, c;
    const char *fmt = 0;
    while ((c = su_getopt_long(argc, argv, "c:fLt", longs, 0)) != -1) {
        switch (c) {
        case 'L': follow = 1; break;
        case 'f': fs = 1; break;
        case 't': terse = 1; break;
        case 'c': fmt = su_optarg; esc = 0; break;
        case PRINTF_OPT: fmt = su_optarg; esc = 1; break;
        case CACHED_OPT: {
            static const char *const modes[] = { "default", "never", "always" };
            int ok = 0;
            for (int i = 0; i < 3; i++) if (!strcmp(su_optarg, modes[i])) ok = 1;
            if (!ok) {
                fflush(stdout);
                fprintf(stderr, "%s: invalid argument %s for %s\nValid arguments are:\n",
                        su_short, su_quote(su_optarg), su_quote("--cached"));
                for (int i = 0; i < 3; i++) fprintf(stderr, "  - %s\n", su_quote(modes[i]));
                su_try_help(1);
            }
            break;
        }
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE...\n"
                   "Show the status of each FILE, or of the file system it is on.\n"
                   "\n"
                   "  -L, --dereference     follow links\n"
                   "  -f, --file-system     the file system, not the file\n"
                   "      --cached=MODE     taken (default, never, always); there is\n"
                   "                        no attribute cache to choose about here\n"
                   "  -c  --format=FORMAT   FORMAT instead of the report, a newline after\n"
                   "      --printf=FORMAT   FORMAT with backslash escapes, no newline\n"
                   "  -t, --terse           the terse one-line form\n"
                   "      --help            show this help and exit\n"
                   "      --version         show the version and exit\n"
                   "\n"
                   "FORMAT's directives are GNU stat's: %%a %%A %%b %%B %%C %%d %%D %%Hd %%Ld\n"
                   "%%f %%F %%g %%G %%h %%i %%m %%n %%N %%o %%s %%r %%R %%Hr %%Lr %%t %%T %%u\n"
                   "%%U %%w %%W %%x %%X %%y %%Y %%z %%Z; with -f, %%a %%b %%c %%d %%f %%i %%l\n"
                   "%%n %%s %%S %%t %%T.\n",
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
    for (int i = su_optind; i < argc; i++) {
        struct st_file f;
        memset(&f, 0, sizeof f);
        f.name = argv[i];
        f.follow = follow;
        f.is_fs = fs;
        if (fs) {
            if (st_statfs(f.name, &f.fs) != 0) {
                su_error(0, errno, "cannot read file system information for %s",
                         su_quoteaf(f.name));
                st_status = 1;
                continue;
            }
        } else {
            int r = !strcmp(f.name, "-") ? fstat(0, &f.st)
                  : follow ? stat(f.name, &f.st) : lstat(f.name, &f.st);
            if (r != 0) {
                su_error(0, errno, "cannot statx %s", su_quoteaf(f.name));
                st_status = 1;
                continue;
            }
        }
        struct sn_sb out = { 0, 0, 0 };
        sn_puts(&out, "");
        if (fmt) {
            st_format(&out, fmt, &f, esc);
            if (!esc) sn_putc(&out, '\n');
        } else if (terse) {
            st_format(&out, fs ? "%n %i %l %t %s %S %b %f %a %c %d\n"
                               : "%n %s %b %f %u %g %D %i %h %t %T %X %Y %Z %W %o\n", &f, 0);
        } else if (fs) {
            st_format(&out, "  File: \"%n\"\n"
                            "    ID: %-8i Namelen: %-7l Type: %T\n"
                            "Block size: %-10s Fundamental block size: %S\n"
                            "Blocks: Total: %-10b Free: %-10f Available: %a\n"
                            "Inodes: Total: %-10c Free: %d\n", &f, 0);
        } else {
            /* the name and a link's target unquoted, as GNU's report */
            sn_puts(&out, "  File: ");
            sn_puts(&out, f.name);
            if (S_ISLNK(f.st.st_mode) && !follow) {
                char tgt[4096];
                long n = readlink(f.name, tgt, sizeof tgt - 1);
                if (n >= 0) { tgt[n] = 0; sn_puts(&out, " -> "); sn_puts(&out, tgt); }
            }
            sn_putc(&out, '\n');
            st_format(&out, "  Size: %-10s\tBlocks: %-10b IO Block: %-6o %F\n", &f, 0);
            if (S_ISCHR(f.st.st_mode) || S_ISBLK(f.st.st_mode))
                st_format(&out, "Device: %Hd,%Ld\tInode: %-11i Links: %-5h Device type: %Hr,%Lr\n", &f, 0);
            else
                st_format(&out, "Device: %Hd,%Ld\tInode: %-11i Links: %h\n", &f, 0);
            st_format(&out, "Access: (%04a/%10.10A)  Uid: (%5u/%8U)   Gid: (%5g/%8G)\n"
                            "Access: %x\nModify: %y\nChange: %z\n Birth: %w\n", &f, 0);
        }
        fwrite(out.s, 1, out.n, stdout);
        free(out.s);
    }
    if (fflush(stdout) != 0) su_error(1, errno, "write error");
    return st_status;
}
