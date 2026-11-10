/* su_df.c -- df, as GNU coreutils 9.7's (logs/utilgate/staging/df.sh):
 * df [OPTION]... [FILE]...
 *
 * Everything here was measured in WSL by giving GNU's df a mount table of
 * the fixture's own (tmpfs mounts in a user and mount namespace, and a
 * /proc/self/mountinfo written by hand over a tmpfs /proc), so each rule
 * below has a case that shows it.
 *
 * THE TABLE is /proc/self/mountinfo (its octal escapes decoded; a line
 * that does not parse is skipped), else /proc/self/mounts; neither:
 * "cannot read table of mounted file systems: ENOENT" -- an empty
 * mountinfo is the same message with no errno. With FILEs that is only a
 * "Warning: " and every FILE gets a "-" row -- unless -a.
 *
 * LISTING (no FILE). Each entry's mount point is stat()ed: one that
 * cannot be is shown only with -a, as a "-" row. Two entries on one
 * device are one file system, and one of them is kept, in table order:
 * the newer replaces the kept one when its source has a '/' and the kept
 * one's does not, or when its mount point is shorter (and its mountinfo
 * root no longer), or when it is the same mount point with another source
 * (an over-mount). Two remote entries with different sources are both
 * kept (unless --total). Without -a an entry that would not be shown --
 * a pseudo file system, a type -t/-x leave out, a remote one under -l --
 * never replaces a kept one, but is kept when it is the first. Shown
 * then: the kept entries, less (without -a) the pseudo types and the file
 * systems with no blocks; with -a every entry, a discarded one as a "-"
 * row when its source differs from the kept one's.
 *   Pseudo: autofs proc subfs debugfs devpts fusectl fuse.portal mqueue
 *   rpc_pipefs sysfs devfs kernfs ignore none devtmpfs squashfs.
 *   Remote: a source with ':', "-hosts", "//..." for smbfs smb3 cifs, and
 *   the types afs auristorfs coda.
 *
 * A FILE is stat()ed ("FILE: strerror"); a block device is the entry
 * whose source it is; otherwise among the entries on FILE's device, the
 * one whose mount point holds the canonical path (the longest such),
 * else the last. With none, the row is "-", Type "-", and the mount point
 * is found by climbing from FILE while the device stays the same; that
 * row is never left out. FILE rows are not deduplicated, and pseudo or
 * empty file systems are shown; -t -x -l still apply to them.
 *
 * SIZES. -B SIZE (-B1K, -BM, -B 2MiB, human-readable, si), -k, -m, -h,
 * -H: the last wins; else -P's 1024 (512 under POSIXLY_CORRECT); else
 * DF_BLOCK_SIZE, BLOCK_SIZE, BLOCKSIZE (an invalid one is ignored); else
 * 1024 (512 under POSIXLY_CORRECT). A SIZE with no digits (-BK, -BMB,
 * -BKiB) prints each value with that suffix. Every value is rounded up;
 * -h -H show 3 digits at most: "4.8M" below 10, "48M" above, "1.0G" when
 * rounding reaches the next unit. Use% is used/(used+avail), rounded up,
 * "-" when both are 0. The header names the unit: "1K-blocks", "1kB" (a
 * size that is not a multiple of 1024 is shown in powers of 1000 with a
 * B), "1.1kB", "1M-blocks"; -P "1024-blocks" and "Capacity". Columns are
 * as wide as their widest cell (Filesystem at least 14, Size Used Avail
 * 5), one space apart, text left and numbers right, the last unpadded. A
 * control character in a name is '?'; a cell with a byte past ASCII is
 * printed as it is, unpadded, and does not widen its column.
 *
 * --output[=FIELDS] (source fstype itotal iused iavail ipcent size used
 * avail pcent file target) cannot be had with -i -T -P; File is FILE as
 * given, "-" when listing. --total adds "total". -t/-x TYPE (-F is -t)
 * select by the table's type; one TYPE in both is an error. "no file
 * systems processed" when nothing was shown and no FILE failed.
 *
 * The old df knew -a -h -k -T, named every file system after its type,
 * listed duplicates, and had no -B -H -i -l -P -t -x --output --total. */

#ifdef SU_HOST
#include <sys/statvfs.h>
#endif

struct sy_dfent {
    char *src, *dir, *type, *root;
    int remote, dummy;
    int ok;                 /* the mount point could be stat()ed */
    dev_t dev;
    int discarded;
};

static struct sy_dfent *sy_dft;
static int sy_dfn;

/* mountinfo's escapes: \ooo */
static char *sy_dfunesc(const char *s) {
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1), *q = o;
    if (!o) su_error(1, ENOMEM, "memory exhausted");
    while (*s) {
        if (s[0] == '\\' && s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
            s[3] >= '0' && s[3] <= '7') {
            *q++ = (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0'));
            s += 4;
        } else *q++ = *s++;
    }
    *q = 0;
    return o;
}

static int sy_dfin(const char *s, const char *const *list) {
    for (int i = 0; list[i]; i++) if (!strcmp(s, list[i])) return 1;
    return 0;
}

static const char *const sy_dfdummy[] = {
    "autofs", "proc", "subfs", "debugfs", "devpts", "fusectl", "fuse.portal",
    "mqueue", "rpc_pipefs", "sysfs", "devfs", "kernfs", "ignore", "none",
    "devtmpfs", "squashfs", 0
};

