/* tar -- create, list and extract ustar archives.
 *
 *     tar -c [-zjv] [-C dir] -f archive file...
 *     tar -t [-zjv]          -f archive
 *     tar -x [-zjv] [-C dir] -f archive [member...]
 *
 * The last name in the essential set that existed only as a busybox
 * applet. It is worth having natively for a second reason: the initrd
 * this system boots from IS a ustar archive, so the format is already
 * load-bearing here -- but only the kernel could read one.
 *
 * -z and -j are real, not shelled out: gz_compress/puff_gzip and
 * bz_compress/bz_decompress are in libtoby, so a compressed archive is
 * built and read in one process. The archive is assembled in memory
 * before compression because a gzip stream cannot be written
 * incrementally without a streaming encoder, and these archives are the
 * size of a source tree, not a disk.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include <toby/deflate.h>
#include <toby/bzip2.h>
#include <tobyos/puff.h>

#define BLK 512

static int c_create, x_extract, t_list, v_verbose;
static int z_gzip, j_bzip2;
static const char *f_archive;
static const char *C_dir;
static int status;

/* ------------------------------------------------------- growable buffer */

struct buf { uint8_t *d; size_t len, cap; };

static int buf_need(struct buf *b, size_t n) {
    if (b->len + n <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 65536;
    while (nc < b->len + n) nc *= 2;
    uint8_t *p = (uint8_t *)realloc(b->d, nc);
    if (!p) return 0;
    b->d = p; b->cap = nc;
    return 1;
}
static int buf_add(struct buf *b, const void *p, size_t n) {
    if (!buf_need(b, n)) return 0;
    memcpy(b->d + b->len, p, n);
    b->len += n;
    return 1;
}
static int buf_zero(struct buf *b, size_t n) {
    if (!buf_need(b, n)) return 0;
    memset(b->d + b->len, 0, n);
    b->len += n;
    return 1;
}

/* --------------------------------------------------------- ustar header */

struct ustar {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12];
    char chksum[8], typeflag, linkname[100], magic[6], version[2];
    char uname[32], gname[32], devmajor[8], devminor[8], prefix[155], pad[12];
};

static void oct(char *dst, size_t width, unsigned long long v) {
    /* ustar numbers are octal, right-aligned, zero-padded, NUL-terminated
     * -- width-1 digits and a terminator. */
    memset(dst, '0', width - 1);
    dst[width - 1] = 0;
    for (size_t i = width - 1; i > 0 && v; i--) {
        dst[i - 1] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

static unsigned long long unoct(const char *s, size_t n) {
    unsigned long long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '7') { if (v) break; else continue; }
        v = v * 8 + (unsigned long long)(s[i] - '0');
    }
    return v;
}

/* The header checksum is computed with the checksum field itself read as
 * eight spaces -- the one rule everyone gets wrong the first time. */
static unsigned long hdr_sum(const struct ustar *h) {
    const unsigned char *p = (const unsigned char *)h;
    unsigned long s = 0;
    for (size_t i = 0; i < BLK; i++)
        s += (i >= 148 && i < 156) ? (unsigned char)' ' : p[i];
    return s;
}

static void fill_header(struct ustar *h, const char *name,
                        const struct stat *st, char type)
{
    memset(h, 0, sizeof *h);
    size_t nl = strlen(name);
    if (nl < sizeof h->name) memcpy(h->name, name, nl);
    else {
        /* ustar splits a long path at a '/' into prefix + name. */
        size_t split = nl - (sizeof h->name - 1);
        while (split < nl && name[split] != '/') split++;
        if (split < nl && split < sizeof h->prefix) {
            memcpy(h->prefix, name, split);
            memcpy(h->name, name + split + 1, nl - split - 1);
        } else {
            memcpy(h->name, name + nl - (sizeof h->name - 1), sizeof h->name - 1);
        }
    }
    oct(h->mode,  8, (unsigned long long)(st->st_mode & 07777));
    oct(h->uid,   8, (unsigned long long)st->st_uid);
    oct(h->gid,   8, (unsigned long long)st->st_gid);
    oct(h->size, 12, type == '5' ? 0ull : (unsigned long long)st->st_size);
    oct(h->mtime,12, (unsigned long long)st->st_mtime);
    h->typeflag = type;
    memcpy(h->magic, "ustar", 5);
    memcpy(h->version, "00", 2);
    memcpy(h->uname, "root", 4);
    memcpy(h->gname, "root", 4);
    oct(h->devmajor, 8, 0);
    oct(h->devminor, 8, 0);
    unsigned long s = hdr_sum(h);
    /* Six octal digits, a NUL, then a space -- the historical layout. */
    oct(h->chksum, 7, s);
    h->chksum[6] = 0;
    h->chksum[7] = ' ';
}

/* ------------------------------------------------------------- creating */

static int add_path(struct buf *b, const char *path);

static int add_dir(struct buf *b, const char *path) {
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "tar: %s: %s\n", path, strerror(errno)); status = 2; return -1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        add_path(b, sub);
    }
    closedir(d);
    return 0;
}

