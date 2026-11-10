/* ranlib -- write the symbol index into an ar archive.
 *
 *     ranlib [-t] [-v] archive.a ...
 *     ranlib -D archive.a          deterministic (the default here)
 *
 * An archive without an index is just a bag of object files: the linker has
 * to read every member to find out what each one defines. The index is the
 * "/" member at the front holding, in big-endian:
 *
 *     uint32  number of symbols
 *     uint32  archive offset of the member defining symbol i, for each i
 *     char[]  the symbol names, NUL-terminated, in the same order
 *
 * DETERMINISTIC BY DEFAULT. GNU ranlib stamps the index member with the
 * current time unless told otherwise; this writes 0, so building the same
 * archive twice produces the same bytes. That is what makes the output
 * comparable to `ranlib -D` on the host, which is how it is tested.
 *
 * Long member names live in the "//" member and a header name of "/N" is an
 * offset into it -- an archive built from paths longer than 15 characters
 * is the normal case, not an edge one, so it is handled rather than
 * refused.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;

#define ARMAG   "!<arch>\n"
#define SARMAG  8
#define ARFMAG  "`\n"

struct ar_hdr {
    char name[16], date[12], uid[6], gid[6], mode[8], size[10], fmag[2];
};

static int verbose;

static u32 rd32le(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64le(const u8 *p) { return (u64)rd32le(p) | ((u64)rd32le(p + 4) << 32); }

static void wr32be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

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

/* An ar size/date field is space-padded decimal, not NUL-terminated. */
static long field_num(const char *f, size_t n) {
    char tmp[16];
    size_t l = n < sizeof tmp - 1 ? n : sizeof tmp - 1;
    memcpy(tmp, f, l);
    tmp[l] = 0;
    return atol(tmp);
}

struct member {
    size_t hdr_off;        /* offset of this member's 60-byte header */
    size_t data_off;
    size_t size;
    char   name[256];
};

struct symref { char name[256]; int member; };

/* Collects the DEFINED global symbols of one ELF64 member. A symbol that is
 * undefined (SHN_UNDEF) or local defines nothing and must not be indexed --
 * an index that claims a member defines a symbol it only references sends
 * the linker to the wrong object. */
static int elf_globals(const u8 *o, size_t olen, int midx,
                       struct symref **out, size_t *nout, size_t *cap)
{
    if (olen < 64 || memcmp(o, "\177ELF", 4) || o[4] != 2 || o[5] != 1) return 0;
    u64 shoff = rd64le(o + 0x28);
    int shentsize = (int)(o[0x3A] | (o[0x3B] << 8));
    int shnum = (int)(o[0x3C] | (o[0x3D] << 8));
    if (shentsize != 64 || shoff == 0 || shoff + (u64)shnum * 64 > olen) return 0;

    for (int i = 0; i < shnum; i++) {
        const u8 *sh = o + shoff + (u64)i * 64;
        u32 type = rd32le(sh + 4);
        if (type != 2 /* SHT_SYMTAB */) continue;
        u64 off = rd64le(sh + 24), size = rd64le(sh + 32);
        u32 link = rd32le(sh + 40);
        u64 entsize = rd64le(sh + 56);
        if (entsize != 24 || off + size > olen || link >= (u32)shnum) continue;
        const u8 *strsh = o + shoff + (u64)link * 64;
        u64 stroff = rd64le(strsh + 24), strsize = rd64le(strsh + 32);
        if (stroff + strsize > olen) continue;
        const char *str = (const char *)(o + stroff);

        for (u64 k = 0; k + 24 <= size; k += 24) {
            const u8 *sym = o + off + k;
            u32 nameoff = rd32le(sym);
            u8 info = sym[4];
            u16 shndx = (u16)(sym[6] | (sym[7] << 8));
            int bind = info >> 4;
            if (bind != 1 /* GLOBAL */ && bind != 2 /* WEAK */) continue;
            if (shndx == 0 /* SHN_UNDEF */) continue;
            if (nameoff == 0 || nameoff >= strsize) continue;
            const char *nm = str + nameoff;
            if (!*nm) continue;
            if (*nout == *cap) {
                *cap = *cap ? *cap * 2 : 256;
                struct symref *ns = (struct symref *)realloc(*out, *cap * sizeof **out);
                if (!ns) return 0;
                *out = ns;
            }
            snprintf((*out)[*nout].name, sizeof (*out)[0].name, "%s", nm);
            (*out)[*nout].member = midx;
            (*nout)++;
        }
    }
    return 1;
}