static void sy_dfadd(char *src, char *dir, char *type, char *root) {
    static int cap;
    if (sy_dfn == cap) {
        cap = cap ? cap * 2 : 64;
        sy_dft = (struct sy_dfent *)realloc(sy_dft, (size_t)cap * sizeof *sy_dft);
        if (!sy_dft) su_error(1, ENOMEM, "memory exhausted");
    }
    struct sy_dfent *e = &sy_dft[sy_dfn++];
    memset(e, 0, sizeof *e);
    e->src = sy_dfunesc(src);
    e->dir = sy_dfunesc(dir);
    e->type = sy_dfunesc(type);
    e->root = root ? sy_dfunesc(root) : 0;
    static const char *const rtypes[] = { "afs", "auristorfs", "coda", 0 };
    static const char *const smb[] = { "smbfs", "smb3", "cifs", 0 };
    e->remote = strchr(e->src, ':') != 0 || !strcmp(e->src, "-hosts") ||
                (e->src[0] == '/' && e->src[1] == '/' && sy_dfin(e->type, smb)) ||
                sy_dfin(e->type, rtypes);
    e->dummy = sy_dfin(e->type, sy_dfdummy);
}

/* the table; 0, or -1 with errno set (0 for an empty one) */
static int sy_dfread(void) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    int info = f != 0;
    if (!f) f = fopen("/proc/self/mounts", "r");
    if (!f) return -1;
    char *line = 0;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *w[64];
        int n = 0;
        for (char *t = strtok(line, " "); t && n < 64; t = strtok(0, " ")) w[n++] = t;
        if (info) {
            /* ID PARENT MAJ:MIN ROOT POINT OPTS [OPTIONAL...] - TYPE SOURCE SUPER */
            int dash = -1;
            for (int i = 6; i < n; i++) if (!strcmp(w[i], "-")) { dash = i; break; }
            if (n < 6 || dash < 0 || dash + 2 >= n || !strchr(w[2], ':')) continue;
            sy_dfadd(w[dash + 2], w[4], w[dash + 1], w[3]);
        } else {
            if (n < 3) continue;
            sy_dfadd(w[0], w[1], w[2], 0);
        }
    }
    free(line);
    fclose(f);
    if (sy_dfn == 0) { errno = 0; return -1; }
    return 0;
}

/* ---- the numbers -------------------------------------------------------- */

struct sy_dffs {
    uintmax_t frsize, blocks, bfree, bavail, files, ffree;
};

#ifndef SU_HOST
static long sy_dfsc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
#endif

static int sy_dfstatfs(const char *path, struct sy_dffs *fs) {
#ifdef SU_HOST
    struct statvfs v;
    if (statvfs(path, &v) != 0) return -1;
    fs->frsize = v.f_frsize ? v.f_frsize : v.f_bsize;
    fs->blocks = v.f_blocks;
    fs->bfree = v.f_bfree;
    fs->bavail = v.f_bavail;
    fs->files = v.f_files;
    fs->ffree = v.f_ffree;
#else
    struct abi_statfs v;
    memset(&v, 0, sizeof v);
    long rc = sy_dfsc2(ABI_SYS_STATFS, (long)path, (long)&v);
    if (rc < 0) { errno = (int)-rc; return -1; }
    fs->frsize = v.bsize;
    fs->blocks = v.blocks;
    fs->bfree = v.bfree;
    fs->bavail = v.bavail;
    fs->files = v.files;
    fs->ffree = v.ffree;
#endif
    return 0;
}

/* the block size: human 1024 or 1000, or a unit, printed with SFX ("K",
 * "kB", "MiB") when the SIZE had no digits */
struct sy_dfbs {
    int human;
    uintmax_t unit;
    char sfx[8];
};

/* VALUE in human form, rounded up: N (below BASE), "4.8M", "48M" */
static void sy_dfhuman(uintmax_t v, unsigned base, int exact_int, int with_b,
                       char *out, size_t cap) {
    const char *letters = base == 1024 ? "KMGTPEZY" : "kMGTPEZY";
    if (v < base) {
        snprintf(out, cap, "%ju%s", v, with_b ? "B" : "");
        return;
    }
    uintmax_t p = base;
    int e = 0;
    while (v / p >= base && e < 7) { p *= base; e++; }
    uintmax_t q = v / p, r = v % p;
    const char *b = with_b ? "B" : "";
    if (exact_int && r == 0) {
        snprintf(out, cap, "%ju%c%s", q, letters[e], b);
        return;
    }
    if (q < 10) {
        uintmax_t tenths = q * 10 + (r * 10 + p - 1) / p;
        if (tenths < 100) {
            snprintf(out, cap, "%ju.%ju%c%s", tenths / 10, tenths % 10, letters[e], b);
            return;
        }
        snprintf(out, cap, "10%c%s", letters[e], b);
        return;
    }
    uintmax_t n = q + (r != 0);
    if (n >= base) snprintf(out, cap, "1.0%c%s", letters[e + 1], b);
    else snprintf(out, cap, "%ju%c%s", n, letters[e], b);
}

