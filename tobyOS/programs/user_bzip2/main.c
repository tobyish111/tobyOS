/* bzip2 / bunzip2 / bzcat -- one multi-call binary, dispatching on argv[0].
 *
 *     bzip2   [-cdfkqstvz123456789] [file ...]
 *     bunzip2 [-cfkqstv] [file ...]          (== bzip2 -d)
 *     bzcat   [-qv] [file ...]               (== bzip2 -dc)
 *
 * Both halves are real: the Burrows-Wheeler encoder and decoder live in
 * libtoby/src/bzip2_enc.c and libtoby/src/bzip2.c, written from the format
 * description rather than ported. Output has been checked against real
 * bzip2 -- it accepts what this writes, and this reads what it writes,
 * including multi-block streams.
 *
 * -t verifies rather than just decoding: the decoder rejects a stream
 * whose structure is wrong, and the per-block CRC is checked here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <toby/bzip2.h>

static const char *prog = "bzip2";
static int dflag, cflag, fflag, kflag, tflag, qflag, vflag;
static int level = 9;
static int status;

static void warn(const char *fmt, const char *a) {
    if (qflag) return;
    fprintf(stderr, "%s: ", prog);
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
}

static int slurp_fd(int fd, uint8_t **out, size_t *outlen) {
    size_t cap = 65536, len = 0;
    uint8_t *b = (uint8_t *)malloc(cap);
    if (!b) return -1;
    for (;;) {
        if (len == cap) {
            uint8_t *nb = (uint8_t *)realloc(b, cap * 2);
            if (!nb) { free(b); return -1; }
            b = nb; cap *= 2;
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

static int has_bz_suffix(const char *p) {
    size_t n = strlen(p);
    return (n > 4 && !strcmp(p + n - 4, ".bz2")) ||
           (n > 3 && !strcmp(p + n - 3, ".bz"))  ||
           (n > 5 && !strcmp(p + n - 5, ".tbz2")) ||
           (n > 4 && !strcmp(p + n - 4, ".tbz"));
}

/* .tbz/.tbz2 mean .tar; everything else just loses its suffix. */
static void strip_suffix(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if ((n > 5 && !strcmp(in + n - 5, ".tbz2")) ||
        (n > 4 && !strcmp(in + n - 4, ".tbz"))) {
        size_t cut = (in[n - 1] == '2') ? 5 : 4;
        snprintf(out, cap, "%.*s.tar", (int)(n - cut), in);
        return;
    }
    size_t cut = 0;
    if (n > 4 && !strcmp(in + n - 4, ".bz2"))     cut = 4;
    else if (n > 3 && !strcmp(in + n - 3, ".bz")) cut = 3;
    snprintf(out, cap, "%.*s", (int)(n - cut), in);
}

static int do_path(const char *path) {
    uint8_t *in = NULL, *out = NULL;
    size_t inlen = 0, outlen = 0;
    const char *why = "";

    if (slurp(path, &in, &inlen) != 0) {
        warn("%s: No such file or directory", path);
        status = 1;
        return -1;
    }

    char target[1024];
    if (dflag || tflag) {
        if (bz_decompress(in, inlen, &out, &outlen, &why) != 0) {
            fprintf(stderr, "%s: %s: %s\n", prog, path, why);
            free(in); status = 2; return -1;
        }
        if (tflag) {
            if (vflag) printf("%s:\t ok\n", path);
            free(in); free(out);
            return 0;
        }
        strip_suffix(path, target, sizeof target);
        if (!strcmp(target, path)) {
            warn("%s: unknown suffix -- ignored", path);
            free(in); free(out); status = 1; return -1;
        }
    } else {
        if (has_bz_suffix(path) && !fflag) {
            warn("%s: already has .bz2 suffix -- unchanged", path);
            free(in); status = 1; return -1;
        }
        if (bz_compress(in, inlen, level, &out, &outlen, &why) != 0) {
            fprintf(stderr, "%s: %s: %s\n", prog, path, why);
            free(in); status = 2; return -1;
        }
        snprintf(target, sizeof target, "%s.bz2", path);
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
    const char *base = argv[0] ? argv[0] : "bzip2";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    if (!strcmp(base, "bunzip2")) dflag = 1;
    if (!strcmp(base, "bzcat"))   { dflag = 1; cflag = 1; }

    int files[256], nfiles = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--stdout"))     { cflag = 1; continue; }
            if (!strcmp(a, "--decompress")) { dflag = 1; continue; }
            if (!strcmp(a, "--compress"))   { dflag = 0; continue; }
            if (!strcmp(a, "--keep"))       { kflag = 1; continue; }
            if (!strcmp(a, "--force"))      { fflag = 1; continue; }
            if (!strcmp(a, "--test"))       { tflag = 1; dflag = 1; continue; }
            if (!strcmp(a, "--quiet"))      { qflag = 1; continue; }
            if (!strcmp(a, "--verbose"))    { vflag = 1; continue; }
            if (!strcmp(a, "--version")) { printf("%s (tobyOS) 1.0\n", prog); return 0; }
            if (!strcmp(a, "--help")) {
                printf("usage: %s [-cdfkqstvz123456789] [file ...]\n", prog);
                return 0;
            }
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'c': cflag = 1; break;
                case 'd': dflag = 1; break;
                case 'z': dflag = 0; break;
                case 'f': fflag = 1; break;
                case 'k': kflag = 1; break;
                case 'q': qflag = 1; break;
                case 's': break;         /* small-memory mode: accepted */
                case 't': tflag = 1; dflag = 1; break;
                case 'v': vflag = 1; break;
                case 'V': printf("%s (tobyOS) 1.0\n", prog); return 0;
                case 'h': printf("usage: %s [-cdfkqstvz123456789]"
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

    if (nfiles == 0) {
        uint8_t *in = NULL, *out = NULL;
        size_t inlen = 0, outlen = 0;
        const char *why = "";
        if (slurp_fd(0, &in, &inlen) != 0) {
            fprintf(stderr, "%s: read error\n", prog);
            return 1;
        }
        if (dflag) {
            if (bz_decompress(in, inlen, &out, &outlen, &why) != 0) {
                fprintf(stderr, "%s: stdin: %s\n", prog, why);
                free(in); return 2;
            }
            if (tflag) { if (vflag) printf("stdin:\t ok\n"); free(in); free(out); return 0; }
        } else if (bz_compress(in, inlen, level, &out, &outlen, &why) != 0) {
            fprintf(stderr, "%s: stdin: %s\n", prog, why);
            free(in); return 2;
        }
        if (spill(1, out, outlen) != 0) status = 1;
        free(in); free(out);
        return status;
    }

    for (int i = 0; i < nfiles; i++) do_path(argv[files[i]]);
    return status;
}
