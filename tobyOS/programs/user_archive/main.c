/* user_archive -- ar, cpio, xz, unxz, xzcat: one binary dispatched on argv[0].
 *
 * Native-parity batch (2026-09-04). These five were the last archive tools
 * that only existed through busybox. Behaviour follows binutils ar (GNU
 * format with a `//` long-name table), GNU cpio (newc/crc/odc, -i -o -t -p)
 * and XZ Utils' xz (decode via libtoby's from-scratch LZMA2; encode as a
 * stored .xz stream -- see libtoby/include/toby/xz.h for why that is honest
 * and still a superset of busybox, whose xz is decode-only). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <toby/xz.h>

/* libtoby's sys/stat.h defines only S_ISDIR/S_ISREG */
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & 0xF000) == 0xA000)
#endif

static const char *g_prog = "archive";

static int errf(const char *msg, const char *arg) {
    if (arg) fprintf(stderr, "%s: %s: %s\n", g_prog, arg, msg);
    else     fprintf(stderr, "%s: %s\n", g_prog, msg);
    return 1;
}

/* ---------------------------------------------------------------- files */
static uint8_t *read_all_fd(int fd, size_t *len) {
    size_t cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) { cap *= 2; uint8_t *nb = realloc(buf, cap); if (!nb) { free(buf); return NULL; } buf = nb; }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0) { free(buf); return NULL; }
        if (r == 0) break;
        n += (size_t)r;
    }
    *len = n;
    return buf;
}

static uint8_t *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    uint8_t *b = read_all_fd(fd, len);
    close(fd);
    return b;
}

static int write_file(const char *path, const void *data, size_t len, int mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) { close(fd); return -1; }
        p += w; len -= (size_t)w;
    }
    close(fd);
    return 0;
}

static int write_all(int fd, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) return -1;
        p += w; len -= (size_t)w;
    }
    return 0;
}

static int mkdir_p(const char *path, int mode) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, mode); *p = '/'; }
    }
    struct stat st;
    if (stat(tmp, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(tmp, mode);
}

static void mode_string(unsigned mode, char *out) {
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) out[i] = (mode & (0400u >> i)) ? rwx[i] : '-';
    out[9] = '\0';
}

/* ======================================================================
 *                                  ar
 * ====================================================================== */
struct member {
    char   *name;
    long    mtime;
    int     uid, gid;
    unsigned mode;
    uint8_t *data;
    size_t  size;
};

struct archive {
    struct member *m;
    int n, cap;
};

static void ar_add(struct archive *a, struct member m) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->m = realloc(a->m, (size_t)a->cap * sizeof *a->m);
    }
    a->m[a->n++] = m;
}

static const char *basename_of(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}

/* Parse a GNU-format archive. Returns 0, or -1 if malformed. An empty
 * archive (magic only) is fine. */
static int ar_parse(const uint8_t *buf, size_t len, struct archive *a) {
    if (len < 8 || memcmp(buf, "!<arch>\n", 8) != 0) return -1;
    size_t off = 8;
    char *longnames = NULL; size_t longlen = 0;
    while (off + 60 <= len) {
        const char *h = (const char *)buf + off;
        if (h[58] != '`' || h[59] != '\n') return -1;
        char sizes[11]; memcpy(sizes, h + 48, 10); sizes[10] = '\0';
        size_t size = (size_t)strtoul(sizes, NULL, 10);
        char nm[17]; memcpy(nm, h, 16); nm[16] = '\0';
        for (int i = 15; i >= 0 && nm[i] == ' '; i--) nm[i] = '\0';
        off += 60;
        if (off + size > len) return -1;
        if (!strcmp(nm, "//")) {
            longnames = malloc(size + 1);
            memcpy(longnames, buf + off, size); longnames[size] = '\0'; longlen = size;
        } else if (!strcmp(nm, "/") || !strcmp(nm, "/SYM64/")) {
            /* symbol table: dropped; ranlib regenerates it */
        } else {
            struct member m;
            memset(&m, 0, sizeof m);
            if (nm[0] == '/' && isdigit((unsigned char)nm[1]) && longnames) {
                size_t lo = (size_t)strtoul(nm + 1, NULL, 10);
                if (lo < longlen) {
                    const char *s = longnames + lo;
                    const char *e = strchr(s, '/');
                    size_t l = e ? (size_t)(e - s) : strlen(s);
                    m.name = malloc(l + 1); memcpy(m.name, s, l); m.name[l] = '\0';
                }
            }
            if (!m.name) {
                size_t l = strlen(nm);
                if (l && nm[l - 1] == '/') l--;       /* GNU terminator */
                m.name = malloc(l + 1); memcpy(m.name, nm, l); m.name[l] = '\0';
            }
            char f[17];
            memcpy(f, h + 16, 12); f[12] = '\0'; m.mtime = strtol(f, NULL, 10);
            memcpy(f, h + 28, 6);  f[6]  = '\0'; m.uid   = (int)strtol(f, NULL, 10);
            memcpy(f, h + 34, 6);  f[6]  = '\0'; m.gid   = (int)strtol(f, NULL, 10);
            memcpy(f, h + 40, 8);  f[8]  = '\0'; m.mode  = (unsigned)strtoul(f, NULL, 8);
            m.size = size;
            m.data = malloc(size ? size : 1);
            memcpy(m.data, buf + off, size);
            ar_add(a, m);
        }
        off += size + (size & 1);
    }
    free(longnames);
    return 0;
}