/* -B's SIZE: 0, or 1 not a size, 2 a bad suffix, 3 too large */
static int sy_dfbsparse(const char *arg, struct sy_dfbs *bs) {
    memset(bs, 0, sizeof *bs);
    size_t al = strlen(arg);
    if (al && !strncmp(arg, "human-readable", al)) { bs->human = 1024; return 0; }
    if (al && !strncmp(arg, "si", al)) { bs->human = 1000; return 0; }
    const char *p = arg;
    if (*p == '\'') p++;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    uintmax_t v = 1;
    int digits = 0, over = 0;
    if (isdigit((unsigned char)*p)) {
        v = 0;
        for (; isdigit((unsigned char)*p); p++) {
            unsigned d = (unsigned)(*p - '0');
            if (v > (UINTMAX_MAX - d) / 10) over = 1;
            else v = v * 10 + d;
        }
        digits = 1;
        if (over) return 3;
    } else if (!*p || !strchr("eEgGkKmMpPtTyYzZ", *p)) {
        return 1;
    }
    if (*p) {
        int power;
        char L;
        switch (*p) {
        case 'k': case 'K': power = 1; L = 'K'; break;
        case 'm': case 'M': power = 2; L = 'M'; break;
        case 'g': case 'G': power = 3; L = 'G'; break;
        case 't': case 'T': power = 4; L = 'T'; break;
        case 'P': power = 5; L = 'P'; break;
        case 'E': power = 6; L = 'E'; break;
        case 'Z': power = 7; L = 'Z'; break;
        case 'Y': power = 8; L = 'Y'; break;
        default: return 2;
        }
        unsigned base = 1024;
        const char *rest = p + 1;
        if (rest[0] == 'B') { base = 1000; rest++; }
        else if (rest[0] == 'i' && rest[1] == 'B') rest += 2;
        if (*rest) return 2;
        for (int i = 0; i < power; i++) {
            if (v > UINTMAX_MAX / base) return 3;
            v *= base;
        }
        if (!digits) {
            if (base == 1000) snprintf(bs->sfx, sizeof bs->sfx, "%cB", L == 'K' ? 'k' : L);
            else snprintf(bs->sfx, sizeof bs->sfx, "%c%s", L, p[1] == 'i' ? "iB" : "");
        }
    }
    if (v == 0) return 1;
    bs->unit = v;
    return 0;
}

static void sy_dfbsopt(const char *arg, const char *opt, struct sy_dfbs *bs) {
    int r = sy_dfbsparse(arg, bs);
    if (r == 0) return;
    /* the argument as it was given, in plain quotes: these messages
     * escape nothing */
    fflush(stdout);
    if (r == 1) fprintf(stderr, "%s: invalid %s argument '%s'\n", su_short, opt, arg);
    else if (r == 2) fprintf(stderr, "%s: invalid suffix in %s argument '%s'\n", su_short, opt, arg);
    else fprintf(stderr, "%s: %s argument '%s' too large\n", su_short, opt, arg);
    exit(1);
}

/* ---- the table of output ------------------------------------------------ */

enum { SY_SRC, SY_FSTYPE, SY_ITOTAL, SY_IUSED, SY_IAVAIL, SY_IPCENT, SY_SIZE,
       SY_USED, SY_AVAIL, SY_PCENT, SY_FILE, SY_TARGET, SY_NFIELD };

static const char *const sy_dffname[SY_NFIELD] = {
    "source", "fstype", "itotal", "iused", "iavail", "ipcent", "size", "used",
    "avail", "pcent", "file", "target"
};

static int sy_dffield[SY_NFIELD], sy_dfnfield;
static struct sy_dfbs sy_dfb;
static int sy_dfposix;

struct sy_dfrow { char *cell[SY_NFIELD]; };
static struct sy_dfrow *sy_dfrows;
static int sy_dfnrows, sy_dfrcap;

static char *sy_dfdup(const char *s) {
    char *d = strdup(s);
    if (!d) su_error(1, ENOMEM, "memory exhausted");
    return d;
}

/* a name for a cell: control characters as '?' */
static char *sy_dfname(const char *s) {
    char *d = sy_dfdup(s);
    for (char *p = d; *p; p++)
        if ((unsigned char)*p < 0x20 || *p == 0x7f) *p = '?';
    return d;
}

static struct sy_dfrow *sy_dfnewrow(void) {
    if (sy_dfnrows == sy_dfrcap) {
        sy_dfrcap = sy_dfrcap ? sy_dfrcap * 2 : 32;
        sy_dfrows = (struct sy_dfrow *)realloc(sy_dfrows, (size_t)sy_dfrcap * sizeof *sy_dfrows);
        if (!sy_dfrows) su_error(1, ENOMEM, "memory exhausted");
    }
    struct sy_dfrow *r = &sy_dfrows[sy_dfnrows++];
    memset(r, 0, sizeof *r);
    return r;
}

/* a size in bytes, as the block size says */
static char *sy_dfsize(uintmax_t bytes) {
    char b[64];
    if (sy_dfb.human) sy_dfhuman(bytes, (unsigned)sy_dfb.human, 0, 0, b, sizeof b);
    else {
        uintmax_t u = sy_dfb.unit;
        uintmax_t n = bytes / u + (bytes % u != 0);
        snprintf(b, sizeof b, "%ju%s", n, sy_dfb.sfx);
    }
    return sy_dfdup(b);
}

