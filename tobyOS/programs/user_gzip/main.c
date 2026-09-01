/* gzip / gunzip / zcat -- one multi-call binary, dispatching on argv[0].
 *
 *     gzip   [-cdfklLqrtvV123456789] [file ...]
 *     gunzip [-cfklqrtv] [file ...]          (== gzip -d)
 *     zcat   [-fqv] [file ...]               (== gzip -dc)
 *
 * Nothing in this OS could read or write a .gz file from the shell before
 * this, which also made zgrep/zdiff/zless/gzexe impossible -- they are all
 * wrappers around this program.
 *
 * Neither half is a stub. Compression is a real fixed-Huffman DEFLATE
 * encoder (libtoby/src/deflate.c); decompression is src/puff.c, the same
 * inflater the browser has used for Content-Encoding: gzip, compiled a
 * second time for userland rather than copied. puff does NOT check the
 * trailer, so the CRC-32 and ISIZE are verified here -- that is what makes
 * `gzip -t` mean something rather than just "it inflated without crashing".
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
#include <tobyos/puff.h>

static const char *prog = "gzip";
static int dflag, cflag, fflag, kflag, tflag, lflag, qflag, vflag, rflag;
static int level = 6;
static int status;

static void warn(const char *fmt, const char *a) {
    if (qflag) return;
    fprintf(stderr, "%s: ", prog);
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
}

/* ---- whole-file I/O. These files live in RAM either way. ---- */

static int slurp_fd(int fd, uint8_t **out, size_t *outlen) {
    size_t cap = 65536, len = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) return -1;
    for (;;) {
        if (len == cap) {
            size_t nc = cap * 2;
            uint8_t *nb = (uint8_t *)realloc(b, nc);
            if (!nb) { free(b); return -1; }
            b = nb; cap = nc;
        }
        ssize_t n = read(fd, b + len, cap - len);
        if (n < 0) { free(b); return -1; }
        if (n == 0) break;
        len += (size_t)n;
    }
    *out = b; *outlen = len;
    return 0;
}

static int slurp(const char *path, uint8_t **out, size_t *outlen) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = slurp_fd(fd, out, outlen);
    close(fd);
    return rc;
}