static int add_path(struct buf *b, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "tar: %s: %s\n", path, strerror(errno));
        status = 2;
        return -1;
    }
    struct ustar h;
    if (S_ISDIR(st.st_mode)) {
        char withslash[1024];
        snprintf(withslash, sizeof withslash, "%s/", path);
        fill_header(&h, withslash, &st, '5');
        if (!buf_add(b, &h, BLK)) return -1;
        if (v_verbose) printf("%s\n", withslash);
        return add_dir(b, path);
    }
    fill_header(&h, path, &st, '0');
    if (!buf_add(b, &h, BLK)) return -1;
    if (v_verbose) printf("%s\n", path);

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "tar: %s: %s\n", path, strerror(errno)); status = 2; return -1; }
    char blk[BLK];
    size_t remaining = (size_t)st.st_size;
    while (remaining) {
        size_t want = remaining < BLK ? remaining : BLK;
        size_t got = fread(blk, 1, want, f);
        if (got == 0) break;
        if (got < BLK) memset(blk + got, 0, BLK - got);
        if (!buf_add(b, blk, BLK)) { fclose(f); return -1; }
        remaining -= got;
    }
    fclose(f);
    return 0;
}

/* ----------------------------------------------------------- compression */

static int write_out(const uint8_t *d, size_t n) {
    FILE *o = (!f_archive || !strcmp(f_archive, "-")) ? stdout : fopen(f_archive, "wb");
    if (!o) { fprintf(stderr, "tar: cannot create %s\n", f_archive); return 1; }
    size_t w = fwrite(d, 1, n, o);
    if (o != stdout) fclose(o);
    return w == n ? 0 : 1;
}

static int read_in(uint8_t **out, size_t *outlen) {
    FILE *f = (!f_archive || !strcmp(f_archive, "-")) ? stdin : fopen(f_archive, "rb");
    if (!f) { fprintf(stderr, "tar: %s: %s\n", f_archive, strerror(errno)); return -1; }
    size_t cap = 1 << 20, len = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) { if (f != stdin) fclose(f); return -1; }
    for (;;) {
        if (len == cap) {
            uint8_t *nb = (uint8_t *)realloc(b, cap * 2);
            if (!nb) { free(b); if (f != stdin) fclose(f); return -1; }
            b = nb; cap *= 2;
        }
        size_t got = fread(b + len, 1, cap - len, f);
        if (got == 0) break;
        len += got;
    }
    if (f != stdin) fclose(f);

    /* Decompress by MAGIC, not by whether -z or -j was given: `tar -tf
     * x.tgz` without -z is something everyone types, and GNU handles it. */
    if (len > 2 && b[0] == 0x1F && b[1] == 0x8B) {
        unsigned long cap2 = gz_isize(b, len);
        if (cap2 == 0) cap2 = (unsigned long)len * 8;
        if (cap2 < 65536) cap2 = 65536;
        for (int tries = 0; tries < 24; tries++) {
            uint8_t *o = (uint8_t *)malloc(cap2);
            if (!o) { free(b); return -1; }
            unsigned long got = cap2;
            int pr = puff_gzip(o, &got, b, (unsigned long)len);
            if (pr == PUFF_TRUNC) { free(o); cap2 *= 4; continue; }
            if (pr != PUFF_OK) {
                free(o); free(b);
                fprintf(stderr, "tar: archive is not a valid gzip stream\n");
                return -1;
            }
            free(b);
            *out = o; *outlen = (size_t)got;
            return 0;
        }
        free(b);
        fprintf(stderr, "tar: gzip stream did not fit\n");
        return -1;
    }
    if (len > 3 && b[0] == 'B' && b[1] == 'Z' && b[2] == 'h') {
        uint8_t *o = NULL; size_t olen = 0; const char *why = "";
        if (bz_decompress(b, len, &o, &olen, &why) != 0) {
            fprintf(stderr, "tar: %s\n", why);
            free(b);
            return -1;
        }
        free(b);
        *out = o; *outlen = olen;
        return 0;
    }
    *out = b; *outlen = len;
    return 0;
}

/* -------------------------------------------------- listing / extracting */

static int want_member(const char *name, int argc, char **argv, int first) {
    if (first >= argc) return 1;         /* no members named: take all */
    for (int i = first; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (!strcmp(name, argv[i])) return 1;
        if (!strncmp(name, argv[i], l) && (name[l] == '/' || name[l] == 0)) return 1;
    }
    return 0;
}

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