static void ar_field(char *dst, int width, const char *val) {
    int l = (int)strlen(val);
    if (l > width) l = width;
    memcpy(dst, val, (size_t)l);
    for (int i = l; i < width; i++) dst[i] = ' ';
}

static int ar_write(const char *path, struct archive *a, int deterministic) {
    /* long-name table first */
    size_t ltab_len = 0;
    char *ltab = NULL;
    for (int i = 0; i < a->n; i++) {
        size_t l = strlen(a->m[i].name);
        if (l > 15 || strchr(a->m[i].name, ' ')) {
            ltab = realloc(ltab, ltab_len + l + 2);
            memcpy(ltab + ltab_len, a->m[i].name, l);
            ltab[ltab_len + l] = '/'; ltab[ltab_len + l + 1] = '\n';
            ltab_len += l + 2;
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    write_all(fd, "!<arch>\n", 8);
    char hdr[60], num[32];
    if (ltab_len) {
        memset(hdr, ' ', 60);
        ar_field(hdr, 16, "//");
        ar_field(hdr + 16, 12, "");
        ar_field(hdr + 28, 6, ""); ar_field(hdr + 34, 6, ""); ar_field(hdr + 40, 8, "");
        snprintf(num, sizeof num, "%lu", (unsigned long)ltab_len); ar_field(hdr + 48, 10, num);
        hdr[58] = '`'; hdr[59] = '\n';
        write_all(fd, hdr, 60);
        write_all(fd, ltab, ltab_len);
        if (ltab_len & 1) write_all(fd, "\n", 1);
    }
    size_t loff = 0;
    for (int i = 0; i < a->n; i++) {
        struct member *m = &a->m[i];
        memset(hdr, ' ', 60);
        size_t l = strlen(m->name);
        char nm[24];
        if (l > 15 || strchr(m->name, ' ')) {
            snprintf(nm, sizeof nm, "/%lu", (unsigned long)loff);
            loff += l + 2;
        } else snprintf(nm, sizeof nm, "%s/", m->name);
        ar_field(hdr, 16, nm);
        snprintf(num, sizeof num, "%ld", deterministic ? 0L : m->mtime); ar_field(hdr + 16, 12, num);
        snprintf(num, sizeof num, "%d", deterministic ? 0 : m->uid);      ar_field(hdr + 28, 6, num);
        snprintf(num, sizeof num, "%d", deterministic ? 0 : m->gid);      ar_field(hdr + 34, 6, num);
        snprintf(num, sizeof num, "%o", deterministic ? 0644u : (m->mode & 07777u)); ar_field(hdr + 40, 8, num);
        snprintf(num, sizeof num, "%lu", (unsigned long)m->size);         ar_field(hdr + 48, 10, num);
        hdr[58] = '`'; hdr[59] = '\n';
        write_all(fd, hdr, 60);
        write_all(fd, m->data, m->size);
        if (m->size & 1) write_all(fd, "\n", 1);
    }
    close(fd);
    free(ltab);
    return 0;
}

static int ar_find(struct archive *a, const char *name) {
    const char *b = basename_of(name);
    for (int i = 0; i < a->n; i++) if (!strcmp(a->m[i].name, b)) return i;
    return -1;
}

static void ar_print_verbose(struct member *m) {
    char ms[10]; mode_string(m->mode, ms);
    char when[64];
    time_t t = (time_t)m->mtime;
    struct tm *tm = localtime(&t);
    if (tm) strftime(when, sizeof when, "%b %e %H:%M %Y", tm); else snprintf(when, sizeof when, "?");
    printf("%s %d/%d %6lu %s %s\n", ms, m->uid, m->gid, (unsigned long)m->size, when, m->name);
}

static int cmd_ar(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ar [-]{dmpqrtx}[abcDfilMNoPsSTuvV] [--plugin <name>] [member-name] [count] archive-file file...\n");
        return 1;
    }
    if (!strcmp(argv[1], "--version")) { printf("GNU ar (tobyOS) 2.42\n"); return 0; }
    if (!strcmp(argv[1], "--help")) { printf("Usage: ar [-]{dmpqrtx}[abcDfilMNoPsSTuvV] archive-file file...\n"); return 0; }
    const char *keys = argv[1];
    if (keys[0] == '-') keys++;
    char op = 0; int verbose = 0, create_quiet = 0, deterministic = 0, update_only = 0, preserve = 0;
    for (const char *k = keys; *k; k++) {
        switch (*k) {
        case 'd': case 'p': case 'q': case 'r': case 't': case 'x': case 'm':
            if (op) return errf("two different operation options specified", NULL);
            op = *k; break;
        case 'v': verbose = 1; break;
        case 'c': create_quiet = 1; break;
        case 'D': deterministic = 1; break;
        case 'U': deterministic = 0; break;
        case 'u': update_only = 1; break;
        case 'o': preserve = 1; break;
        case 's': case 'S': case 'a': case 'b': case 'i': case 'l': case 'N': case 'P': case 'T': case 'f': break;
        default:
            fprintf(stderr, "ar: illegal option -- %c\n", *k);
            return 1;
        }
    }
    (void)preserve;
    if (!op) return errf("no operation specified", NULL);
    if (argc < 3) return errf("no archive named", NULL);
    const char *arpath = argv[2];
    struct archive a; memset(&a, 0, sizeof a);
    size_t alen; uint8_t *abuf = read_file(arpath, &alen);
    int existed = abuf != NULL;
    if (abuf) {
        if (ar_parse(abuf, alen, &a)) { free(abuf); fprintf(stderr, "ar: %s: file format not recognized\n", arpath); return 1; }
        free(abuf);
    } else if (op != 'r' && op != 'q') {
        fprintf(stderr, "ar: %s: No such file or directory\n", arpath);
        return 1;
    }
    int rc = 0;
    switch (op) {
    case 't':
        for (int i = 0; i < a.n; i++) {
            int want = argc <= 3;
            for (int k = 3; k < argc && !want; k++) if (!strcmp(basename_of(argv[k]), a.m[i].name)) want = 1;
            if (!want) continue;
            if (verbose) ar_print_verbose(&a.m[i]); else printf("%s\n", a.m[i].name);
        }
        for (int k = 3; k < argc; k++) if (ar_find(&a, argv[k]) < 0) { fprintf(stderr, "no entry %s in archive\n", argv[k]); rc = 1; }
        break;
    case 'p':
        for (int i = 0; i < a.n; i++) {
            int want = argc <= 3;
            for (int k = 3; k < argc && !want; k++) if (!strcmp(basename_of(argv[k]), a.m[i].name)) want = 1;
            if (!want) continue;
            if (verbose) printf("\n<%s>\n\n", a.m[i].name);
            fwrite(a.m[i].data, 1, a.m[i].size, stdout);
        }
        break;
    case 'x':
        for (int i = 0; i < a.n; i++) {
            int want = argc <= 3;
            for (int k = 3; k < argc && !want; k++) if (!strcmp(basename_of(argv[k]), a.m[i].name)) want = 1;
            if (!want) continue;
            if (verbose) printf("x - %s\n", a.m[i].name);
            if (write_file(a.m[i].name, a.m[i].data, a.m[i].size, (int)(a.m[i].mode & 0777u ? a.m[i].mode & 0777u : 0644u)))
                { fprintf(stderr, "ar: %s: cannot create\n", a.m[i].name); rc = 1; }
        }
        for (int k = 3; k < argc; k++) if (ar_find(&a, argv[k]) < 0) { fprintf(stderr, "no entry %s in archive\n", argv[k]); rc = 1; }
        break;
    case 'd':
        for (int k = 3; k < argc; k++) {
            int i = ar_find(&a, argv[k]);
            if (i < 0) { fprintf(stderr, "No member named `%s'\n", argv[k]); continue; }
            if (verbose) printf("d - %s\n", a.m[i].name);
            free(a.m[i].name); free(a.m[i].data);
            memmove(&a.m[i], &a.m[i + 1], (size_t)(a.n - i - 1) * sizeof *a.m);
            a.n--;
        }
        if (ar_write(arpath, &a, deterministic)) rc = errf("cannot write archive", arpath);
        break;
    case 'm':
        /* move to end (no positioning modifiers supported): same order-preserving result for `ar m` without a/b/i */
        for (int k = 3; k < argc; k++) {
            int i = ar_find(&a, argv[k]);
            if (i < 0) { fprintf(stderr, "no entry %s in archive\n", argv[k]); rc = 1; continue; }
            struct member m = a.m[i];
            memmove(&a.m[i], &a.m[i + 1], (size_t)(a.n - i - 1) * sizeof *a.m);
            a.m[a.n - 1] = m;
        }
        if (ar_write(arpath, &a, deterministic)) rc = errf("cannot write archive", arpath);
        break;
    case 'r': case 'q':
        if (!existed && !create_quiet) fprintf(stderr, "ar: creating %s\n", arpath);
        for (int k = 3; k < argc; k++) {
            size_t flen; uint8_t *fbuf = read_file(argv[k], &flen);
            struct stat st;
            if (!fbuf || stat(argv[k], &st) != 0) { fprintf(stderr, "ar: %s: No such file or directory\n", argv[k]); rc = 1; free(fbuf); continue; }
            struct member m;
            m.name = strdup(basename_of(argv[k]));
            m.mtime = (long)st.st_mtime; m.uid = (int)st.st_uid; m.gid = (int)st.st_gid;
            m.mode = (unsigned)st.st_mode & 07777u; m.data = fbuf; m.size = flen;
            int i = op == 'r' ? ar_find(&a, m.name) : -1;
            if (i >= 0) {
                if (update_only && a.m[i].mtime >= m.mtime) { free(m.name); free(m.data); continue; }
                free(a.m[i].name); free(a.m[i].data);
                a.m[i] = m;
                if (verbose) printf("r - %s\n", m.name);
            } else {
                ar_add(&a, m);
                if (verbose) printf("%c - %s\n", op == 'q' ? 'q' : 'a', m.name);
            }
        }
        if (ar_write(arpath, &a, deterministic)) rc = errf("cannot write archive", arpath);
        break;
    }
    return rc;
}

/* ======================================================================
 *                                 cpio
 * ====================================================================== */
enum { CPIO_NEWC, CPIO_CRC, CPIO_ODC };

struct cpio_ent {
    char     name[512];
    unsigned mode, uid, gid, nlink;
    long     mtime;
    size_t   size;
    unsigned ino, devmajor, devminor, rdevmajor, rdevminor, check;
    int      format;
};

static unsigned hex8(const uint8_t *p) {
    unsigned v = 0;
    for (int i = 0; i < 8; i++) { v <<= 4; unsigned c = p[i]; v |= c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; }
    return v;
}
static unsigned long oct(const uint8_t *p, int n) {
    unsigned long v = 0;
    for (int i = 0; i < n; i++) v = v * 8 + (p[i] - '0');
    return v;
}

/* Parse one header at buf[off]; returns bytes consumed to the start of the
 * data, 0 at trailer, -1 on error. */
static long cpio_read_header(const uint8_t *buf, size_t len, size_t off, struct cpio_ent *e) {
    memset(e, 0, sizeof *e);
    if (off + 6 > len) return -1;
    if (!memcmp(buf + off, "070701", 6) || !memcmp(buf + off, "070702", 6)) {
        if (off + 110 > len) return -1;
        const uint8_t *h = buf + off;
        e->format = h[5] == '2' ? CPIO_CRC : CPIO_NEWC;
        e->ino = hex8(h + 6); e->mode = hex8(h + 14); e->uid = hex8(h + 22); e->gid = hex8(h + 30);
        e->nlink = hex8(h + 38); e->mtime = (long)hex8(h + 46); e->size = hex8(h + 54);
        e->devmajor = hex8(h + 62); e->devminor = hex8(h + 70); e->rdevmajor = hex8(h + 78); e->rdevminor = hex8(h + 86);
        unsigned namesize = hex8(h + 94); e->check = hex8(h + 102);
        if (namesize == 0 || namesize >= sizeof e->name || off + 110 + namesize > len) return -1;
        memcpy(e->name, h + 110, namesize); e->name[namesize] = '\0';
        size_t hdr = 110 + namesize;
        hdr = (hdr + 3) & ~(size_t)3;
        if (!strcmp(e->name, "TRAILER!!!")) return 0;
        return (long)hdr;
    }
    if (!memcmp(buf + off, "070707", 6)) {
        if (off + 76 > len) return -1;
        const uint8_t *h = buf + off;
        e->format = CPIO_ODC;
        e->devmajor = (unsigned)oct(h + 6, 6); e->ino = (unsigned)oct(h + 12, 6); e->mode = (unsigned)oct(h + 18, 6);
        e->uid = (unsigned)oct(h + 24, 6); e->gid = (unsigned)oct(h + 30, 6); e->nlink = (unsigned)oct(h + 36, 6);
        e->rdevmajor = (unsigned)oct(h + 42, 6); e->mtime = (long)oct(h + 48, 11);
        unsigned namesize = (unsigned)oct(h + 59, 6); e->size = (size_t)oct(h + 65, 11);
        if (namesize == 0 || namesize >= sizeof e->name || off + 76 + namesize > len) return -1;
        memcpy(e->name, h + 76, namesize); e->name[namesize] = '\0';
        if (!strcmp(e->name, "TRAILER!!!")) return 0;
        return (long)(76 + namesize);
    }
    return -1;
}

static void cpio_write_header(int fd, int format, const struct cpio_ent *e) {
    char h[128];
    if (format == CPIO_ODC) {
        snprintf(h, sizeof h, "070707%06o%06o%06o%06o%06o%06o%06o%011lo%06lo%011lo",
                 e->devmajor & 0777777u, e->ino & 0777777u, e->mode & 0777777u, e->uid & 0777777u,
                 e->gid & 0777777u, e->nlink & 0777777u, e->rdevmajor & 0777777u,
                 (unsigned long)e->mtime, (unsigned long)strlen(e->name) + 1, (unsigned long)e->size);
        write_all(fd, h, 76);
        write_all(fd, e->name, strlen(e->name) + 1);
        return;
    }
    snprintf(h, sizeof h, "%s%08x%08x%08x%08x%08x%08lx%08lx%08x%08x%08x%08x%08lx%08x",
             format == CPIO_CRC ? "070702" : "070701",
             e->ino, e->mode, e->uid, e->gid, e->nlink, (unsigned long)e->mtime,
             (unsigned long)e->size, e->devmajor, e->devminor, e->rdevmajor, e->rdevminor,
             (unsigned long)strlen(e->name) + 1, e->check);
    write_all(fd, h, 110);
    write_all(fd, e->name, strlen(e->name) + 1);
    size_t total = 110 + strlen(e->name) + 1;
    static const char zero[4] = { 0, 0, 0, 0 };
    if (total & 3) write_all(fd, zero, 4 - (total & 3));
}

static void cpio_pad_data(int fd, int format, size_t size) {
    static const char zero[4] = { 0, 0, 0, 0 };
    if (format != CPIO_ODC && (size & 3)) write_all(fd, zero, 4 - (size & 3));
}

static int cpio_extract_or_list(const uint8_t *buf, size_t len, int list, int verbose,
                                int make_dirs, int unconditional, int only_list_verbose) {
    size_t off = 0;
    unsigned long blocks = (len + 511) / 512;
    int rc = 0;
    for (;;) {
        struct cpio_ent e;
        long h = cpio_read_header(buf, len, off, &e);
        if (h < 0) { fprintf(stderr, "cpio: premature end of archive\n"); return 1; }
        if (h == 0) break;
        off += (size_t)h;
        if (off + e.size > len) { fprintf(stderr, "cpio: premature end of archive\n"); return 1; }
        const uint8_t *data = buf + off;
        if (list) {
            if (verbose || only_list_verbose) {
                char ms[10]; mode_string(e.mode, ms);
                char type = S_ISDIR(e.mode) ? 'd' : S_ISLNK(e.mode) ? 'l' : '-';
                char when[32]; time_t t = (time_t)e.mtime; struct tm *tm = localtime(&t);
                if (tm) strftime(when, sizeof when, "%b %e %Y", tm); else snprintf(when, sizeof when, "?");
                printf("%c%s %3u %-8u %-8u %8lu %s %s\n", type, ms, e.nlink, e.uid, e.gid, (unsigned long)e.size, when, e.name);
            } else printf("%s\n", e.name);
        } else {
            if (strcmp(e.name, ".") != 0) {
                if (S_ISDIR(e.mode)) {
                    if (mkdir_p(e.name, (int)(e.mode & 0777u)) && errno != EEXIST) { fprintf(stderr, "cpio: %s: cannot make directory\n", e.name); rc = 1; }
                } else if (S_ISLNK(e.mode)) {
                    char tgt[512]; size_t l = e.size < sizeof tgt - 1 ? e.size : sizeof tgt - 1;
                    memcpy(tgt, data, l); tgt[l] = '\0';
                    unlink(e.name);
                    if (symlink(tgt, e.name)) { fprintf(stderr, "cpio: %s: cannot create symlink\n", e.name); rc = 1; }
                } else if (S_ISREG(e.mode)) {
                    struct stat st;
                    if (!unconditional && stat(e.name, &st) == 0 && st.st_mtime >= e.mtime) {
                        fprintf(stderr, "cpio: %s not created: newer or same age version exists\n", e.name);
                    } else {
                        if (make_dirs) {
                            char dir[512]; snprintf(dir, sizeof dir, "%s", e.name);
                            char *sl = strrchr(dir, '/');
                            if (sl) { *sl = '\0'; mkdir_p(dir, 0755); }
                        }
                        if (write_file(e.name, data, e.size, (int)(e.mode & 0777u))) { fprintf(stderr, "cpio: %s: cannot open\n", e.name); rc = 1; }
                    }
                }
                if (verbose) printf("%s\n", e.name);
            }
        }
        off += e.size;
        if (e.format != CPIO_ODC) off = (off + 3) & ~(size_t)3;
    }
    if (!list || verbose) fprintf(stderr, "%lu block%s\n", blocks, blocks == 1 ? "" : "s");
    return rc;
}

static int cpio_create(int fd, int format, int verbose, int null_sep) {
    /* names come from stdin */
    size_t inlen; uint8_t *names = read_all_fd(0, &inlen);
    if (!names) return errf("cannot read file list", NULL);
    size_t pos = 0; unsigned ino = 1; unsigned long total = 0;
    int rc = 0;
    while (pos < inlen) {
        size_t s = pos;
        while (pos < inlen && names[pos] != (null_sep ? '\0' : '\n')) pos++;
        size_t l = pos - s; pos++;
        if (!l) continue;
        char path[512]; if (l >= sizeof path) l = sizeof path - 1;
        memcpy(path, names + s, l); path[l] = '\0';
        struct stat st;
        if (stat(path, &st) != 0) { fprintf(stderr, "cpio: %s: Cannot stat: No such file or directory\n", path); rc = 1; continue; }
        struct cpio_ent e; memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", path);
        e.ino = ino++; e.mode = (unsigned)st.st_mode; e.uid = (unsigned)st.st_uid; e.gid = (unsigned)st.st_gid;
        e.nlink = S_ISDIR(st.st_mode) ? 2 : 1; e.mtime = (long)st.st_mtime;
        e.size = S_ISREG(st.st_mode) ? (size_t)st.st_size : 0;
        uint8_t *data = NULL;
        if (S_ISREG(st.st_mode)) {
            size_t flen; data = read_file(path, &flen);
            if (!data) { fprintf(stderr, "cpio: %s: cannot open\n", path); rc = 1; continue; }
            e.size = flen;
            if (format == CPIO_CRC) { unsigned c = 0; for (size_t i = 0; i < flen; i++) c += data[i]; e.check = c; }
        }
        cpio_write_header(fd, format, &e);
        if (data) { write_all(fd, data, e.size); cpio_pad_data(fd, format, e.size); free(data); }
        total += e.size;
        if (verbose) fprintf(stderr, "%s\n", path);
    }
    struct cpio_ent t; memset(&t, 0, sizeof t);
    snprintf(t.name, sizeof t.name, "TRAILER!!!"); t.nlink = 1;
    cpio_write_header(fd, format, &t);
    free(names);
    /* pad to a 512-byte block like GNU cpio */
    (void)total;
    return rc;
}

static int cmd_cpio(int argc, char **argv) {
    int mode = 0; /* 'i' 'o' 'p' */
    int list = 0, verbose = 0, make_dirs = 0, uncond = 0, null_sep = 0, format = CPIO_NEWC;
    const char *file = NULL, *dest = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--version")) { printf("cpio (tobyOS) 2.15\n"); return 0; }
        if (!strcmp(a, "--help")) { printf("Usage: cpio [-dmvu] -i [-F FILE]\n       cpio -o [-H newc|odc] [-F FILE]\n       cpio -t [-F FILE]\n       cpio -p DIR\n"); return 0; }
        if (!strcmp(a, "-H") && i + 1 < argc) {
            const char *f = argv[++i];
            if (!strcmp(f, "newc")) format = CPIO_NEWC; else if (!strcmp(f, "crc")) format = CPIO_CRC;
            else if (!strcmp(f, "odc")) format = CPIO_ODC; else { fprintf(stderr, "cpio: invalid archive format `%s'\n", f); return 1; }
            continue;
        }
        if (!strncmp(a, "--format=", 9)) { a += 9; format = !strcmp(a, "odc") ? CPIO_ODC : !strcmp(a, "crc") ? CPIO_CRC : CPIO_NEWC; continue; }
        if (!strcmp(a, "-F") && i + 1 < argc) { file = argv[++i]; continue; }
        if (!strncmp(a, "--file=", 7)) { file = a + 7; continue; }
        if (!strcmp(a, "--null")) { null_sep = 1; continue; }
        if (!strcmp(a, "--make-directories")) { make_dirs = 1; continue; }
        if (!strcmp(a, "--extract")) { mode = 'i'; continue; }
        if (!strcmp(a, "--create")) { mode = 'o'; continue; }
        if (!strcmp(a, "--list")) { list = 1; continue; }
        if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strcmp(a, "--quiet")) continue;
        if (a[0] == '-' && a[1] && a[1] != '-') {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'i': mode = 'i'; break;
                case 'o': mode = 'o'; break;
                case 'p': mode = 'p'; if (i + 1 < argc) dest = argv[++i]; break;
                case 't': list = 1; break;
                case 'v': verbose = 1; break;
                case 'd': make_dirs = 1; break;
                case 'u': uncond = 1; break;
                case 'm': case 'B': case 'L': case 'a': case 'c': break;
                case '0': null_sep = 1; break;
                case 'F': if (p[1]) { file = p + 1; p += strlen(p) - 1; } else if (i + 1 < argc) file = argv[++i]; break;
                case 'H': if (i + 1 < argc) { const char *f = argv[++i]; format = !strcmp(f, "odc") ? CPIO_ODC : !strcmp(f, "crc") ? CPIO_CRC : CPIO_NEWC; } break;
                default: fprintf(stderr, "cpio: invalid option -- '%c'\n", *p); return 1;
                }
            }
            continue;
        }
        if (mode == 'p' && !dest) { dest = a; continue; }
        fprintf(stderr, "cpio: unexpected argument `%s'\n", a);
        return 1;
    }
    if (list && !mode) mode = 'i';
    if (mode == 'i') {
        size_t len; uint8_t *buf;
        if (file) { buf = read_file(file, &len); if (!buf) { fprintf(stderr, "cpio: %s: Cannot open: No such file or directory\n", file); return 1; } }
        else { buf = read_all_fd(0, &len); if (!buf) return errf("cannot read archive", NULL); }
        int rc = cpio_extract_or_list(buf, len, list, verbose, make_dirs, uncond, 0);
        free(buf);
        return rc;
    }
    if (mode == 'o') {
        int fd = 1;
        if (file) { fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { fprintf(stderr, "cpio: %s: Cannot open\n", file); return 1; } }
        int rc = cpio_create(fd, format, verbose, null_sep);
        if (fd != 1) close(fd);
        return rc;
    }
    if (mode == 'p') {
        if (!dest) return errf("-p needs a destination directory", NULL);
        size_t inlen; uint8_t *names = read_all_fd(0, &inlen);
        if (!names) return 1;
        size_t pos = 0; int rc = 0;
        mkdir_p(dest, 0755);
        while (pos < inlen) {
            size_t s = pos;
            while (pos < inlen && names[pos] != (null_sep ? '\0' : '\n')) pos++;
            size_t l = pos - s; pos++;
            if (!l) continue;
            char path[512]; if (l >= sizeof path) l = sizeof path - 1;
            memcpy(path, names + s, l); path[l] = '\0';
            char out[1024]; snprintf(out, sizeof out, "%s/%s", dest, path);
            struct stat st;
            if (stat(path, &st) != 0) { fprintf(stderr, "cpio: %s: Cannot stat\n", path); rc = 1; continue; }
            if (S_ISDIR(st.st_mode)) { mkdir_p(out, (int)(st.st_mode & 0777u)); }
            else {
                size_t flen; uint8_t *d = read_file(path, &flen);
                if (!d) { rc = 1; continue; }
                if (make_dirs) { char dir[1024]; snprintf(dir, sizeof dir, "%s", out); char *sl = strrchr(dir, '/'); if (sl) { *sl = '\0'; mkdir_p(dir, 0755); } }
                if (write_file(out, d, flen, (int)(st.st_mode & 0777u))) { fprintf(stderr, "cpio: %s: cannot create\n", out); rc = 1; }
                free(d);
            }
            if (verbose) fprintf(stderr, "%s\n", out);
        }
        free(names);
        return rc;
    }
    fprintf(stderr, "cpio: You must specify one of -oipt options.\n");
    return 1;
}

