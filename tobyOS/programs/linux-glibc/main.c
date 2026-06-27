/* B25 proof: a REAL glibc (Buildroot 2025.08, glibc 2.x) statically-linked
 * x86-64 Linux binary -- NOT freestanding, NOT musl.  It exercises the glibc
 * C runtime startup (TLS via arch_prctl, __libc_start_main, rseq/robust-list
 * registration, brk-backed malloc) and a spread of libc surface, then a
 * pthread (clone + futex + TLS) so the kernel's Linux ABI is proven against
 * the most demanding mainstream libc.
 *
 * Each check sets a bit in the exit status.  All-pass => exit 63.  The program
 * also prints human-readable LXGLIBC: lines so the QEMU harness can grep a
 * verdict from the serial log.
 *
 * Built out-of-tree by logs/b25.sh against a downloaded Bootlin glibc sysroot
 * (the in-tree freestanding clang cannot supply glibc headers/libs).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

static volatile int g_thread_ran;

static void *worker(void *arg)
{
    /* touches errno (TLS) + returns a value the joiner verifies */
    errno = 0;
    g_thread_ran = 1;
    return (void *)(unsigned long)(0x1000 + (int)(long)arg);
}

int main(void)
{
    int bits = 0;

    /* bit0: stdio works (buffered printf through glibc -> fd 1) */
    if (printf("LXGLIBC: hello from a real glibc static binary\n") > 0)
        bits |= 1;

    /* bit1: heap (brk/mmap-backed malloc) + string ops */
    char *p = malloc(8192);
    if (p) {
        strcpy(p, "glibc-heap");
        if (strcmp(p, "glibc-heap") == 0 && strlen(p) == 10)
            bits |= 2;
        free(p);
    }

    /* bit2: snprintf with floating point (glibc's printf float path) */
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%.3f|%d|%s", 3.14159, 42, "ok");
    if (n > 0 && strcmp(buf, "3.142|42|ok") == 0)
        bits |= 4;

    /* bit3: clock/time syscalls via glibc wrappers */
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0 &&
        (ts.tv_sec != 0 || ts.tv_nsec != 0))
        bits |= 8;

    /* bit4: environment + getpid (auxv / glibc cached pid) */
    pid_t pid = getpid();
    if (pid > 0)
        bits |= 16;

    /* bit5: pthread create + join (clone, futex, TLS, robust list) */
    pthread_t th;
    void *ret = NULL;
    if (pthread_create(&th, NULL, worker, (void *)5) == 0 &&
        pthread_join(th, &ret) == 0 &&
        g_thread_ran == 1 &&
        ret == (void *)(unsigned long)0x1005)
        bits |= 32;

    printf("LXGLIBC: stdio=%d heap=%d sprintf=%d clock=%d pid=%d pthread=%d\n",
           !!(bits & 1), !!(bits & 2), !!(bits & 4),
           !!(bits & 8), !!(bits & 16), !!(bits & 32));
    printf("LXGLIBC: VERDICT bits=%d (63=all)\n", bits);
    fflush(stdout);
    return bits;
}