static int spill(int fd, const uint8_t *b, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* ---- decompress, with the trailer actually checked ---- */

static int inflate_buf(const uint8_t *in, size_t inlen,
                       uint8_t **out, size_t *outlen, const char *label)
{
    if (inlen < 18 || in[0] != 0x1F || in[1] != 0x8B) {
        warn("%s: not in gzip format", label);
        return -1;
    }
    /* ISIZE is the original length modulo 2^32 -- exact for anything under
     * 4 GiB, and a starting guess otherwise. A short guess comes back as
     * PUFF_TRUNC, so grow and retry rather than trusting it blindly. */
    unsigned long cap = gz_isize(in, inlen);
    if (cap == 0) cap = (unsigned long)inlen * 8;
    if (cap < 4096) cap = 4096;

    for (int tries = 0; tries < 24; tries++) {
        uint8_t *buf = (uint8_t *)malloc(cap);
        if (!buf) { warn("%s: out of memory", label); return -1; }
        unsigned long got = cap;
        int pr = puff_gzip(buf, &got, in, (unsigned long)inlen);
        if (pr == PUFF_TRUNC) { free(buf); cap *= 4; continue; }
        if (pr != PUFF_OK) {
            free(buf);
            warn("%s: invalid compressed data--format violated", label);
            return -1;
        }
        /* puff stops at the deflate stream; the gzip trailer is ours to
         * check. Without this, corruption decodes to silent garbage. */
        uint32_t want_crc = 0, want_len = 0;
        const uint8_t *t = in + inlen - 8;
        want_crc = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                   ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
        want_len = (uint32_t)t[4] | ((uint32_t)t[5] << 8) |
                   ((uint32_t)t[6] << 16) | ((uint32_t)t[7] << 24);
        if ((uint32_t)(got & 0xFFFFFFFFu) != want_len) {
            free(buf);
            warn("%s: invalid compressed data--length error", label);
            return -1;
        }
        if (gz_crc32(buf, (size_t)got, 0) != want_crc) {
            free(buf);
            warn("%s: invalid compressed data--crc error", label);
            return -1;
        }
        *out = buf; *outlen = (size_t)got;
        return 0;
    }
    warn("%s: invalid compressed data--length error", label);
    return -1;
}

/* ---- name mapping ---- */

static int has_gz_suffix(const char *p) {
    size_t n = strlen(p);
    return (n > 3 && !strcmp(p + n - 3, ".gz")) ||
           (n > 5 && !strcmp(p + n - 5, ".gzip")) ||
           (n > 2 && !strcmp(p + n - 2, ".z"))   ||
           (n > 4 && !strcmp(p + n - 4, ".tgz"));
}

/* .tgz means .tar, everything else just loses its suffix. */
static void strip_suffix(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (n > 4 && !strcmp(in + n - 4, ".tgz")) {
        snprintf(out, cap, "%.*s.tar", (int)(n - 4), in);
        return;
    }
    size_t cut = 0;
    if (n > 3 && !strcmp(in + n - 3, ".gz"))        cut = 3;
    else if (n > 5 && !strcmp(in + n - 5, ".gzip")) cut = 5;
    else if (n > 2 && !strcmp(in + n - 2, ".z"))    cut = 2;
    snprintf(out, cap, "%.*s", (int)(n - cut), in);
}

/* ---- gzip -l ---- */

static int list_one(const char *path) {
    uint8_t *in; size_t inlen;
    if (slurp(path, &in, &inlen) != 0) { warn("%s: No such file or directory", path); status = 1; return -1; }
    if (inlen < 18 || in[0] != 0x1F || in[1] != 0x8B) {
        warn("%s: not in gzip format", path); free(in); status = 1; return -1;
    }
    uint32_t un = gz_isize(in, inlen);
    char name[512];
    strip_suffix(path, name, sizeof name);
    long ratio = un ? (long)(100 - (long long)inlen * 100 / (long long)un) : 0;
    printf("%10lu %10lu %5ld%% %s\n", (unsigned long)inlen,
           (unsigned long)un, ratio, name);
    free(in);
    return 0;
}

/* ---- the per-file worker ---- */

static int do_path(const char *path);

static int do_dir(const char *path) {
    DIR *d = opendir(path);
    if (!d) { warn("%s: cannot open directory", path); status = 1; return -1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[1024];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        do_path(sub);
    }
    closedir(d);
    return 0;
}

static int do_path(const char *path) {
    struct stat st;
    if (rflag && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return do_dir(path);

    uint8_t *in = NULL, *out = NULL;
    size_t inlen = 0, outlen = 0;

    if (slurp(path, &in, &inlen) != 0) {
        warn("%s: No such file or directory", path);
        status = 1;
        return -1;
    }

    char target[1024];
    if (dflag || tflag) {
        if (inflate_buf(in, inlen, &out, &outlen, path) != 0) {
            free(in); status = 1; return -1;
        }
        if (tflag) {
            if (vflag) printf("%s:\t OK\n", path);
            free(in); free(out);
            return 0;
        }
        strip_suffix(path, target, sizeof target);
        if (!strcmp(target, path)) {
            warn("%s: unknown suffix -- ignored", path);
            free(in); free(out); status = 1; return -1;
        }
    } else {
        if (has_gz_suffix(path) && !fflag) {
            warn("%s: already has .gz suffix -- unchanged", path);
            free(in); status = 1; return -1;
        }
        const char *base = path;
        for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
        if (gz_compress(in, inlen, level, base, 0, &out, &outlen) != 0) {
            warn("%s: compression failed", path);
            free(in); status = 1; return -1;
        }
        snprintf(target, sizeof target, "%s.gz", path);
    }

    if (cflag) {
        if (spill(1, out, outlen) != 0) { warn("%s: write error", path); status = 1; }
        free(in); free(out);
        return 0;
    }

    if (!fflag) {
        struct stat ts;
        if (stat(target, &ts) == 0) {
            warn("%s: already exists -- not overwritten", target);
            free(in); free(out); status = 1; return -1;
        }
    }
    int fd = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        warn("%s: cannot create", target);
        free(in); free(out); status = 1; return -1;
    }
    int wrc = spill(fd, out, outlen);
    close(fd);
    if (wrc != 0) {
        warn("%s: write error", target);
        free(in); free(out); status = 1; return -1;
    }

    if (vflag) {
        long ratio = 0;
        if (dflag) ratio = outlen ? (long)(100 - (long long)inlen * 100 / (long long)outlen) : 0;
        else       ratio = inlen  ? (long)(100 - (long long)outlen * 100 / (long long)inlen) : 0;
        printf("%s:\t%ld%% -- replaced with %s\n", path, ratio, target);
    }
    if (!kflag) unlink(path);

    free(in); free(out);
    return 0;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "gzip";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    if (!strcmp(base, "gunzip") || !strcmp(base, "uncompress")) dflag = 1;
    if (!strcmp(base, "zcat") || !strcmp(base, "gzcat")) { dflag = 1; cflag = 1; }

    int files[256], nfiles = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--stdout"))     { cflag = 1; continue; }
            if (!strcmp(a, "--decompress")) { dflag = 1; continue; }
            if (!strcmp(a, "--keep"))       { kflag = 1; continue; }
            if (!strcmp(a, "--force"))      { fflag = 1; continue; }
            if (!strcmp(a, "--test"))       { tflag = 1; dflag = 1; continue; }
            if (!strcmp(a, "--list"))       { lflag = 1; continue; }
            if (!strcmp(a, "--verbose"))    { vflag = 1; continue; }
            if (!strcmp(a, "--quiet"))      { qflag = 1; continue; }
            if (!strcmp(a, "--recursive"))  { rflag = 1; continue; }
            if (!strcmp(a, "--version")) {
                printf("%s (tobyOS) 1.0\n", prog);
                return 0;
            }
            if (!strcmp(a, "--help")) {
                printf("usage: %s [-cdfklLqrtvV123456789] [file ...]\n", prog);
                return 0;
            }
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'c': cflag = 1; break;
                case 'd': dflag = 1; break;
                case 'f': fflag = 1; break;
                case 'k': kflag = 1; break;
                case 'l': lflag = 1; break;
                case 'n': case 'N': break;   /* name/time handling: accepted */
                case 'q': qflag = 1; break;
                case 'r': rflag = 1; break;
                case 't': tflag = 1; dflag = 1; break;
                case 'v': vflag = 1; break;
                case 'V': printf("%s (tobyOS) 1.0\n", prog); return 0;
                case 'h': printf("usage: %s [-cdfklLqrtvV123456789]"
                                 " [file ...]\n", prog); return 0;
                case '1': case '2': case '3': case '4': case '5':
                case '6': case '7': case '8': case '9':
                    level = *p - '0'; break;
                default:
                    fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *p);
                    return 1;
                }
            }
            continue;
        }
        if (nfiles < (int)(sizeof files / sizeof files[0])) files[nfiles++] = i;
    }

    if (lflag) {
        if (nfiles == 0) { fprintf(stderr, "%s: -l needs a file\n", prog); return 1; }
        printf("%10s %10s %5s %s\n", "compressed", "uncompressed", "ratio",
               "uncompressed_name");
        for (int i = 0; i < nfiles; i++) list_one(argv[files[i]]);
        return status;
    }

    /* No files: filter stdin to stdout, which is what a pipeline wants. */
    if (nfiles == 0) {
        uint8_t *in = NULL, *out = NULL;
        size_t inlen = 0, outlen = 0;
        if (slurp_fd(0, &in, &inlen) != 0) {
            fprintf(stderr, "%s: read error\n", prog);
            return 1;
        }
        if (dflag) {
            if (inflate_buf(in, inlen, &out, &outlen, "stdin") != 0) {
                free(in); return 1;
            }
            if (tflag) { if (vflag) printf("stdin:\t OK\n"); free(in); free(out); return 0; }
        } else if (gz_compress(in, inlen, level, NULL, 0, &out, &outlen) != 0) {
            fprintf(stderr, "%s: compression failed\n", prog);
            free(in); return 1;
        }
        if (spill(1, out, outlen) != 0) status = 1;
        free(in); free(out);
        return status;
    }

    for (int i = 0; i < nfiles; i++) do_path(argv[files[i]]);
    return status;
}