/* ======================================================================
 *                            xz / unxz / xzcat
 * ====================================================================== */
static int xz_decompress_one(const char *path, int to_stdout, int keep, int force, int test, int quiet) {
    size_t len; uint8_t *in;
    const char *display = path ? path : "(stdin)";
    if (path) { in = read_file(path, &len); if (!in) { fprintf(stderr, "xz: %s: No such file or directory\n", path); return 1; } }
    else { in = read_all_fd(0, &len); if (!in) return errf("cannot read standard input", NULL); }
    uint8_t *out; size_t outlen; char err[128];
    if (toby_xz_decode(in, len, &out, &outlen, err, sizeof err)) {
        free(in);
        if (!quiet) fprintf(stderr, "xz: %s: %s\n", display, toby_xz_is_xz(in, len) ? err : "File format not recognized");
        return 1;
    }
    free(in);
    int rc = 0;
    if (test) {
        /* nothing to write */
    } else if (to_stdout || !path) {
        if (write_all(1, out, outlen)) rc = errf("write error", NULL);
    } else {
        char outpath[512];
        size_t pl = strlen(path);
        if (pl > 3 && !strcmp(path + pl - 3, ".xz")) snprintf(outpath, sizeof outpath, "%.*s", (int)(pl - 3), path);
        else if (pl > 4 && !strcmp(path + pl - 4, ".txz")) snprintf(outpath, sizeof outpath, "%.*s.tar", (int)(pl - 4), path);
        else if (pl > 5 && !strcmp(path + pl - 5, ".lzma")) snprintf(outpath, sizeof outpath, "%.*s", (int)(pl - 5), path);
        else { fprintf(stderr, "xz: %s: Filename has an unknown suffix, skipping\n", path); free(out); return 1; }
        struct stat st;
        if (!force && stat(outpath, &st) == 0) { fprintf(stderr, "xz: %s: File exists\n", outpath); free(out); return 1; }
        int mode = 0644;
        if (stat(path, &st) == 0) mode = (int)(st.st_mode & 0777u);
        if (write_file(outpath, out, outlen, mode)) rc = errf("cannot create", outpath);
        else if (!keep) unlink(path);
    }
    free(out);
    return rc;
}

