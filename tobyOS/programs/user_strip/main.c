/* strip -- drop symbol and debug sections from an ELF64 object.
 *
 *     strip [-s|-g|-d|-S] [-R section] [-o out] [-p] file ...
 *
 * This tree self-hosts: tcc, as and ld all run here, so the thing that
 * produces a smaller binary after a build belongs here too.
 *
 * THE RULE THAT MAKES THIS SAFE: every section the loader can see keeps
 * its original file offset. The program headers point at file offsets, not
 * at sections, so moving an SHF_ALLOC section would break a PT_LOAD that
 * no section header records. Only the tail -- the section header table and
 * the rebuilt name table -- is written anywhere new.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

struct ehdr64 {
    u8  ident[16];
    u16 type, machine;
    u32 version;
    u64 entry, phoff, shoff;
    u32 flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct shdr64 {
    u32 name, type;
    u64 flags, addr, offset, size;
    u32 link, info;
    u64 align, entsize;
};

#define SHT_NULL     0
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_NOBITS   8
#define SHF_ALLOC    0x2

static const char *prog = "strip";
static int status;
static int strip_debug_only, preserve_dates, verbose;
static const char *outfile;
static const char *removes[32];
static int nremoves;

static int u16at(const u8 *p) { return p[0] | (p[1] << 8); }

/* Little-endian accessors: the file is x86-64, and reading it field by
 * field rather than casting a struct over it keeps this honest about
 * alignment. */
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { for (int i = 0; i < 4; i++) p[i] = (u8)(v >> (i * 8)); }
static void wr64(u8 *p, u64 v) { for (int i = 0; i < 8; i++) p[i] = (u8)(v >> (i * 8)); }

static u8 *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    u8 *b = (u8 *)malloc(cap);
    if (!b) { fclose(f); return NULL; }
    for (;;) {
        if (n == cap) {
            u8 *nb = (u8 *)realloc(b, cap * 2);
            if (!nb) { free(b); fclose(f); return NULL; }
            b = nb; cap *= 2;
        }
        size_t got = fread(b + n, 1, cap - n, f);
        if (!got) break;
        n += got;
    }
    fclose(f);
    *len = n;
    return b;
}

static int named_for_removal(const char *nm) {
    for (int i = 0; i < nremoves; i++) if (!strcmp(nm, removes[i])) return 1;
    return 0;
}

/* .debug_*, .zdebug_* and the DWARF companions gcc and clang emit. */
static int is_debug_section(const char *nm) {
    return !strncmp(nm, ".debug", 6) || !strncmp(nm, ".zdebug", 7)
        || !strcmp(nm, ".stab") || !strcmp(nm, ".stabstr")
        || !strcmp(nm, ".line") || !strncmp(nm, ".gnu.debug", 10);
}

