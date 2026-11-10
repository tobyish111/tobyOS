/* zip / unzip -- the ZIP container, over the deflate this tree already has.
 *
 *     zip   [-0-9] [-q] archive.zip file ...
 *     unzip [-l] [-t] [-p] [-o] [-q] [-d dir] archive.zip [member ...]
 *
 * `zip` was one of the last essential names absent outright, and `unzip`
 * existed only as a busybox applet. Both are container work: a ZIP entry
 * stores a BARE deflate stream, which is why libtoby grew deflate_raw()
 * and puff grew puff_inflate_raw() rather than either tool carrying its
 * own copy of an LZ77 matcher.
 *
 * Read support covers stored (method 0) and deflated (method 8) entries,
 * which is what every ordinary .zip in the world uses. The encrypted and
 * bzip2/lzma methods are refused BY NAME rather than producing garbage.
 * Zip64 is refused for the same reason: the 4 GiB fields simply cannot say
 * what a larger archive means.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include <toby/deflate.h>
#include <tobyos/puff.h>

#define SIG_LOCAL   0x04034b50u
#define SIG_CENTRAL 0x02014b50u
#define SIG_EOCD    0x06054b50u

static const char *prog = "zip";
static int qflag, level = 6, status;

/* ------------------------------------------------------------- buffers */

struct buf { uint8_t *d; size_t len, cap; };

static int bneed(struct buf *b, size_t n) {
    if (b->len + n <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 65536;
    while (nc < b->len + n) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->d, nc);
    if (!p) return 0;
    b->d = p; b->cap = nc;
    return 1;
}
static int badd(struct buf *b, const void *p, size_t n) {
    if (!bneed(b, n)) return 0;
    memcpy(b->d + b->len, p, n);
    b->len += n;
    return 1;
}
static int b16(struct buf *b, unsigned v) {
    uint8_t t[2] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF) };
    return badd(b, t, 2);
}
static int b32(struct buf *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                     (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
    return badd(b, t, 4);
}

static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 65536, n = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) { fclose(f); return NULL; }
    for (;;) {
        if (n == cap) {
            uint8_t *nb = (uint8_t *)realloc(b, cap * 2);
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

/* ------------------------------------------------------------- creating */

static int add_entry(struct buf *out, struct buf *cd, const char *name,
                     uint32_t *count)
{
    size_t ulen = 0;
    uint8_t *data = slurp(name, &ulen);
    if (!data) {
        fprintf(stderr, "%s: %s: %s\n", prog, name, strerror(errno));
        status = 1;
        return -1;
    }
    uint32_t crc = gz_crc32(data, ulen, 0);

    uint8_t *comp = NULL;
    size_t clen = 0;
    int method = 8;
    if (level == 0 || ulen == 0 ||
        deflate_raw(data, ulen, level, &comp, &clen) != 0 || clen >= ulen) {
        /* Storing beats deflating when deflating did not help, which is
         * what keeps a .zip of already-compressed files from growing. */
        free(comp);
        comp = NULL;
        method = 0;
        clen = ulen;
    }

    size_t local_off = out->len;
    size_t nlen = strlen(name);

    b32(out, SIG_LOCAL);
    b16(out, 20); b16(out, 0); b16(out, (unsigned)method);
    b16(out, 0); b16(out, 0);                  /* dos time / date */
    b32(out, crc);
    b32(out, (uint32_t)clen);
    b32(out, (uint32_t)ulen);
    b16(out, (unsigned)nlen); b16(out, 0);
    badd(out, name, nlen);
    badd(out, method ? comp : data, clen);

    b32(cd, SIG_CENTRAL);
    b16(cd, 20); b16(cd, 20); b16(cd, 0); b16(cd, (unsigned)method);
    b16(cd, 0); b16(cd, 0);
    b32(cd, crc);
    b32(cd, (uint32_t)clen);
    b32(cd, (uint32_t)ulen);
    b16(cd, (unsigned)nlen); b16(cd, 0); b16(cd, 0);
    b16(cd, 0); b16(cd, 0);
    b32(cd, 0100644u << 16);                   /* external attributes */
    b32(cd, (uint32_t)local_off);
    badd(cd, name, nlen);

    if (!qflag)
        printf("  adding: %s (%s %zu -> %zu)\n", name,
               method ? "deflated" : "stored", ulen, clen);
    (*count)++;
    free(data);
    free(comp);
    return 0;
}

static int do_zip(int argc, char **argv) {
    const char *archive = NULL;
    const char *files[256];
    int nf = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                if (*p >= '0' && *p <= '9') level = *p - '0';
                else if (*p == 'q') qflag = 1;
                else if (*p == 'r') continue;   /* directories are walked by the caller */
            }
            continue;
        }
        if (!archive) archive = a;
        else if (nf < 256) files[nf++] = a;
    }
    if (!archive || nf == 0) {
        fprintf(stderr, "usage: zip [-0-9] [-q] archive.zip file ...\n");
        return 2;
    }

    struct buf out, cd;
    memset(&out, 0, sizeof out);
    memset(&cd, 0, sizeof cd);
    uint32_t count = 0;
    for (int i = 0; i < nf; i++) add_entry(&out, &cd, files[i], &count);

    size_t cd_off = out.len;
    badd(&out, cd.d, cd.len);
    b32(&out, SIG_EOCD);
    b16(&out, 0); b16(&out, 0);
    b16(&out, (unsigned)count); b16(&out, (unsigned)count);
    b32(&out, (uint32_t)cd.len);
    b32(&out, (uint32_t)cd_off);
    b16(&out, 0);

    FILE *f = fopen(archive, "wb");
    if (!f) {
        fprintf(stderr, "zip: cannot create %s: %s\n", archive, strerror(errno));
        free(out.d); free(cd.d);
        return 1;
    }
    fwrite(out.d, 1, out.len, f);
    fclose(f);
    free(out.d);
    free(cd.d);
    return status;
}