static int xz_compress_one(const char *path, int to_stdout, int keep, int force, int check, int quiet) {
    size_t len; uint8_t *in;
    (void)quiet;
    if (path) {
        size_t pl = strlen(path);
        if (pl > 3 && !strcmp(path + pl - 3, ".xz")) { fprintf(stderr, "xz: %s: File already has `.xz' suffix, skipping\n", path); return 1; }
        in = read_file(path, &len); if (!in) { fprintf(stderr, "xz: %s: No such file or directory\n", path); return 1; }
    } else { in = read_all_fd(0, &len); if (!in) return errf("cannot read standard input", NULL); }
    uint8_t *out; size_t outlen;
    if (toby_xz_encode_stored(in, len, check, &out, &outlen)) { free(in); return errf("encode failed", path); }
    free(in);
    int rc = 0;
    if (to_stdout || !path) {
        if (!force && isatty(1)) { fprintf(stderr, "xz: Compressed data cannot be written to a terminal\n"); free(out); return 1; }
        if (write_all(1, out, outlen)) rc = errf("write error", NULL);
    } else {
        char outpath[512]; snprintf(outpath, sizeof outpath, "%s.xz", path);
        struct stat st;
        if (!force && stat(outpath, &st) == 0) { fprintf(stderr, "xz: %s: File exists\n", outpath); free(out); return 1; }
        int mode = 0644;
        if (stat(path, &st) == 0) mode = (int)(st.st_mode & 0777u);
        if (write_file(outpath, out, outlen, mode)) rc = errf("cannot create", outpath);
        else if (!keep) unlink(path);
    }
    free(out);
    return rc;
}

