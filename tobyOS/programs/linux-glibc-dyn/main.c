/* B26 proof: a REAL glibc (Buildroot 2025.08, glibc 2.41) *DYNAMICALLY* linked
 * x86-64 Linux binary.  Where B25 proved the static glibc C-runtime, this proves
 * the full dynamic path:
 *
 *   - the program is a PIE with PT_INTERP=/lib/ld-linux-x86-64.so.2 and
 *     DT_NEEDED libc.so.6 / libm.so.6;
 *   - the kernel loads BOTH the PIE and the glibc ld.so, hands ld.so the auxv;
 *   - glibc's ld.so self-relocates, opens + file-backed-mmaps libc.so.6 from the
 *     VFS, relocates it, sets up TLS via the DTV, and resolves PLT entries
 *     lazily on first call;
 *   - then a normal glibc program runs on top.
 *
 * The last check (bit6) goes one step further: a *runtime* dlopen("libm.so.6")
 * + dlsym("cos"), which forces ld.so to open + map + relocate a fresh DSO long
 * after startup -- the hardest dynamic-loader path there is.
 *
 * Each check sets a bit in the exit status.  Core (startup) all-pass => 63;
 * with the runtime-dlopen bonus => 127.  Human-readable LXGLIBCDYN: lines let
 * the QEMU harness grep a verdict from the serial log.
 *
 * Built out-of-tree by programs/linux-glibc-dyn/build.sh against a downloaded
 * Bootlin glibc sysroot (the in-tree freestanding clang cannot supply glibc).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <dlfcn.h>
#include <math.h>

static volatile int g_thread_ran;

static void *worker(void *arg)
{
    errno = 0;
    g_thread_ran = 1;
    return (void *)(unsigned long)(0x1000 + (int)(long)arg);
}

int main(void)
{
    int bits = 0;

    /* bit0: stdio works (buffered printf through the SHARED glibc -> fd 1) */
    if (printf("LXGLIBCDYN: hello from a real DYNAMIC glibc binary\n") > 0)
        bits |= 1;

    /* bit1: heap (brk/mmap-backed malloc) + string ops */
    char *p = malloc(8192);
    if (p) {
        strcpy(p, "glibc-dyn-heap");
        if (strcmp(p, "glibc-dyn-heap") == 0 && strlen(p) == 14)
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

    /* bit4: getpid (auxv / glibc cached pid) */
    pid_t pid = getpid();
    if (pid > 0)
        bits |= 16;

    /* bit5: pthread create + join (clone, futex, TLS, robust list) -- in glibc
     * 2.34+ the pthread implementation is folded into libc.so.6, so this also
     * proves the shared libc's threading surface is fully relocated. */
    pthread_t th;
    void *ret = NULL;
    if (pthread_create(&th, NULL, worker, (void *)5) == 0 &&
        pthread_join(th, &ret) == 0 &&
        g_thread_ran == 1 &&
        ret == (void *)(unsigned long)0x1005)
        bits |= 32;

    /* bit6: RUNTIME dlopen + dlsym -- forces ld.so to open/map/relocate a fresh
     * DSO (libm.so.6) after startup, then call through the resolved pointer. */
    void *h = dlopen("libm.so.6", RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        double (*cosfn)(double) = (double (*)(double))dlsym(h, "cos");
        if (cosfn) {
            double c = cosfn(0.0);            /* cos(0) == 1.0 */
            if (c > 0.999 && c < 1.001)
                bits |= 64;
        }
        dlclose(h);
    } else {
        printf("LXGLIBCDYN: dlopen failed: %s\n", dlerror());
    }

    printf("LXGLIBCDYN: stdio=%d heap=%d sprintf=%d clock=%d pid=%d pthread=%d dlopen=%d\n",
           !!(bits & 1), !!(bits & 2), !!(bits & 4), !!(bits & 8),
           !!(bits & 16), !!(bits & 32), !!(bits & 64));
    printf("LXGLIBCDYN: VERDICT bits=%d (63=core, 127=core+dlopen)\n", bits);
    fflush(stdout);
    return bits;
}