/* ----------------------------------------------------------- extracting */

static int mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(tmp, 0755);
        *p = '/';
    }
    return 0;
}

static int wanted(const char *name, int argc, char **argv, int first) {
    if (first >= argc) return 1;
    for (int i = first; i < argc; i++) {
        if (argv[i][0] == 0x2D) continue;   /* an option, not a member */
        if (!strcmp(name, argv[i])) return 1;
    }
    return 0;
}

static int do_unzip(int argc, char **argv) {
    const char *archive = NULL, *dir = NULL;
    int list = 0, over = 0, test = 0, pipe_out = 0, first_member = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "-d") && i + 1 < argc) { dir = argv[++i]; continue; }
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'l') list = 1;
                else if (*p == 'o') over = 1;
                else if (*p == 't') test = 1;
                else if (*p == 'p') { pipe_out = 1; qflag = 1; }
                else if (*p == 'q') qflag = 1;
            }
            continue;
        }
        if (!archive) { archive = a; first_member = i + 1; }
    }
    (void)over;
    if (!archive) {
        fprintf(stderr, "usage: unzip [-l] [-t] [-p] [-o] [-q] [-d dir] archive.zip\n");
        return 2;
    }

    size_t n = 0;
    uint8_t *z = slurp(archive, &n);
    if (!z) { fprintf(stderr, "unzip: cannot read %s\n", archive); return 2; }
    if (n < 22) { fprintf(stderr, "unzip: %s: not a zip archive\n", archive); free(z); return 2; }

    /* The end-of-central-directory record is at the END, and may be
     * followed by a comment, so it is searched for backwards. */
    long eocd = -1;
    for (long i = (long)n - 22; i >= 0; i--) {
        if (r32(z + i) == SIG_EOCD) { eocd = i; break; }
    }
    if (eocd < 0) { fprintf(stderr, "unzip: %s: not a zip archive\n", archive); free(z); return 2; }

    uint32_t total  = r16(z + eocd + 10);
    uint32_t cdsize = r32(z + eocd + 12);
    uint32_t cdoff  = r32(z + eocd + 16);
    if ((size_t)cdoff + cdsize > n) {
        fprintf(stderr, "unzip: %s: central directory past the file\n", archive);
        free(z); return 2;
    }

    if (list && !qflag) printf("  Length      Name\n---------  ----\n");
    if (test && !qflag) printf("Archive:  %s\n", archive);
    size_t p = cdoff;
    uint32_t seen = 0;
    for (; seen < total && p + 46 <= n; seen++) {
        if (r32(z + p) != SIG_CENTRAL) break;
        uint16_t method = r16(z + p + 10);
        uint32_t crc    = r32(z + p + 16);
        uint32_t clen   = r32(z + p + 20);
        uint32_t ulen   = r32(z + p + 24);
        uint16_t nlen   = r16(z + p + 28);
        uint16_t elen   = r16(z + p + 30);
        uint16_t klen   = r16(z + p + 32);
        uint32_t loff   = r32(z + p + 42);
        char name[1024];
        size_t take = nlen < sizeof name - 1 ? nlen : sizeof name - 1;
        memcpy(name, z + p + 46, take);
        name[take] = 0;
        p += 46 + nlen + elen + klen;

        if (!wanted(name, argc, argv, first_member)) continue;

        if (list) { printf("%9u  %s\n", ulen, name); continue; }

        if (ulen == 0xFFFFFFFFu || clen == 0xFFFFFFFFu) {
            fprintf(stderr, "unzip: %s: zip64 entries are not supported\n", name);
            status = 1;
            continue;
        }
        if (method != 0 && method != 8) {
            fprintf(stderr, "unzip: %s: compression method %u is not supported "
                            "(only stored and deflated)\n", name, method);
            status = 1;
            continue;
        }
        if ((size_t)loff + 30 > n || r32(z + loff) != SIG_LOCAL) {
            fprintf(stderr, "unzip: %s: bad local header\n", name);
            status = 1;
            continue;
        }
        uint16_t lnlen = r16(z + loff + 26);
        uint16_t lelen = r16(z + loff + 28);
        size_t data = loff + 30 + lnlen + lelen;
        if (data + clen > n) {
            fprintf(stderr, "unzip: %s: entry data past the file\n", name);
            status = 1;
            continue;
        }

        uint8_t *plain = NULL;
        if (method == 0) {
            plain = (uint8_t *)malloc(ulen ? ulen : 1);
            if (plain) memcpy(plain, z + data, ulen);
        } else {
            plain = (uint8_t *)malloc(ulen ? ulen : 1);
            unsigned long got = ulen;
            if (plain && puff_inflate_raw(plain, &got, z + data, clen) != 0) {
                fprintf(stderr, "unzip: %s: bad deflate stream\n", name);
                free(plain);
                status = 1;
                continue;
            }
        }
        if (!plain) { fprintf(stderr, "unzip: out of memory\n"); status = 2; break; }

        /* The stored CRC is checked -- an archive that unpacks to the wrong
         * bytes is worse than one that refuses. */
        if (gz_crc32(plain, ulen, 0) != crc) {
            fprintf(stderr, "unzip: %s: CRC error\n", name);
            free(plain);
            status = 1;
            continue;
        }

        if (test) {
            if (!qflag) printf("    testing: %-24s OK\n", name);
            free(plain);
            continue;
        }
        if (pipe_out) {
            if (ulen) fwrite(plain, 1, ulen, stdout);
            free(plain);
            continue;
        }

        char dest[1200];
        if (dir) snprintf(dest, sizeof dest, "%s/%s", dir, name);
        else     snprintf(dest, sizeof dest, "%s", name);

        size_t dl = strlen(dest);
        if (dl && dest[dl - 1] == '/') {         /* a directory entry */
            mkdirs(dest);
            mkdir(dest, 0755);
            free(plain);
            continue;
        }
        mkdirs(dest);
        FILE *o = fopen(dest, "wb");
        if (!o) {
            fprintf(stderr, "unzip: cannot create %s: %s\n", dest, strerror(errno));
            free(plain);
            status = 1;
            continue;
        }
        if (ulen) fwrite(plain, 1, ulen, o);
        fclose(o);
        free(plain);
        if (!qflag) printf("  inflating: %s\n", dest);
    }

    if (test && !qflag) {
        if (status == 0) printf("No errors detected in compressed data of %s.\n", archive);
        else fprintf(stderr, "At least one error was detected in %s.\n", archive);
    }
    free(z);
    return status;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "zip";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    if (!strcmp(base, "unzip")) return do_unzip(argc, argv);
    return do_zip(argc, argv);
}