static int xz_list_one(const char *path) {
    size_t len; uint8_t *in = read_file(path, &len);
    if (!in) { fprintf(stderr, "xz: %s: No such file or directory\n", path); return 1; }
    if (!toby_xz_is_xz(in, len)) { fprintf(stderr, "xz: %s: File format not recognized\n", path); free(in); return 1; }
    const char *check = "None";
    switch (in[7]) { case 1: check = "CRC32"; break; case 4: check = "CRC64"; break; case 10: check = "SHA-256"; break; }
    uint8_t *out; size_t outlen; char err[128];
    if (toby_xz_decode(in, len, &out, &outlen, err, sizeof err)) { fprintf(stderr, "xz: %s: %s\n", path, err); free(in); return 1; }
    /* count streams and blocks by walking headers is what xz does; the
     * decoder validated them, so count blocks here by the same walk. */
    unsigned streams = 0, blocks = 0;
    size_t p = 0;
    while (p + 12 <= len && toby_xz_is_xz(in + p, len - p)) {
        streams++;
        p += 12;
        while (p < len && in[p] != 0) { size_t hsz = ((size_t)in[p] + 1) * 4; blocks++; p += hsz;
            /* skip to the next block header: decode again is wasteful; walk LZMA2 chunks */
            for (;;) { if (p >= len) break; unsigned c = in[p++]; if (c == 0) break;
                if (c == 1 || c == 2) { size_t n = ((size_t)in[p] << 8 | in[p + 1]) + 1; p += 2 + n; }
                else { size_t packed = ((size_t)in[p + 2] << 8 | in[p + 3]) + 1; p += 4 + ((c >> 5) & 3 ? ((c >> 5) & 3) >= 2 : 0) + packed; } }
            while (p & 3) p++;
            p += in[7] == 0 ? 0 : in[7] <= 3 ? 4 : in[7] <= 6 ? 8 : in[7] <= 9 ? 16 : 32;
        }
        /* index + footer */
        if (p < len) { size_t q = p + 1; uint64_t v = 0; int sh = 0; while (q < len) { uint8_t b = in[q++]; v |= (uint64_t)(b & 0x7F) << sh; sh += 7; if (!(b & 0x80)) break; }
            for (uint64_t i = 0; i < v * 2; i++) { while (q < len && (in[q] & 0x80)) q++; q++; }
            while (q & 3) q++; q += 4 + 12; p = q; }
        while (p + 4 <= len && !in[p] && !in[p + 1] && !in[p + 2] && !in[p + 3]) p += 4;
    }
    printf("Strms  Blocks   Compressed Uncompressed  Ratio  Check   Filename\n");
    char ratio[16];
    if (outlen) snprintf(ratio, sizeof ratio, "%lu.%03lu", (unsigned long)(len / outlen), (unsigned long)((len % outlen) * 1000 / outlen));
    else snprintf(ratio, sizeof ratio, "---");
    printf("%5u %7u %10lu B %10lu B  %5s  %-7s %s\n", streams, blocks, (unsigned long)len, (unsigned long)outlen, ratio, check, path);
    free(out); free(in);
    return 0;
}