static char *sy_dfcount(uintmax_t n) {
    char b[64];
    if (sy_dfb.human) sy_dfhuman(n, (unsigned)sy_dfb.human, 0, 0, b, sizeof b);
    else snprintf(b, sizeof b, "%ju", n);
    return sy_dfdup(b);
}

static char *sy_dfpct(uintmax_t used, uintmax_t avail) {
    char b[32];
    uintmax_t t = used + avail;
    if (t == 0) return sy_dfdup("-");
    uintmax_t u100 = used * 100;
    snprintf(b, sizeof b, "%ju%%", u100 / t + (u100 % t != 0));
    return sy_dfdup(b);
}

struct sy_dfsum { uintmax_t size, used, avail, itotal, iused, iavail; int any; };
static struct sy_dfsum sy_dftot;

/* a row: SRC TYPE TARGET FILE and FS's numbers (or all "-" when FS is 0) */
static void sy_dfputrow(const char *src, const char *type, const char *target,
                        const char *file, const struct sy_dffs *fs) {
    struct sy_dfrow *r = sy_dfnewrow();
    r->cell[SY_SRC] = sy_dfname(src);
    r->cell[SY_FSTYPE] = sy_dfname(fs ? type : "-");
    r->cell[SY_TARGET] = sy_dfname(target);
    r->cell[SY_FILE] = sy_dfname(file);
    if (!fs) {
        for (int f = SY_ITOTAL; f <= SY_PCENT; f++) r->cell[f] = sy_dfdup("-");
        return;
    }
    uintmax_t used = fs->blocks >= fs->bfree ? fs->blocks - fs->bfree : 0;
    uintmax_t iused = fs->files >= fs->ffree ? fs->files - fs->ffree : 0;
    r->cell[SY_SIZE] = sy_dfsize(fs->blocks * fs->frsize);
    r->cell[SY_USED] = sy_dfsize(used * fs->frsize);
    r->cell[SY_AVAIL] = sy_dfsize(fs->bavail * fs->frsize);
    r->cell[SY_PCENT] = sy_dfpct(used, fs->bavail);
    r->cell[SY_ITOTAL] = sy_dfcount(fs->files);
    r->cell[SY_IUSED] = sy_dfcount(iused);
    r->cell[SY_IAVAIL] = sy_dfcount(fs->ffree);
    r->cell[SY_IPCENT] = sy_dfpct(iused, fs->ffree);
    sy_dftot.size += fs->blocks * fs->frsize;
    sy_dftot.used += used * fs->frsize;
    sy_dftot.avail += fs->bavail * fs->frsize;
    sy_dftot.itotal += fs->files;
    sy_dftot.iused += iused;
    sy_dftot.iavail += fs->ffree;
    sy_dftot.any = 1;
}

/* the header row */
static void sy_dfheader(struct sy_dfrow *r, int output_mode) {
    char b[64];
    r->cell[SY_SRC] = sy_dfdup("Filesystem");
    r->cell[SY_FSTYPE] = sy_dfdup("Type");
    r->cell[SY_ITOTAL] = sy_dfdup("Inodes");
    r->cell[SY_IUSED] = sy_dfdup("IUsed");
    r->cell[SY_IAVAIL] = sy_dfdup("IFree");
    r->cell[SY_IPCENT] = sy_dfdup("IUse%");
    if (sy_dfb.human) snprintf(b, sizeof b, "Size");
    else if (sy_dfposix) snprintf(b, sizeof b, "%ju-blocks", sy_dfb.unit);
    else if (sy_dfb.sfx[0]) snprintf(b, sizeof b, "1%s-blocks", sy_dfb.sfx);
    else {
        /* the base that divides the size further: both 1000 and 1024
         * are tried together, step by step, while both divide it; the one
         * that still divides when the other stops wins, and with neither,
         * powers of 1000 with a B (1024000 is "1.1MB", 1048576000
         * "1000M") */
        char h[48];
        uintmax_t q1000 = sy_dfb.unit, q1024 = sy_dfb.unit;
        int d1000, d1024;
        do {
            d1000 = q1000 % 1000 == 0; q1000 /= 1000;
            d1024 = q1024 % 1024 == 0; q1024 /= 1024;
        } while (d1000 && d1024);
        if (d1024 && !d1000) sy_dfhuman(sy_dfb.unit, 1024, 1, 0, h, sizeof h);
        else sy_dfhuman(sy_dfb.unit, 1000, 1, 1, h, sizeof h);
        snprintf(b, sizeof b, "%s-blocks", h);
    }
    r->cell[SY_SIZE] = sy_dfdup(b);
    r->cell[SY_USED] = sy_dfdup("Used");
    r->cell[SY_AVAIL] = sy_dfdup(sy_dfb.human || output_mode ? "Avail" : "Available");
    r->cell[SY_PCENT] = sy_dfdup(sy_dfposix && !sy_dfb.human ? "Capacity" : "Use%");
    r->cell[SY_FILE] = sy_dfdup("File");
    r->cell[SY_TARGET] = sy_dfdup("Mounted on");
}

static int sy_dfascii(const char *s) {
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return 0;
    return 1;
}