static int do_archive(const char *path, int list_only) {
    size_t len = 0;
    u8 *a = slurp(path, &len);
    if (!a) {
        fprintf(stderr, "ranlib: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (len < SARMAG || memcmp(a, ARMAG, SARMAG)) {
        fprintf(stderr, "ranlib: %s: not an archive\n", path);
        free(a);
        return 1;
    }

    /* Pass 1: walk the members, keeping the long-name table if present and
     * SKIPPING any index that is already there -- rewriting means building
     * a new one, not appending to the old. */
    struct member mem[512];
    int nmem = 0;
    const char *longnames = NULL;
    size_t longnames_len = 0;

    size_t off = SARMAG;
    while (off + sizeof(struct ar_hdr) <= len) {
        const struct ar_hdr *h = (const struct ar_hdr *)(a + off);
        if (memcmp(h->fmag, ARFMAG, 2)) break;
        long size = field_num(h->size, sizeof h->size);
        if (size < 0 || off + sizeof *h + (size_t)size > len) break;
        size_t data = off + sizeof *h;

        char raw[17];
        memcpy(raw, h->name, 16);
        raw[16] = 0;
        for (int i = 15; i >= 0 && (raw[i] == ' '); i--) raw[i] = 0;

        if (!strcmp(raw, "//")) {
            longnames = (const char *)(a + data);
            longnames_len = (size_t)size;
        } else if (!strcmp(raw, "/") || !strcmp(raw, "__.SYMDEF")) {
            /* the existing index -- dropped */
        } else if (nmem < 512) {
            struct member *m = &mem[nmem];
            m->hdr_off = off;
            m->data_off = data;
            m->size = (size_t)size;
            if (raw[0] == '/' && raw[1] >= '0' && raw[1] <= '9' && longnames) {
                size_t lo = (size_t)atol(raw + 1);
                size_t k = 0;
                m->name[0] = 0;
                while (lo + k < longnames_len && longnames[lo + k] != '/' &&
                       longnames[lo + k] != '\n' && k < sizeof m->name - 1) {
                    m->name[k] = longnames[lo + k];
                    k++;
                }
                m->name[k] = 0;
            } else {
                size_t l = strlen(raw);
                if (l && raw[l - 1] == '/') raw[l - 1] = 0;   /* GNU name terminator */
                snprintf(m->name, sizeof m->name, "%s", raw);
            }
            nmem++;
        }
        off = data + (size_t)size + ((size_t)size & 1);
    }

    struct symref *syms = NULL;
    size_t nsym = 0, cap = 0;
    for (int i = 0; i < nmem; i++)
        elf_globals(a + mem[i].data_off, mem[i].size, i, &syms, &nsym, &cap);

    if (list_only) {
        printf("%s: %d member(s), %u symbol(s)\n", path, nmem, (unsigned)nsym);
        for (size_t i = 0; i < nsym; i++)
            printf("  %-32s %s\n", syms[i].name, mem[syms[i].member].name);
        free(syms);
        free(a);
        return 0;
    }

    /* The index's own size depends on where the members land, and where the
     * members land depends on the index's size. Both are known up front:
     * the index is 4 + 4*nsym + the name bytes, padded to even. */
    size_t names_len = 0;
    for (size_t i = 0; i < nsym; i++) names_len += strlen(syms[i].name) + 1;
    size_t idx_len = 4 + 4 * nsym + names_len;
    size_t idx_pad = idx_len & 1;

    /* Everything after the archive magic that is NOT an old index or the
     * long-name table gets copied verbatim; the long-name table must stay
     * FIRST after the index, because "/N" names index into it. */
    size_t body_cap = len + idx_len + 128;
    u8 *body = (u8 *)malloc(body_cap);
    if (!body) { free(syms); free(a); fprintf(stderr, "ranlib: out of memory\n"); return 1; }
    size_t blen = 0;
    size_t base = SARMAG + sizeof(struct ar_hdr) + idx_len + idx_pad;

    if (longnames) {
        const struct ar_hdr *lh = (const struct ar_hdr *)(longnames - sizeof(struct ar_hdr));
        size_t whole = sizeof *lh + longnames_len + (longnames_len & 1);
        memcpy(body + blen, lh, whole);
        blen += whole;
    }
    size_t *newoff = (size_t *)calloc((size_t)(nmem ? nmem : 1), sizeof *newoff);
    if (!newoff) { free(body); free(syms); free(a); return 1; }
    for (int i = 0; i < nmem; i++) {
        size_t whole = sizeof(struct ar_hdr) + mem[i].size + (mem[i].size & 1);
        newoff[i] = base + blen;
        memcpy(body + blen, a + mem[i].hdr_off, whole);
        blen += whole;
    }

    u8 *idx = (u8 *)calloc(1, idx_len + 1);
    if (!idx) { free(newoff); free(body); free(syms); free(a); return 1; }
    wr32be(idx, (u32)nsym);
    size_t np = 4 + 4 * nsym;
    for (size_t i = 0; i < nsym; i++) {
        wr32be(idx + 4 + 4 * i, (u32)newoff[syms[i].member]);
        size_t l = strlen(syms[i].name) + 1;
        memcpy(idx + np, syms[i].name, l);
        np += l;
    }

    FILE *o = fopen(path, "wb");
    if (!o) {
        fprintf(stderr, "ranlib: cannot rewrite %s: %s\n", path, strerror(errno));
        free(idx); free(newoff); free(body); free(syms); free(a);
        return 1;
    }
    fwrite(ARMAG, 1, SARMAG, o);
    struct ar_hdr ih;
    memset(&ih, ' ', sizeof ih);
    ih.name[0] = '/';
    /* Deterministic: a zero timestamp, uid, gid and mode 0 -- the same
     * choice `ranlib -D` makes, and the reason two builds of one archive
     * compare equal. */
    memcpy(ih.date, "0", 1);
    memcpy(ih.uid, "0", 1);
    memcpy(ih.gid, "0", 1);
    memcpy(ih.mode, "0", 1);
    char sz[12];
    int n = snprintf(sz, sizeof sz, "%u", (unsigned)idx_len);
    memcpy(ih.size, sz, (size_t)n);
    memcpy(ih.fmag, ARFMAG, 2);
    fwrite(&ih, 1, sizeof ih, o);
    fwrite(idx, 1, idx_len, o);
    if (idx_pad) fputc('\n', o);
    fwrite(body, 1, blen, o);
    fclose(o);

    if (verbose)
        printf("ranlib: %s: %d member(s), %u symbol(s) indexed\n",
               path, nmem, (unsigned)nsym);

    free(idx); free(newoff); free(body); free(syms); free(a);
    return 0;
}

int main(int argc, char **argv) {
    int list_only = 0, nfiles = 0, rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-t")) { list_only = 1; continue; }
        if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; continue; }
        if (!strcmp(a, "-D")) continue;          /* deterministic: already the default */
        if (!strcmp(a, "-U")) {
            fprintf(stderr, "ranlib: -U (real timestamps) is not supported; "
                            "this ranlib is always deterministic\n");
            return 1;
        }
        if (!strcmp(a, "--version")) { printf("ranlib (tobyOS) 1.0\n"); return 0; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: ranlib [-t] [-v] archive.a ...\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "ranlib: unknown option %s\n", a); return 1; }
        nfiles++;
        rc |= do_archive(a, list_only);
    }
    if (!nfiles) { fprintf(stderr, "usage: ranlib [-t] [-v] archive.a ...\n"); return 1; }
    return rc;
}
