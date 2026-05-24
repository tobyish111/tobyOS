/* user_wget/main.c -- HTTP download tool for tobyOS.
 *
 * Usage: wget <url> [-o outfile]
 *
 * Fetches a URL via the kernel's SYS_HTTP_GET syscall (which handles
 * DNS, TCP, and HTTP internally) and writes the response body to a
 * file or stdout. Maximum download size is 64 KB (kernel limit).
 */

#include <stdio.h>
#include <string.h>

static inline long sys_http_get(const char *url, void *buf,
                                unsigned int buf_sz) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "0"(79L), "D"(url), "S"(buf), "d"((long)buf_sz)
        : "rcx", "r11", "memory");
    return r;
}

#define MAX_BUF 65536

static char dl_buf[MAX_BUF];

static const char *err_str(long code) {
    switch ((int)code) {
    case -2:  return "host not found (ENOENT)";
    case -5:  return "I/O / network error (EIO)";
    case -12: return "out of memory (ENOMEM)";
    case -14: return "bad buffer address (EFAULT)";
    case -22: return "invalid URL or argument (EINVAL)";
    case -38: return "HTTP not implemented (ENOSYS)";
    default:  return "unknown error";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: wget <url> [-o outfile]\n"
            "\n"
            "  Downloads <url> via the kernel HTTP stack.\n"
            "  -o <file>   write output to <file> instead of stdout\n"
            "\n"
            "  Max download size: 64 KB.\n");
        return 1;
    }

    const char *url = argv[1];
    const char *outfile = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "wget: -o requires a filename\n");
                return 1;
            }
            outfile = argv[++i];
        } else {
            fprintf(stderr, "wget: unknown option '%s'\n", argv[i]);
            return 1;
        }
    }

    fprintf(stderr, "Downloading %s...\n", url);

    long n = sys_http_get(url, dl_buf, MAX_BUF);
    if (n < 0) {
        fprintf(stderr, "wget: download failed: %s (code %ld)\n",
                err_str(n), n);
        return 1;
    }

    fprintf(stderr, "Downloading... %ld bytes received\n", n);

    if (outfile) {
        FILE *f = fopen(outfile, "w");
        if (!f) {
            fprintf(stderr, "wget: cannot open '%s' for writing\n", outfile);
            return 1;
        }
        size_t written = fwrite(dl_buf, 1, (size_t)n, f);
        fclose(f);
        if ((long)written != n) {
            fprintf(stderr, "wget: short write (%zu of %ld bytes)\n",
                    written, n);
            return 1;
        }
        fprintf(stderr, "Saved to %s (%ld bytes)\n", outfile, n);
    } else {
        fwrite(dl_buf, 1, (size_t)n, stdout);
    }

    return 0;
}