static void sy_dfprint(void) {
    int w[SY_NFIELD];
    for (int k = 0; k < sy_dfnfield; k++) {
        int f = sy_dffield[k];
        w[k] = f == SY_SRC ? 14 : (f == SY_SIZE || f == SY_USED || f == SY_AVAIL) ? 5 : 0;
        for (int i = 0; i < sy_dfnrows; i++) {
            const char *c = sy_dfrows[i].cell[f];
            if (!sy_dfascii(c)) continue;
            int l = (int)strlen(c);
            if (l > w[k]) w[k] = l;
        }
    }
    for (int i = 0; i < sy_dfnrows; i++) {
        for (int k = 0; k < sy_dfnfield; k++) {
            int f = sy_dffield[k];
            const char *c = sy_dfrows[i].cell[f];
            int left = f == SY_SRC || f == SY_FSTYPE || f == SY_FILE || f == SY_TARGET;
            int last = k == sy_dfnfield - 1;
            int pad = sy_dfascii(c) ? w[k] - (int)strlen(c) : 0;
            if (k) putchar(' ');
            if (left) {
                fputs(c, stdout);
                if (!last) for (int j = 0; j < pad; j++) putchar(' ');
            } else {
                for (int j = 0; j < pad; j++) putchar(' ');
                fputs(c, stdout);
            }
        }
        putchar('\n');
    }
}

/* ---- selection ----------------------------------------------------------- */

static char **sy_dfsel, **sy_dfexc;
static int sy_dfnsel, sy_dfnexc;
static int sy_dfall, sy_dflocal, sy_dftotal;

static void sy_dfpush(char ***v, int *n, const char *s) {
    *v = (char **)realloc(*v, (size_t)(*n + 1) * sizeof **v);
    if (!*v) su_error(1, ENOMEM, "memory exhausted");
    (*v)[(*n)++] = (char *)s;
}

static int sy_dftype_ok(const char *type) {
    if (sy_dfnsel) {
        int in = 0;
        for (int i = 0; i < sy_dfnsel && !in; i++) in = !strcmp(type, sy_dfsel[i]);
        if (!in) return 0;
    }
    for (int i = 0; i < sy_dfnexc; i++) if (!strcmp(type, sy_dfexc[i])) return 0;
    return 1;
}

static int sy_dfshown(const struct sy_dfent *e) {
    return sy_dftype_ok(e->type) && !(sy_dflocal && e->remote);
}

/* is mount point DIR the path P or one of its parents */
static int sy_dfholds(const char *dir, const char *p) {
    size_t l = strlen(dir);
    if (!strcmp(dir, "/")) return p[0] == '/';
    return !strncmp(dir, p, l) && (p[l] == 0 || p[l] == '/');
}

static int sy_dfdedup_find(int *devs, int nd, dev_t d) {
    for (int i = 0; i < nd; i++) if (sy_dft[devs[i]].dev == d) return i;
    return -1;
}

static void sy_dfdedup(int *kept, int *nkept) {
    int nd = 0;
    for (int i = 0; i < sy_dfn; i++) {
        struct sy_dfent *e = &sy_dft[i];
        struct stat st;
        if (stat(e->dir, &st) != 0) continue;
        e->ok = 1;
        e->dev = st.st_dev;
        int k = sy_dfdedup_find(kept, nd, e->dev);
        if (k < 0) { kept[nd++] = i; continue; }
        struct sy_dfent *K = &sy_dft[kept[k]];
        if (!sy_dfall && (e->dummy || !sy_dfshown(e))) { e->discarded = 1; continue; }
        if (!sy_dftotal && e->remote && K->remote && strcmp(e->src, K->src)) {
            kept[k] = i;            /* both stay */
            continue;
        }
        int below = K->root && e->root && strlen(K->root) < strlen(e->root);
        if ((strchr(e->src, '/') && !strchr(K->src, '/')) ||
            (strlen(K->dir) > strlen(e->dir) && !below) ||
            (strcmp(K->src, e->src) && !strcmp(K->dir, e->dir))) {
            K->discarded = 1;
            kept[k] = i;
        } else e->discarded = 1;
    }
    *nkept = nd;
}

/* PATH made absolute with "." ".." and symbolic links resolved (malloc'd),
 * or 0 and errno -- realpath's job, done here because libtoby's realpath
 * only joins the working directory on */
static char *sy_dfcanon(const char *path) {
    size_t cap = 4096;
    char *out = (char *)malloc(cap), *todo = (char *)malloc(cap * 2), *link = (char *)malloc(cap);
    if (!out || !todo || !link) su_error(1, ENOMEM, "memory exhausted");
    int links = 0;
    if (path[0] == '/') snprintf(todo, cap * 2, "%s", path);
    else {
        if (!getcwd(out, cap)) { free(out); free(todo); free(link); return 0; }
        snprintf(todo, cap * 2, "%s/%s", out, path);
    }
    strcpy(out, "/");
    char *p = todo;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - p);
        if (n == 1 && p[0] == '.') { p = e; continue; }
        if (n == 2 && p[0] == '.' && p[1] == '.') {
            char *s = strrchr(out, '/');
            if (s == out) out[1] = 0;
            else *s = 0;
            p = e;
            continue;
        }
        size_t ol = strlen(out);
        if (ol + n + 2 > cap) { errno = ENAMETOOLONG; free(out); free(todo); free(link); return 0; }
        if (ol > 1) out[ol++] = '/';
        memcpy(out + ol, p, n);
        out[ol + n] = 0;
        struct stat st;
        if (lstat(out, &st) != 0) { int er = errno; free(out); free(todo); free(link); errno = er; return 0; }
        if (S_ISLNK(st.st_mode)) {
            if (++links > 40) { errno = ELOOP; free(out); free(todo); free(link); return 0; }
            ssize_t l = readlink(out, link, cap - 1);
            if (l < 0) { int er = errno; free(out); free(todo); free(link); errno = er; return 0; }
            link[l] = 0;
            /* the target, then what was left */
            char *rest = (char *)malloc(cap * 2);
            if (!rest) su_error(1, ENOMEM, "memory exhausted");
            snprintf(rest, cap * 2, "%s%s", link, e);
            if (link[0] == '/') strcpy(out, "/");
            else {
                char *s = strrchr(out, '/');
                if (s == out) out[1] = 0;
                else *s = 0;
            }
            strcpy(todo, rest);
            free(rest);
            p = todo;
            continue;
        }
        p = e;
    }
    free(todo);
    free(link);
    return out;
}

