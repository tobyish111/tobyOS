/* df -- free space on each filesystem, in coreutils' layout.
 *
 *     df [-a] [-h] [-k] [-T] [FILE...]
 *
 * The numbers are statfs's (ABI_SYS_STATFS): the ones busybox's df and every
 * Linux program read through statfs(2). Until M3 this called
 * ABI_SYS_FS_CHECK -- a structural CHECKER -- through a struct laid out
 * differently from the kernel's, so `df /tmp` printed "Inodes: 0 used / 0
 * total" and "WARNING: 1886221359 error(s) found" (1886221359 is the bytes
 * "/tmp" read as a number). Nothing here checks anything.
 *
 * With no FILE every mount in /proc/mounts is listed, except those with no
 * blocks at all (proc, dev...) unless -a -- as coreutils does. The
 * Filesystem column is the mount's source; /proc/mounts says "none" for all
 * of them, so the filesystem's type stands in, as "tmpfs" does on Linux.
 * Sizes are 1K blocks, or powers of 1024 with -h, rounded UP; Use% is
 * used/(used+available), rounded up, "-" when both are 0. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <tobyos/abi/abi.h>

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}

static int opt_all, opt_human, opt_type;

/* The type statfs reports, by coreutils' names (stat -f %T). */
static const char *type_name(uint32_t magic) {
    switch (magic) {
    case 0x01021994u: return "tmpfs";
    case 0x858458f6u: return "ramfs";
    case 0x0000EF53u: return "ext2/ext3";
    case 0x00004d44u: return "msdos";
    case 0x746f6279u: return "tobyfs";
    default:          return "-";
    }
}

/* ---- mounts ------------------------------------------------------------ */

#define MAX_MOUNTS 64
static char *g_mount[MAX_MOUNTS];
static int g_nmount;

static void load_mounts(void) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;
    char line[512];
    while (g_nmount < MAX_MOUNTS && fgets(line, sizeof line, f)) {
        char *p = strchr(line, ' ');          /* source, then the point */
        if (!p) continue;
        p++;
        char *e = strchr(p, ' ');
        if (!e) continue;
        *e = '\0';
        int dup = 0;
        for (int i = 0; i < g_nmount; i++)
            if (strcmp(g_mount[i], p) == 0) dup = 1;
        if (!dup) g_mount[g_nmount++] = strdup(p);
    }
    fclose(f);
}

/* "/a/./b/../c" from the working directory, with . and .. resolved. */
static void absolute(const char *in, char *out, size_t cap) {
    char tmp[1024];
    if (in[0] == '/') {
        snprintf(tmp, sizeof tmp, "%s", in);
    } else {
        char cwd[512];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
    }
    size_t o = 0;
    out[o++] = '/';
    out[o] = '\0';
    char *save = tmp;
    while (*save) {
        while (*save == '/') save++;
        if (!*save) break;
        char *seg = save;
        while (*save && *save != '/') save++;
        size_t n = (size_t)(save - seg);
        if (n == 1 && seg[0] == '.') continue;
        if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            if (o > 1) {
                o--;                              /* the trailing '/' */
                while (o > 1 && out[o - 1] != '/') o--;
                out[o] = '\0';
            }
            continue;
        }
        if (o + n + 2 > cap) break;
        memcpy(out + o, seg, n);
        o += n;
        out[o++] = '/';
        out[o] = '\0';
    }
    if (o > 1) out[--o] = '\0';                  /* no trailing '/' */
}

/* The mount a path lives on: the longest mount point that is a prefix of
 * it at a '/' boundary. */
static const char *mount_of(const char *abs) {
    const char *best = "/";
    size_t bl = 1;
    for (int i = 0; i < g_nmount; i++) {
        const char *m = g_mount[i];
        size_t ml = strlen(m);
        if (strcmp(m, "/") == 0) continue;
        if (strncmp(abs, m, ml) == 0 && (abs[ml] == '\0' || abs[ml] == '/') &&
            ml > bl) {
            best = m;
            bl = ml;
        }
    }
    return best;
}

/* ---- rows --------------------------------------------------------------- */

#define NCOL 7
struct row { char col[NCOL][64]; };
static struct row g_rows[MAX_MOUNTS + 1];
static int g_nrows;

/* bytes, coreutils' -h: below 1024 a plain number; else one decimal under
 * 10 and none from 10 up, each rounded UP. */
static void human(uint64_t bytes, char *out, size_t cap) {
    static const char suf[] = "KMGTPE";
    if (bytes < 1024) { snprintf(out, cap, "%lu", (unsigned long)bytes); return; }
    uint64_t div = 1024;
    int u = 0;
    while (u < 5 && bytes / div >= 1024) { div *= 1024; u++; }
    uint64_t tenths = (bytes * 10 + div - 1) / div;
    if (tenths < 100) {
        snprintf(out, cap, "%lu.%lu%c", (unsigned long)(tenths / 10),
                 (unsigned long)(tenths % 10), suf[u]);
        return;
    }
    uint64_t whole = (bytes + div - 1) / div;
    if (whole >= 1024 && u < 5) { u++; div *= 1024; whole = (bytes + div - 1) / div;
        tenths = (bytes * 10 + div - 1) / div;
        if (tenths < 100) {
            snprintf(out, cap, "%lu.%lu%c", (unsigned long)(tenths / 10),
                     (unsigned long)(tenths % 10), suf[u]);
            return;
        }
    }
    snprintf(out, cap, "%lu%c", (unsigned long)whole, suf[u]);
}

