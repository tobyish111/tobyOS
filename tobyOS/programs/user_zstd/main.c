/* zstd / unzstd / zstdcat -- one multi-call binary, on argv[0].
 *
 *     zstd    [-cdfkqtv123456789] [file ...]
 *     unzstd  [-cfkqtv] [file ...]          (== zstd -d)
 *     zstdcat [-qv] [file ...]              (== zstd -dc)
 *
 * .zst is what modern distros ship packages and kernels in, so this was
 * the last format that actually blocked reading real data here.
 *
 * DECOMPRESSION IS COMPLETE and checked against real zstd 1.5.7 across
 * levels 1 to 19. COMPRESSION EMITS RAW BLOCKS: a valid .zst frame any
 * zstd decodes, but the bytes are not compressed. That is said on stderr
 * every time rather than left to be discovered from the file size --
 * and gzip and bzip2 in this tree are both real, so anything that wants
 * a smaller file has two working choices.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <toby/zstd.h>

static const char *prog = "zstd";
static int dflag, cflag, fflag, kflag, tflag, qflag, vflag;
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

static int has_zst_suffix(const char *p) {
    size_t n = strlen(p);
    return (n > 4 && !strcmp(p + n - 4, ".zst")) ||
           (n > 4 && !strcmp(p + n - 4, ".tzst"));
}

static void strip_suffix(const char *in, char *out, size_t cap) {
    size_t n = strlen(in);
    if (n > 5 && !strcmp(in + n - 5, ".tzst")) {
        snprintf(out, cap, "%.*s.tar", (int)(n - 5), in);
        return;
    }
    size_t cut = (n > 4 && !strcmp(in + n - 4, ".zst")) ? 4 : 0;
    snprintf(out, cap, "%.*s", (int)(n - cut), in);
}

/* Said once per run, not once per file. */
static int warned_raw;
static void warn_raw_once(void) {
    if (qflag || warned_raw) return;
    warned_raw = 1;
    fprintf(stderr, "%s: compressing with RAW blocks -- the output is a valid "
                    ".zst that any zstd reads, but it is not smaller. Use gzip "
                    "or bzip2 if you want compression.\n", prog);
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
        if (zstd_decompress(in, inlen, &out, &outlen, &why) != 0) {
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
        if (has_zst_suffix(path) && !fflag) {
            warn("%s: already has .zst suffix -- unchanged", path);
            free(in); status = 1; return -1;
        }
        warn_raw_once();
        if (zstd_compress(in, inlen, &out, &outlen, &why) != 0) {
            fprintf(stderr, "%s: %s: %s\n", prog, path, why);
            free(in); status = 2; return -1;
        }
        snprintf(target, sizeof target, "%s.zst", path);
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
    if (wrc != 0) { warn("%s: write error", target); free(in); free(out); status = 1; return -1; }
    if (vflag) printf("%s:\t%zu -> %zu bytes\n", path, inlen, outlen);
    if (!kflag) unlink(path);
    free(in); free(out);
    return 0;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "zstd";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    if (!strcmp(base, "unzstd"))  dflag = 1;
    if (!strcmp(base, "zstdcat")) { dflag = 1; cflag = 1; }

    int files[256], nfiles = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--stdout"))     { cflag = 1; continue; }
            if (!strcmp(a, "--decompress")) { dflag = 1; continue; }
            if (!strcmp(a, "--keep"))       { kflag = 1; continue; }
            if (!strcmp(a, "--force"))      { fflag = 1; continue; }
            if (!strcmp(a, "--test"))       { tflag = 1; dflag = 1; continue; }
            if (!strcmp(a, "--quiet"))      { qflag = 1; continue; }
            if (!strcmp(a, "--verbose"))    { vflag = 1; continue; }
            if (!strcmp(a, "--version")) { printf("%s (tobyOS) 1.0\n", prog); return 0; }
            if (!strcmp(a, "--help")) {
                printf("usage: %s [-cdfkqtv123456789] [file ...]\n", prog);
                return 0;
            }
            if (!strncmp(a, "--ultra", 7) || !strncmp(a, "--long", 6)) continue;
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'c': cflag = 1; break;
                case 'd': dflag = 1; break;
                case 'z': dflag = 0; break;
                case 'f': fflag = 1; break;
                case 'k': kflag = 1; break;
                case 'q': qflag = 1; break;
                case 't': tflag = 1; dflag = 1; break;
                case 'v': vflag = 1; break;
                case 'T': break;             /* thread count: single-threaded */
                case 'V': printf("%s (tobyOS) 1.0\n", prog); return 0;
                case 'h': printf("usage: %s [-cdfkqtv123456789]"
                                 " [file ...]\n", prog); return 0;
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    /* Accepted and ignored: raw blocks have no levels. */
                    break;
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
            if (zstd_decompress(in, inlen, &out, &outlen, &why) != 0) {
                fprintf(stderr, "%s: stdin: %s\n", prog, why);
                free(in); return 2;
            }
            if (tflag) { if (vflag) printf("stdin:\t ok\n"); free(in); free(out); return 0; }
        } else {
            warn_raw_once();
            if (zstd_compress(in, inlen, &out, &outlen, &why) != 0) {
                fprintf(stderr, "%s: stdin: %s\n", prog, why);
                free(in); return 2;
            }
        }
        if (spill(1, out, outlen) != 0) status = 1;
        free(in); free(out);
        return status;
    }

    for (int i = 0; i < nfiles; i++) do_path(argv[files[i]]);
    return status;
}