static int do_file(const char *path) {
    size_t len = 0;
    u8 *f = slurp(path, &len);
    if (!f) {
        fprintf(stderr, "%s: %s: %s\n", prog, path, strerror(errno));
        status = 1;
        return -1;
    }
    if (len < 64 || memcmp(f, "\177ELF", 4) || f[4] != 2 || f[5] != 1) {
        fprintf(stderr, "%s: %s: not a little-endian ELF64 file\n", prog, path);
        free(f);
        status = 1;
        return -1;
    }

    u64 shoff     = rd64(f + 0x28);
    int shentsize = u16at(f + 0x3A);
    int shnum     = u16at(f + 0x3C);
    int shstrndx  = u16at(f + 0x3E);

    if (shnum == 0 || shoff == 0) {
        if (verbose) printf("%s: %s: no section headers, nothing to strip\n", prog, path);
        free(f);
        return 0;
    }
    if (shentsize != 64 || shoff + (u64)shnum * 64 > len || shstrndx >= shnum) {
        fprintf(stderr, "%s: %s: the section header table does not fit the file\n", prog, path);
        free(f);
        status = 1;
        return -1;
    }

    struct shdr64 *sh = (struct shdr64 *)calloc((size_t)shnum, sizeof *sh);
    if (!sh) { free(f); fprintf(stderr, "%s: out of memory\n", prog); return -1; }
    for (int i = 0; i < shnum; i++) {
        const u8 *p = f + shoff + (u64)i * 64;
        sh[i].name = rd32(p);       sh[i].type = rd32(p + 4);
        sh[i].flags = rd64(p + 8);  sh[i].addr = rd64(p + 16);
        sh[i].offset = rd64(p + 24); sh[i].size = rd64(p + 32);
        sh[i].link = rd32(p + 40);  sh[i].info = rd32(p + 44);
        sh[i].align = rd64(p + 48); sh[i].entsize = rd64(p + 56);
    }

    const char *shstr = (const char *)(f + sh[shstrndx].offset);
    u64 shstrsize = sh[shstrndx].size;
    if (sh[shstrndx].offset + shstrsize > len) {
        fprintf(stderr, "%s: %s: the section name table does not fit the file\n", prog, path);
        free(sh); free(f); status = 1; return -1;
    }
    /* A name index past the table would read off the end of the mapping. */
#define SNAME(i) ((sh[i].name < shstrsize) ? shstr + sh[i].name : "")

    int *keep = (int *)calloc((size_t)shnum, sizeof *keep);
    int *newidx = (int *)calloc((size_t)shnum, sizeof *newidx);
    if (!keep || !newidx) { free(keep); free(newidx); free(sh); free(f); return -1; }

    for (int i = 0; i < shnum; i++) {
        const char *nm = SNAME(i);
        keep[i] = 1;
        if (i == 0 || i == shstrndx) continue;       /* NULL and the name table stay */
        if (sh[i].flags & SHF_ALLOC) continue;       /* the loader can see it */
        if (named_for_removal(nm)) { keep[i] = 0; continue; }
        if (is_debug_section(nm)) { keep[i] = 0; continue; }
        if (strip_debug_only) continue;
        /* -s: the symbol table and the string table it points at. .dynsym
         * and .dynstr are SHF_ALLOC and were already kept above. */
        if (sh[i].type == SHT_SYMTAB) { keep[i] = 0; continue; }
        if (sh[i].type == SHT_STRTAB && !strcmp(nm, ".strtab")) { keep[i] = 0; continue; }
        if (!strcmp(nm, ".comment")) keep[i] = 0;
    }

    /* Everything kept must stay where it is, so the file can only be cut
     * after the last byte any kept section occupies. */
    u64 cut = 64;
    u64 phoff = rd64(f + 0x20);
    int phnum = u16at(f + 0x38), phentsize = u16at(f + 0x36);
    if (phnum) {
        u64 e = phoff + (u64)phnum * (u64)phentsize;
        if (e > cut) cut = e;
    }
    int nkeep = 0;
    for (int i = 0; i < shnum; i++) {
        if (!keep[i]) continue;
        newidx[i] = nkeep++;
        if (i == shstrndx) continue;                 /* rewritten at the tail */
        if (sh[i].type == SHT_NOBITS) continue;      /* occupies no file bytes */
        u64 e = sh[i].offset + sh[i].size;
        if (e > cut) cut = e;
    }
    if (cut > len) cut = len;

    /* The new name table holds only the names that survived. */
    size_t strcap = 1;
    for (int i = 0; i < shnum; i++) if (keep[i]) strcap += strlen(SNAME(i)) + 1;
    char *nstr = (char *)malloc(strcap);
    u32 *noff = (u32 *)calloc((size_t)shnum, sizeof *noff);
    if (!nstr || !noff) { free(nstr); free(noff); free(keep); free(newidx); free(sh); free(f); return -1; }
    size_t sn = 0;
    nstr[sn++] = 0;
    for (int i = 0; i < shnum; i++) {
        if (!keep[i]) continue;
        const char *nm = SNAME(i);
        if (!*nm) { noff[i] = 0; continue; }
        noff[i] = (u32)sn;
        size_t l = strlen(nm) + 1;
        memcpy(nstr + sn, nm, l);
        sn += l;
    }

    u64 nstroff = (cut + 15) & ~(u64)15;
    u64 nshoff  = (nstroff + sn + 15) & ~(u64)15;
    u64 total   = nshoff + (u64)nkeep * 64;

    u8 *out = (u8 *)calloc(1, (size_t)total);
    if (!out) { free(nstr); free(noff); free(keep); free(newidx); free(sh); free(f);
                fprintf(stderr, "%s: out of memory\n", prog); return -1; }
    memcpy(out, f, (size_t)cut);
    memcpy(out + nstroff, nstr, sn);

    for (int i = 0; i < shnum; i++) {
        if (!keep[i]) continue;
        u8 *p = out + nshoff + (u64)newidx[i] * 64;
        u64 off = sh[i].offset, size = sh[i].size;
        u32 link = sh[i].link, info = sh[i].info;
        if (i == shstrndx) { off = nstroff; size = sn; }
        /* An sh_link into a section that is gone must not keep pointing at
         * whatever now sits at that index. */
        link = (link < (u32)shnum && keep[link]) ? (u32)newidx[link] : 0;
        if (sh[i].type == 4 /* RELA */ || sh[i].type == 9 /* REL */)
            info = (info < (u32)shnum && keep[info]) ? (u32)newidx[info] : 0;
        wr32(p,      noff[i]);
        wr32(p + 4,  sh[i].type);
        wr64(p + 8,  sh[i].flags);
        wr64(p + 16, sh[i].addr);
        wr64(p + 24, off);
        wr64(p + 32, size);
        wr32(p + 40, link);
        wr32(p + 44, info);
        wr64(p + 48, sh[i].align);
        wr64(p + 56, sh[i].entsize);
    }

    wr64(out + 0x28, nshoff);
    wr16(out + 0x3C, (u16)nkeep);
    wr16(out + 0x3E, (u16)newidx[shstrndx]);

    const char *dest = outfile ? outfile : path;
    struct stat before;
    int had_stat = (stat(path, &before) == 0);

    FILE *o = fopen(dest, "wb");
    if (!o) {
        fprintf(stderr, "%s: cannot write %s: %s\n", prog, dest, strerror(errno));
        status = 1;
    } else {
        if (fwrite(out, 1, (size_t)total, o) != total) {
            fprintf(stderr, "%s: %s: short write\n", prog, dest);
            status = 1;
        }
        fclose(o);
        if (had_stat) chmod(dest, before.st_mode & 07777);
        if (verbose)
            printf("%s: %s: %llu -> %llu bytes, %d of %d sections kept\n",
                   prog, dest, (unsigned long long)len,
                   (unsigned long long)total, nkeep, shnum);
    }
    (void)preserve_dates;

    free(out); free(nstr); free(noff); free(keep); free(newidx); free(sh); free(f);
    return 0;
#undef SNAME
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "strip";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;

    const char *files[256];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--strip-debug") || !strcmp(a, "--strip-dwo")) { strip_debug_only = 1; continue; }
        if (!strcmp(a, "--strip-all")) { strip_debug_only = 0; continue; }
        if (!strcmp(a, "--preserve-dates")) { preserve_dates = 1; continue; }
        if (!strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strncmp(a, "--remove-section=", 17)) {
            if (nremoves < 32) removes[nremoves++] = a + 17;
            continue;
        }
        if (!strcmp(a, "--version")) { printf("strip (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "--help")) {
            printf("usage: strip [-s|-g] [-R section] [-o out] [-p] [-v] file ...\n");
            return 0;
        }
        if (!strcmp(a, "-o") && i + 1 < argc) { outfile = argv[++i]; continue; }
        if (!strcmp(a, "-R") && i + 1 < argc) {
            if (nremoves < 32) removes[nremoves++] = argv[++i];
            continue;
        }
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 's': strip_debug_only = 0; break;
                /* -g, -S and -d all mean "debug only" to GNU strip. */
                case 'g': case 'S': case 'd': strip_debug_only = 1; break;
                case 'p': preserve_dates = 1; break;
                case 'v': verbose = 1; break;
                case 'x': case 'X': break;   /* local-symbol pruning: -s covers it */
                default:
                    fprintf(stderr, "%s: unknown option -- '%c'\n", prog, *p);
                    return 1;
                }
            }
            continue;
        }
        if (nf < 256) files[nf++] = a;
    }

    if (nf == 0) {
        fprintf(stderr, "usage: strip [-s|-g] [-R section] [-o out] [-p] [-v] file ...\n");
        return 1;
    }
    if (outfile && nf > 1) {
        fprintf(stderr, "%s: -o takes a single input file\n", prog);
        return 1;
    }
    for (int i = 0; i < nf; i++) do_file(files[i]);
    return status;
}