/* the mount point of canonical PATH (on device D, a directory or not):
 * up while the device stays the same */
static char *sy_dfclimb(const char *path, int isdir, dev_t d) {
    char *p = sy_dfdup(path);
    if (!isdir) {
        char *s = strrchr(p, '/');
        if (s == p) p[1] = 0;
        else if (s) *s = 0;
    }
    while (strcmp(p, "/")) {
        char *s = strrchr(p, '/');
        if (!s) break;
        char *parent = sy_dfdup(p);
        if (s == p) parent[1] = 0;
        else parent[s - p] = 0;
        struct stat st;
        if (stat(parent, &st) != 0 || st.st_dev != d) { free(parent); break; }
        free(p);
        p = parent;
    }
    return p;
}

static void sy_dfusage(void) {
    printf("Usage: %s [OPTION]... [FILE]...\n"
           "Report the space used and free on the file system holding each FILE, or\n"
           "on every mounted file system when there is no FILE.\n"
           "\n"
           "An argument a long option needs, its short form needs too.\n"
           "  -a, --all             list every entry: pseudo, duplicate, unreachable\n"
           "  -B, --block-size=SIZE  count in units of SIZE (-BM: units of 1,048,576\n"
           "                           bytes; SIZE is described below)\n"
           "  -h, --human-readable  sizes in powers of 1024, at most three digits (1023M)\n"
           "  -H, --si              the same in powers of 1000 (1.1G)\n"
           "  -i, --inodes          inodes, not blocks\n"
           "  -k                    the same as --block-size=1K\n"
           "  -l, --local           only file systems on this machine\n"
           "      --no-sync         do not sync first (the default)\n"
           "      --output[=FIELD_LIST]  the columns FIELD_LIST names, or all of them\n"
           "  -P, --portability     the POSIX format\n"
           "      --sync            sync before reading the numbers\n"
           "      --total           drop what cannot add to the space available, and\n"
           "                          add a line of totals\n"
           "  -t, --type=TYPE       only file systems of type TYPE\n"
           "  -T, --print-type      a column with each file system's type\n"
           "  -x, --exclude-type=TYPE   no file systems of type TYPE\n"
           "  -v                    accepted and ignored\n"
           "      --help     show this help and exit\n"
           "      --version  show the version and exit\n"
           "\n"
           "The unit is the first of --block-size, DF_BLOCK_SIZE, BLOCK_SIZE and\n"
           "BLOCKSIZE that is set; with none, 1024 bytes (512 with POSIXLY_CORRECT).\n"
           "\n"
           "SIZE is a whole number with an optional unit: K M G T P E Z Y for powers\n"
           "of 1024 (KiB, MiB... mean the same), KB MB... for powers of 1000.\n"
           "\n"
           "FIELD_LIST is a comma-separated list of: source fstype itotal iused\n"
           "iavail ipcent size used avail pcent file target.\n",
           su_name);
}

