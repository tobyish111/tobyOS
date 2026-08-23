/* linux-ldso -- /etc/ld.so.cache ends the LD_LIBRARY_PATH era
 * (Phase G, 2026-08-22).
 *
 *   bit0  /etc/ld.so.cache exists, carries the modern magic
 *         ("glibc-ld.so.cache" + "1.1"), a plausible entry count, and
 *         every entry's string offsets land inside the file
 *   bit1  a REAL glibc-dynamic PIE (/bin/linux-glibc-dyn: PT_INTERP
 *         ld-linux, DT_NEEDED libc/libm, runtime dlopen) starts and
 *         passes WITHOUT LD_LIBRARY_PATH anywhere in its environment --
 *         ld.so resolved every DSO through the cache alone
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: structural sanity of the generated cache ---- */
    {
        int ok = 0;
        int fd = open("/etc/ld.so.cache", O_RDONLY);
        if (fd >= 0) {
            static uint8_t buf[65536];
            ssize_t n = read(fd, buf, sizeof buf);
            close(fd);
            if (n > 48 && memcmp(buf, "glibc-ld.so.cache", 17) == 0 &&
                memcmp(buf + 17, "1.1", 3) == 0) {
                uint32_t nlibs, lenstr;
                memcpy(&nlibs, buf + 20, 4);
                memcpy(&lenstr, buf + 24, 4);
                size_t want = 48 + (size_t)nlibs * 24 + lenstr;
                int sane = (nlibs > 0 && nlibs < 4096 &&
                            (size_t)n == want);
                int offs_ok = 1;
                for (uint32_t i = 0; i < nlibs && offs_ok; i++) {
                    uint32_t k, v;
                    memcpy(&k, buf + 48 + i * 24 + 4, 4);
                    memcpy(&v, buf + 48 + i * 24 + 8, 4);
                    if (k >= (uint32_t)n || v >= (uint32_t)n) offs_ok = 0;
                }
                printf("ldso: cache nlibs=%u size=%zd sane=%d offs=%d\n",
                       nlibs, n, sane, offs_ok);
                ok = sane && offs_ok;
            } else {
                printf("ldso: cache bad header (n=%zd)\n", n);
            }
        } else {
            printf("ldso: no /etc/ld.so.cache\n");
        }
        if (ok) bits |= 1;
    }

    /* ---- bit1: dynamic glibc binary with NO LD_LIBRARY_PATH ---- */
    {
        int ok = 0;
        pid_t k = fork();
        if (k == 0) {
            char *kargv[] = { (char *)"/bin/linux-glibc-dyn", NULL };
            char *kenvp[] = { (char *)"PATH=/bin", NULL };   /* no LD_* */
            execve("/bin/linux-glibc-dyn", kargv, kenvp);
            _exit(9);
        }
        int st = 0;
        if (k > 0 && waitpid(k, &st, 0) == k && WIFEXITED(st)) {
            /* Its exit code is a bitmask: 63 = all core dynamic-loader
             * checks, 127 = plus the runtime-dlopen bonus. */
            int code = WEXITSTATUS(st);
            printf("ldso: dyn exit=%d (want core 63 bits set)\n", code);
            ok = ((code & 63) == 63);
        }
        if (ok) bits |= 2;
    }

    printf("LXLDSO: VERDICT bits=%d (3=all)\n", bits);
    return bits;
}