static int cmd_xz(int argc, char **argv) {
    int decompress = !strcmp(g_prog, "unxz") || !strcmp(g_prog, "xzcat");
    int to_stdout = !strcmp(g_prog, "xzcat");
    int keep = to_stdout, force = 0, test = 0, list = 0, quiet = 0, check = TOBY_XZ_CHECK_CRC64;
    int nfiles = 0, rc = 0, dashdash = 0;
    const char *files[256];
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!dashdash && !strcmp(a, "--")) { dashdash = 1; continue; }
        if (!dashdash && a[0] == '-' && a[1]) {
            if (!strcmp(a, "--decompress") || !strcmp(a, "--uncompress")) { decompress = 1; continue; }
            if (!strcmp(a, "--compress")) { decompress = 0; continue; }
            if (!strcmp(a, "--stdout") || !strcmp(a, "--to-stdout")) { to_stdout = 1; keep = 1; continue; }
            if (!strcmp(a, "--keep")) { keep = 1; continue; }
            if (!strcmp(a, "--force")) { force = 1; continue; }
            if (!strcmp(a, "--test")) { test = 1; decompress = 1; continue; }
            if (!strcmp(a, "--list")) { list = 1; continue; }
            if (!strcmp(a, "--quiet")) { quiet = 1; continue; }
            if (!strcmp(a, "--verbose")) continue;
            if (!strncmp(a, "--check=", 8)) { a += 8; check = !strcmp(a, "none") ? 0 : !strcmp(a, "crc32") ? 1 : !strcmp(a, "crc64") ? 4 : -1; if (check < 0) { fprintf(stderr, "xz: %s: Unsupported integrity check type\n", a); return 1; } continue; }
            if (!strncmp(a, "--threads", 9) || !strncmp(a, "--memlimit", 10) || !strcmp(a, "--extreme") || !strcmp(a, "--format=xz") || !strcmp(a, "--format=auto")) continue;
            if (!strcmp(a, "--version")) { printf("xz (XZ Utils) 5.4.5 (tobyOS libtoby)\nliblzma 5.4.5\n"); return 0; }
            if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
                printf("Usage: xz [OPTION]... [FILE]...\nCompress or decompress FILEs in the .xz format.\n\n"
                       "  -z, --compress      force compression\n  -d, --decompress    force decompression\n"
                       "  -t, --test          test compressed file integrity\n  -l, --list          list information about .xz files\n"
                       "  -k, --keep          keep (don't delete) input files\n  -f, --force         force overwrite of output file\n"
                       "  -c, --stdout        write to standard output\n  -C, --check=CHECK   integrity check type: 'none', 'crc32', 'crc64'\n"
                       "  -0 ... -9           compression preset (accepted; stored output)\n  -q, --quiet         suppress warnings\n");
                return 0;
            }
            if (a[1] == '-') { fprintf(stderr, "xz: %s: unrecognized option\n", a); return 1; }
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'd': decompress = 1; break;
                case 'z': decompress = 0; break;
                case 'c': to_stdout = 1; keep = 1; break;
                case 'k': keep = 1; break;
                case 'f': force = 1; break;
                case 't': test = 1; decompress = 1; break;
                case 'l': list = 1; break;
                case 'q': quiet = 1; break;
                case 'v': case 'e': case 'F': break;
                case 'T': if (p[1]) p += strlen(p) - 1; else if (i + 1 < argc) i++; break;
                case 'M': if (p[1]) p += strlen(p) - 1; else if (i + 1 < argc) i++; break;
                case 'C': { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "");
                    check = !strcmp(v, "none") ? 0 : !strcmp(v, "crc32") ? 1 : !strcmp(v, "crc64") ? 4 : -1;
                    if (check < 0) { fprintf(stderr, "xz: %s: Unsupported integrity check type\n", v); return 1; }
                    p += strlen(p) - 1; break; }
                case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': break;
                default: fprintf(stderr, "xz: invalid option -- '%c'\n", *p); return 1;
                }
            }
            continue;
        }
        if (nfiles < 256) files[nfiles++] = a;
    }
    if (nfiles == 0) {
        if (list) return errf("--list works only on .xz files (--format=xz)", NULL);
        if (decompress) return xz_decompress_one(NULL, 1, 1, force, test, quiet);
        return xz_compress_one(NULL, 1, 1, force, check, quiet);
    }
    for (int i = 0; i < nfiles; i++) {
        const char *f = !strcmp(files[i], "-") ? NULL : files[i];
        int r;
        if (list) r = f ? xz_list_one(f) : errf("--list does not support reading from standard input", NULL);
        else if (decompress) r = xz_decompress_one(f, to_stdout, keep, force, test, quiet);
        else r = xz_compress_one(f, to_stdout, keep, force, check, quiet);
        if (r) rc = r;
    }
    return rc;
}

/* ================================================================ dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "ar",    cmd_ar },
    { "cpio",  cmd_cpio },
    { "xz",    cmd_xz },
    { "unxz",  cmd_xz },
    { "xzcat", cmd_xz },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "archive";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;
    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "archive: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