static int do_df(int argc, char **argv) {
    enum { SYNC_OPT = 0x200, NO_SYNC_OPT, OUTPUT_OPT, TOTAL_OPT };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "block-size", SU_REQ_ARG, 0, 'B' },
        { "inodes", SU_NO_ARG, 0, 'i' },
        { "human-readable", SU_NO_ARG, 0, 'h' },
        { "si", SU_NO_ARG, 0, 'H' },
        { "local", SU_NO_ARG, 0, 'l' },
        { "output", SU_OPT_ARG, 0, OUTPUT_OPT },
        { "portability", SU_NO_ARG, 0, 'P' },
        { "print-type", SU_NO_ARG, 0, 'T' },
        { "sync", SU_NO_ARG, 0, SYNC_OPT },
        { "no-sync", SU_NO_ARG, 0, NO_SYNC_OPT },
        { "total", SU_NO_ARG, 0, TOTAL_OPT },
        { "type", SU_REQ_ARG, 0, 't' },
        { "exclude-type", SU_REQ_ARG, 0, 'x' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int inodes = 0, ptype = 0, output = 0, dosync = 0, have_bs = 0, c, li = -1;
    int infield[SY_NFIELD] = { 0 };
    struct sy_dfbs bs;
    while ((li = -1, c = su_getopt_long(argc, argv, "aB:iF:hHklmPTt:vx:", longs, &li)) != -1) {
        switch (c) {
        case 'a': sy_dfall = 1; break;
        case 'B':
            /* named as it was given: "-B" or "--block-size" */
            sy_dfbsopt(su_optarg, li >= 0 ? "--block-size" : "-B", &bs);
            sy_dfb = bs;
            have_bs = 1;
            break;
        case 'i':
        case 'T':
        case 'P':
            if (output) {
                fflush(stdout);
                fprintf(stderr, "%s: options -%c and --output are mutually exclusive\n", su_short, c);
                su_try_help(1);
            }
            if (c == 'i') inodes = 1;
            else if (c == 'T') ptype = 1;
            else sy_dfposix = 1;
            break;
        case 'h': memset(&sy_dfb, 0, sizeof sy_dfb); sy_dfb.human = 1024; have_bs = 1; break;
        case 'H': memset(&sy_dfb, 0, sizeof sy_dfb); sy_dfb.human = 1000; have_bs = 1; break;
        case 'k': memset(&sy_dfb, 0, sizeof sy_dfb); sy_dfb.unit = 1024; have_bs = 1; break;
        case 'm': memset(&sy_dfb, 0, sizeof sy_dfb); sy_dfb.unit = 1048576; have_bs = 1; break;
        case 'l': sy_dflocal = 1; break;
        case 't': case 'F': sy_dfpush(&sy_dfsel, &sy_dfnsel, su_optarg); break;
        case 'x': sy_dfpush(&sy_dfexc, &sy_dfnexc, su_optarg); break;
        case 'v': break;
        case SYNC_OPT: dosync = 1; break;
        case NO_SYNC_OPT: dosync = 0; break;
        case TOTAL_OPT: sy_dftotal = 1; break;
        case OUTPUT_OPT: {
            const char *other = inodes ? "-i" : sy_dfposix ? "-P" : ptype ? "-T" : 0;
            if (other) {
                fflush(stdout);
                fprintf(stderr, "%s: options %s and --output are mutually exclusive\n", su_short, other);
                su_try_help(1);
            }
            output = 1;
            /* with no list it means every field -- unless a list is given
             * too, before or after */
            if (!su_optarg) break;
            char *list = sy_dfdup(su_optarg), *s = list;
            for (;;) {
                char *comma = strchr(s, ',');
                if (comma) *comma = 0;
                int f = 0;
                while (f < SY_NFIELD && strcmp(s, sy_dffname[f])) f++;
                fflush(stdout);
                if (f == SY_NFIELD) {
                    fprintf(stderr, "%s: option --output: field %s unknown\n", su_short, su_quote(s));
                    su_try_help(1);
                }
                if (infield[f]) {
                    fprintf(stderr, "%s: option --output: field %s used more than once\n",
                            su_short, su_quote(s));
                    su_try_help(1);
                }
                infield[f] = 1;
                sy_dffield[sy_dfnfield++] = f;
                if (!comma) break;
                s = comma + 1;
            }
            free(list);
            break;
        }
        case SU_HELP_OPT: sy_dfusage(); return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    /* every clash is named, the last -t first, then status 1 */
    int clash = 0;
    for (int i = sy_dfnsel - 1; i >= 0; i--)
        for (int j = 0; j < sy_dfnexc; j++)
            if (!strcmp(sy_dfsel[i], sy_dfexc[j])) {
                su_error(0, 0, "file system type %s both selected and excluded",
                         su_quote(sy_dfsel[i]));
                clash = 1;
                break;              /* once for each -t */
            }
    if (clash) return 1;
    if (output && sy_dfnfield == 0)        /* --output alone: every field */
        for (int f = 0; f < SY_NFIELD; f++) sy_dffield[sy_dfnfield++] = f;
    if (!have_bs) {
        int posix_env = getenv("POSIXLY_CORRECT") != 0;
        memset(&sy_dfb, 0, sizeof sy_dfb);
        sy_dfb.unit = posix_env ? 512 : 1024;
        if (!sy_dfposix) {
            static const char *const envs[] = { "DF_BLOCK_SIZE", "BLOCK_SIZE", "BLOCKSIZE", 0 };
            for (int i = 0; envs[i]; i++) {
                const char *v = getenv(envs[i]);
                if (!v) continue;
                if (sy_dfbsparse(v, &bs) == 0) sy_dfb = bs;
                break;
            }
        }
    }
    if (!output) {
        sy_dfnfield = 0;
        sy_dffield[sy_dfnfield++] = SY_SRC;
        if (ptype) sy_dffield[sy_dfnfield++] = SY_FSTYPE;
        if (inodes) {
            sy_dffield[sy_dfnfield++] = SY_ITOTAL;
            sy_dffield[sy_dfnfield++] = SY_IUSED;
            sy_dffield[sy_dfnfield++] = SY_IAVAIL;
            sy_dffield[sy_dfnfield++] = SY_IPCENT;
        } else {
            sy_dffield[sy_dfnfield++] = SY_SIZE;
            sy_dffield[sy_dfnfield++] = SY_USED;
            sy_dffield[sy_dfnfield++] = SY_AVAIL;
            sy_dffield[sy_dfnfield++] = SY_PCENT;
        }
        sy_dffield[sy_dfnfield++] = SY_TARGET;
    }
    if (dosync) {
#ifdef SU_HOST
        sync();
#else
        sc0(ABI_SYS_SYNC);
#endif
    }

    int nfiles = argc - su_optind, status = 0, failed = 0;
    int tr = sy_dfread();
    if (tr != 0) {
        int e = errno;
        if (nfiles == 0 || sy_dfall)
            su_error(1, e, "cannot read table of mounted file systems");
        su_error(0, e, "Warning: cannot read table of mounted file systems");
        sy_dfn = 0;
    }
    sy_dfheader(sy_dfnewrow(), output);

    if (nfiles == 0) {
        int *kept = (int *)malloc(((size_t)sy_dfn + 1) * sizeof *kept), nkept;
        if (!kept) su_error(1, ENOMEM, "memory exhausted");
        sy_dfdedup(kept, &nkept);
        for (int i = 0; i < sy_dfn; i++) {
            struct sy_dfent *e = &sy_dft[i];
            if (!sy_dfshown(e)) continue;
            if (!e->ok || e->discarded) {
                if (!sy_dfall) continue;
                int same = 0;
                if (e->ok) {
                    int k = sy_dfdedup_find(kept, nkept, e->dev);
                    same = k >= 0 && !strcmp(sy_dft[kept[k]].src, e->src);
                }
                if (!same) { sy_dfputrow(e->src, e->type, e->dir, "-", 0); continue; }
            }
            if (!sy_dfall && e->dummy) continue;
            struct sy_dffs fs;
            if (sy_dfstatfs(e->dir, &fs) != 0) {
                su_error(0, errno, "%s", su_quotef(e->dir));
                status = 1;
                continue;
            }
            if (!sy_dfall && fs.blocks == 0) continue;
            sy_dfputrow(e->src, e->type, e->dir, "-", &fs);
        }
        free(kept);
    } else {
        for (int i = 0; i < sy_dfn; i++) {
            struct stat st;
            if (stat(sy_dft[i].dir, &st) == 0) { sy_dft[i].ok = 1; sy_dft[i].dev = st.st_dev; }
        }
        for (int a = su_optind; a < argc; a++) {
            const char *file = argv[a];
            struct stat st;
            if (stat(file, &st) != 0) {
                su_error(0, errno, "%s", su_quotef(file));
                failed = 1;
                continue;
            }
            int pick = -1;
            char *canon = sy_dfcanon(file);
            if (S_ISBLK(st.st_mode)) {
                for (int i = 0; i < sy_dfn; i++) {
                    if (!strcmp(sy_dft[i].src, file)) { pick = i; continue; }
                    char *rs = sy_dfcanon(sy_dft[i].src);
                    if (rs && canon && !strcmp(rs, canon)) pick = i;
                    free(rs);
                }
            }
            if (pick < 0) {
                size_t best = 0;
                int last = -1;
                for (int i = 0; i < sy_dfn; i++) {
                    if (!sy_dft[i].ok || sy_dft[i].dev != st.st_dev) continue;
                    last = i;
                    size_t l = strlen(sy_dft[i].dir);
                    /* of two that hold it equally, the later (an over-mount) */
                    if (canon && sy_dfholds(sy_dft[i].dir, canon) && (pick < 0 || l >= best)) {
                        pick = i;
                        best = l;
                    }
                }
                if (pick < 0) pick = last;
            }
            struct sy_dffs fs;
            if (pick >= 0) {
                struct sy_dfent *e = &sy_dft[pick];
                if (!sy_dfshown(e)) { free(canon); continue; }
                /* a device's numbers are those of where it is mounted */
                const char *where = S_ISBLK(st.st_mode) ? e->dir : file;
                if (sy_dfstatfs(where, &fs) != 0) {
                    su_error(0, errno, "%s", su_quotef(file));
                    failed = 1;
                } else sy_dfputrow(e->src, e->type, e->dir, file, &fs);
            } else {
                char *mp = sy_dfclimb(canon ? canon : file, S_ISDIR(st.st_mode), st.st_dev);
                if (sy_dfstatfs(file, &fs) != 0) {
                    su_error(0, errno, "%s", su_quotef(file));
                    failed = 1;
                } else sy_dfputrow("-", "-", mp, file, &fs);
                free(mp);
            }
            free(canon);
        }
    }
    if (sy_dfnrows > 1) {
        if (sy_dftotal && sy_dftot.any) {
            struct sy_dfrow *r = sy_dfnewrow();
            r->cell[SY_SRC] = sy_dfdup("total");
            r->cell[SY_FSTYPE] = sy_dfdup("-");
            r->cell[SY_FILE] = sy_dfdup("-");
            r->cell[SY_TARGET] = sy_dfdup("-");
            r->cell[SY_SIZE] = sy_dfsize(sy_dftot.size);
            r->cell[SY_USED] = sy_dfsize(sy_dftot.used);
            r->cell[SY_AVAIL] = sy_dfsize(sy_dftot.avail);
            r->cell[SY_PCENT] = sy_dfpct(sy_dftot.used, sy_dftot.avail);
            r->cell[SY_ITOTAL] = sy_dfcount(sy_dftot.itotal);
            r->cell[SY_IUSED] = sy_dfcount(sy_dftot.iused);
            r->cell[SY_IAVAIL] = sy_dfcount(sy_dftot.iavail);
            r->cell[SY_IPCENT] = sy_dfpct(sy_dftot.iused, sy_dftot.iavail);
        }
        sy_dfprint();
    } else if (!failed) {
        su_error(0, 0, "no file systems processed");
        status = 1;
    }
    if (failed) status = 1;
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return status;
}