static int walk_archive(const uint8_t *d, size_t n, int argc, char **argv, int first) {
    /* Anything shorter than a single header block is not an archive. Without
     * this the loop below simply never runs and a text file reads as an
     * EMPTY archive, exiting 0 -- which is the wrong answer to
     * `tar -tf notes.txt`. */
    if (n > 0 && n < BLK) {
        fprintf(stderr, "tar: this does not look like a tar archive\n");
        return 2;
    }
    size_t off = 0;
    int zeros = 0;
    while (off + BLK <= n) {
        const struct ustar *h = (const struct ustar *)(d + off);
        /* Two consecutive zero blocks end the archive. */
        int allzero = 1;
        for (size_t i = 0; i < BLK; i++) if (d[off + i]) { allzero = 0; break; }
        if (allzero) { off += BLK; if (++zeros >= 2) break; continue; }
        zeros = 0;

        if (memcmp(h->magic, "ustar", 5) != 0) {
            fprintf(stderr, "tar: this does not look like a tar archive\n");
            return 2;
        }
        unsigned long stored = unoct(h->chksum, 8);
        if (stored != hdr_sum(h)) {
            fprintf(stderr, "tar: header checksum error\n");
            return 2;
        }

        char name[300];
        if (h->prefix[0]) snprintf(name, sizeof name, "%.155s/%.100s", h->prefix, h->name);
        else              snprintf(name, sizeof name, "%.100s", h->name);

        unsigned long long size = unoct(h->size, 12);
        size_t body = off + BLK;
        size_t padded = ((size_t)size + BLK - 1) / BLK * BLK;

        if (want_member(name, argc, argv, first)) {
            if (t_list) {
                if (v_verbose)
                    printf("%c %s %llu\n", h->typeflag == '5' ? 'd' : '-', name,
                           (unsigned long long)size);
                else
                    printf("%s\n", name);
            } else if (x_extract) {
                if (h->typeflag == '5') {
                    mkdirs(name);
                    mkdir(name, 0755);
                } else {
                    mkdirs(name);
                    FILE *o = fopen(name, "wb");
                    if (!o) {
                        fprintf(stderr, "tar: cannot create %s: %s\n",
                                name, strerror(errno));
                        status = 2;
                    } else {
                        if (size && body + size <= n)
                            fwrite(d + body, 1, (size_t)size, o);
                        fclose(o);
                    }
                }
                if (v_verbose) printf("%s\n", name);
            }
        }
        off = body + padded;
    }
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "usage: tar -c [-zjv] [-C dir] -f archive file...\n"
        "       tar -t [-zjv] -f archive\n"
        "       tar -x [-zjv] [-C dir] -f archive [member...]\n");
    exit(2);
}

int main(int argc, char **argv) {
    int first_file = argc;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'c': c_create = 1; break;
                case 'x': x_extract = 1; break;
                case 't': t_list = 1; break;
                case 'v': v_verbose = 1; break;
                case 'z': z_gzip = 1; break;
                case 'j': j_bzip2 = 1; break;
                case 'f': {
                    const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!val) usage();
                    f_archive = val;
                    p = ""; p--;
                    break;
                }
                case 'C': {
                    const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                    if (!val) usage();
                    C_dir = val;
                    p = ""; p--;
                    break;
                }
                case 'p': case 'm': case 'o': break;
                default:
                    fprintf(stderr, "tar: unknown option -- '%c'\n", *p);
                    usage();
                }
            }
            continue;
        }
        /* The first bare word is where the file list starts. GNU also
         * accepts a bundled first argument without a dash (`tar xzf`);
         * that arrives above because it starts with a letter only when a
         * dash was used, so the classic form is what is supported here. */
        first_file = i;
        break;
    }

    if (c_create + x_extract + t_list != 1) usage();
    if (C_dir && chdir(C_dir) != 0) {
        fprintf(stderr, "tar: cannot change to %s: %s\n", C_dir, strerror(errno));
        return 2;
    }

    if (c_create) {
        if (first_file >= argc) { fprintf(stderr, "tar: nothing to archive\n"); return 2; }
        struct buf b;
        memset(&b, 0, sizeof b);
        for (int i = first_file; i < argc; i++) add_path(&b, argv[i]);
        /* Two zero blocks close it, then pad to a 10240-byte record as
         * every tar has since tape drives. */
        buf_zero(&b, BLK * 2);
        while (b.len % 10240) if (!buf_zero(&b, BLK)) break;

        int rc;
        if (z_gzip) {
            uint8_t *o; size_t ol;
            if (gz_compress(b.d, b.len, 6, NULL, 0, &o, &ol) != 0) {
                fprintf(stderr, "tar: compression failed\n");
                free(b.d); return 2;
            }
            rc = write_out(o, ol);
            free(o);
        } else if (j_bzip2) {
            uint8_t *o; size_t ol; const char *why = "";
            if (bz_compress(b.d, b.len, 9, &o, &ol, &why) != 0) {
                fprintf(stderr, "tar: %s\n", why);
                free(b.d); return 2;
            }
            rc = write_out(o, ol);
            free(o);
        } else {
            rc = write_out(b.d, b.len);
        }
        free(b.d);
        return rc ? rc : status;
    }

    uint8_t *d = NULL;
    size_t n = 0;
    if (read_in(&d, &n) != 0) return 2;
    int rc = walk_archive(d, n, argc, argv, first_file);
    free(d);
    return rc ? rc : status;
}