static void size_col(uint64_t blocks, uint64_t bsize, char *out, size_t cap) {
    uint64_t bytes = blocks * bsize;
    if (opt_human) human(bytes, out, cap);
    else snprintf(out, cap, "%lu", (unsigned long)((bytes + 1023) / 1024));
}

static void add_header(void) {
    struct row *r = &g_rows[g_nrows++];
    int c = 0;
    snprintf(r->col[c++], 64, "Filesystem");
    if (opt_type) snprintf(r->col[c++], 64, "Type");
    snprintf(r->col[c++], 64, "%s", opt_human ? "Size" : "1K-blocks");
    snprintf(r->col[c++], 64, "Used");
    snprintf(r->col[c++], 64, "%s", opt_human ? "Avail" : "Available");
    snprintf(r->col[c++], 64, "Use%%");
    snprintf(r->col[c++], 64, "Mounted on");
}

/* 0 added, 1 skipped (no blocks, no -a), -1 error (reported, naming the
 * operand as it was typed). */
static int add_row(const char *path, const char *as_typed, const char *point,
                   int explicit_arg) {
    struct abi_statfs sf;
    memset(&sf, 0, sizeof sf);
    long rc = sc2(ABI_SYS_STATFS, (long)path, (long)&sf);
    if (rc < 0) {
        fprintf(stderr, "df: %s: %s\n", as_typed, strerror((int)-rc));
        return -1;
    }
    if (sf.blocks == 0 && !opt_all && !explicit_arg) return 1;
    if (g_nrows >= MAX_MOUNTS + 1) return 1;
    struct row *r = &g_rows[g_nrows++];
    const char *tn = type_name(sf.type_magic);
    uint64_t used = sf.blocks >= sf.bfree ? sf.blocks - sf.bfree : 0;
    int c = 0;
    snprintf(r->col[c++], 64, "%s", strcmp(tn, "-") ? tn : "none");
    if (opt_type) snprintf(r->col[c++], 64, "%s", tn);
    size_col(sf.blocks, sf.bsize, r->col[c++], 64);
    size_col(used, sf.bsize, r->col[c++], 64);
    size_col(sf.bavail, sf.bsize, r->col[c++], 64);
    uint64_t den = used + sf.bavail;
    if (den == 0) snprintf(r->col[c++], 64, "-");
    else snprintf(r->col[c++], 64, "%lu%%",
                  (unsigned long)((used * 100 + den - 1) / den));
    snprintf(r->col[c++], 64, "%s", point);
    return 0;
}

static void pad(int n) { while (n-- > 0) putchar(' '); }

/* coreutils' table: each column as wide as its widest cell and at least
 * its minimum (Filesystem 14, Type 4, the sizes 5, Use% 4), header
 * included; Filesystem and Type left-aligned, the numbers -- headers too --
 * right-aligned, one space apart; "Mounted on" last and unpadded. */
static void print_rows(void) {
    int ncol = opt_type ? 7 : 6;
    int ntext = opt_type ? 2 : 1;
    int w[NCOL];
    for (int c = 0; c < ncol; c++)
        w[c] = (c == 0) ? 14 : (c < ntext) ? 4 : (c == ncol - 2) ? 4 : 5;
    for (int i = 0; i < g_nrows; i++)
        for (int c = 0; c < ncol; c++) {
            int l = (int)strlen(g_rows[i].col[c]);
            if (l > w[c]) w[c] = l;
        }
    for (int i = 0; i < g_nrows; i++) {
        for (int c = 0; c < ncol; c++) {
            const char *s = g_rows[i].col[c];
            int l = (int)strlen(s);
            if (c == ncol - 1) { fputs(s, stdout); break; }
            if (c < ntext) { fputs(s, stdout); pad(w[c] - l); }
            else { pad(w[c] - l); fputs(s, stdout); }
            putchar(' ');
        }
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'a') opt_all = 1;
            else if (*f == 'h') opt_human = 1;
            else if (*f == 'k') opt_human = 0;
            else if (*f == 'T') opt_type = 1;
            else {
                fprintf(stderr, "df: invalid option -- '%c'\n", *f);
                fprintf(stderr, "Usage: df [-a] [-h] [-k] [-T] [FILE...]\n");
                return 1;
            }
        }
    }
    load_mounts();
    add_header();
    int status = 0;
    if (i >= argc) {
        if (g_nmount == 0) g_mount[g_nmount++] = strdup("/");
        for (int m = 0; m < g_nmount; m++)
            if (add_row(g_mount[m], g_mount[m], g_mount[m], 0) < 0) status = 1;
    } else {
        for (; i < argc; i++) {
            char abs[1024];
            absolute(argv[i], abs, sizeof abs);
            if (add_row(abs, argv[i], mount_of(abs), 1) < 0) status = 1;
        }
    }
    if (g_nrows > 1) print_rows();
    return status;
}
