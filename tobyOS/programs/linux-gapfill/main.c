/* linux-gapfill -- the surface added after Phase 3 closed.
 *
 * ===========================================================================
 * WHAT THIS COVERS, AND WHY THESE
 *
 * A census over the dispatcher (not a grep -- see below) said these were the
 * gaps worth closing first: /proc/PID/fdinfo, mkdirat, getcpu, mlock2,
 * fadvise64, openat2. fdinfo leads because it is the live Chromium sandbox
 * blocker: ChrootToSafeEmptyDir() chroots into /proc/self/fdinfo/ precisely
 * because that directory contains exactly the caller's own descriptors, so
 * after closing them it is guaranteed empty.
 *
 * THE MEASUREMENT ITSELF WAS WRONG TWICE, which is why the census now lives in
 * a script rather than a grep. `case N:` followed by `{` was invisible to the
 * first pattern (making clock_nanosleep/memfd_create/sigaltstack look absent);
 * relaxing the pattern then matched lx_scname's ~90-entry NAME TABLE as though
 * those were handlers. Neither number was real. Anything claiming a syscall is
 * missing has to be checked against the dispatcher's own body.
 *
 * ===========================================================================
 * WHAT EACH BIT ASSERTS
 *
 * bit0 /proc/self/fdinfo is a DIRECTORY whose listing TRACKS the fd table --
 *      two-sided: a descriptor we open must APPEAR and, once closed, must be
 *      GONE. "The directory exists" is not the claim; Chromium's use depends
 *      on it emptying out when it closes things.
 * bit1 fdinfo/<n> reports a real position: `pos:` must ADVANCE by exactly the
 *      number of bytes read. A constant 0 would satisfy any check that only
 *      parsed the file.
 * bit2 chroot("/proc/self/fdinfo/") succeeds -- literally the call Chromium
 *      makes, run in a child because it is irreversible.
 * bit3 mkdirat(AT_FDCWD) creates a REAL directory (stat says DIR), and a
 *      dirfd-relative call is REFUSED rather than silently resolved against
 *      the cwd -- creating a directory in the wrong place is worse than an
 *      error, so the refusal is the feature.
 * bit4 getcpu writes BOTH outputs and the cpu id is < the online count.
 * bit5 mlock2 and fadvise64 return 0 (both are honestly no-ops here: nothing
 *      pages out, and fadvise is a hint the spec allows ignoring).
 * bit6 openat2 with resolve=0 opens a file that then actually READS.
 * bit7 openat2 with a RESOLVE_* bit is REFUSED with EINVAL. This is the bit
 *      that matters: those flags are path-resolution RESTRICTIONS, so honouring
 *      the open while dropping them would hand back a sandbox that does not
 *      exist. The errno is the assertion, not the failure.
 *
 * exit status is the bitmask; 255 = all eight.
 * ===========================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#define RESOLVE_NO_SYMLINKS 0x04ULL
/* This sysroot's glibc predates openat2, so SYS_openat2 is not in its headers.
 * The number is the ABI, not the header -- x86-64 openat2 is 437. */
#ifndef SYS_openat2
#define SYS_openat2 437
#endif

struct lx_open_how { unsigned long long flags, mode, resolve; };

static int g_bits;
__attribute__((format(printf, 2, 3)))
static void ok(int bit, const char *fmt, ...)
{
    va_list ap; g_bits |= bit;
    printf("gapfill:   ok   ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}
__attribute__((format(printf, 1, 2)))
static void no(const char *fmt, ...)
{
    va_list ap;
    printf("gapfill:   FAIL ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

/* Is `name` present in /proc/self/fdinfo? */
static int fdinfo_has(const char *name, int *count)
{
    DIR *d = opendir("/proc/self/fdinfo");
    if (!d) return -1;
    int found = 0, n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        n++;
        if (strcmp(e->d_name, name) == 0) found = 1;
    }
    closedir(d);
    if (count) *count = n;
    return found;
}

/* ---- bit0: the listing tracks the fd table, both ways ------------------- */
static void check_fdinfo_dir(void)
{
    struct stat st;
    if (stat("/proc/self/fdinfo", &st) != 0) {
        no("fdinfo: stat errno=%d -- the directory does not exist", errno);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
        no("fdinfo: exists but is not a directory (mode=0%o)",
           (unsigned)st.st_mode);
        return;
    }
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0) { no("fdinfo: could not open anything to test with (errno=%d)",
                     errno); return; }
    /* THE READER PERTURBS WHAT IT MEASURES. The first version tested the fd
     * that open() handed back -- typically 3 -- and reported "still listed
     * after close()". It was: fdinfo_has() calls opendir(), which takes the
     * LOWEST free descriptor, which after the close is the number just freed.
     * The listing was correct, and so live that it showed the reader's own
     * handle. Move the descriptor somewhere opendir will not land while low
     * numbers are free, so the observation cannot create what it observes. */
    const int HIGH = 200;
    if (dup2(fd, HIGH) != HIGH) {
        no("fdinfo: dup2 to %d errno=%d", HIGH, errno);
        close(fd); return;
    }
    close(fd);
    fd = HIGH;
    char nm[16];
    snprintf(nm, sizeof nm, "%d", fd);

    int before = 0;
    int present = fdinfo_has(nm, &before);
    if (present <= 0) {
        no("fdinfo: fd %d is open but does NOT appear in the listing "
           "(%d entries)", fd, before);
        close(fd);
        return;
    }
    close(fd);
    int after = 0;
    int still = fdinfo_has(nm, &after);
    if (still != 0) {
        no("fdinfo: fd %d still listed after close() -- the listing is not "
           "the live fd table", fd);
        return;
    }
    ok(1, "fdinfo: a directory that TRACKS the fd table (fd %d appeared among "
          "%d entries, gone among %d after close)", fd, before, after);
}

/* ---- bit1: pos is real ------------------------------------------------- */
static long fdinfo_pos(int fd)
{
    char path[64], buf[256];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", fd);
    int f = open(path, O_RDONLY);
    if (f < 0) return -1;
    ssize_t n = read(f, buf, sizeof buf - 1);
    close(f);
    if (n <= 0) return -1;
    buf[n] = 0;
    char *p = strstr(buf, "pos:");
    if (!p) return -2;
    return strtol(p + 4, NULL, 10);
}

static void check_fdinfo_pos(void)
{
    int fd = open("/proc/uptime", O_RDONLY);
    if (fd < 0) fd = open("/etc/hostname", O_RDONLY);
    if (fd < 0) { no("fdinfo pos: nothing to open (errno=%d)", errno); return; }
    long p0 = fdinfo_pos(fd);
    char tmp[32];
    ssize_t got = read(fd, tmp, sizeof tmp);
    long p1 = fdinfo_pos(fd);
    close(fd);
    if (p0 < 0 || p1 < 0) {
        no("fdinfo pos: could not read the field (%ld -> %ld)", p0, p1);
        return;
    }
    if (got <= 0) { no("fdinfo pos: the test read returned %ld", (long)got);
                    return; }
    if (p1 - p0 != (long)got) {
        no("fdinfo pos: read %ld bytes but pos moved %ld (%ld -> %ld) -- the "
           "field is not tracking the descriptor", (long)got, p1 - p0, p0, p1);
        return;
    }
    ok(2, "fdinfo pos: advanced by exactly the %ld bytes read (%ld -> %ld)",
       (long)got, p0, p1);
}

/* ---- bit2: Chromium's actual call --------------------------------------- */
static void check_chroot_fdinfo(void)
{
    pid_t p = fork();
    if (p < 0) { no("chroot: fork errno=%d", errno); return; }
    if (p == 0) {
        /* Exactly what sandbox/linux/services/credentials.cc does. */
        if (chroot("/proc/self/fdinfo/") != 0) _exit(errno ? errno : 1);
        if (chdir("/") != 0) _exit(90);
        _exit(42);
    }
    int st = 0;
    if (waitpid(p, &st, 0) != p) { no("chroot: waitpid errno=%d", errno);
                                   return; }
    if (!WIFEXITED(st)) { no("chroot: child died on signal %d", WTERMSIG(st));
                          return; }
    int c = WEXITSTATUS(st);
    if (c == 90) { no("chroot: chroot succeeded but chdir(/) then failed"); return; }
    if (c != 42) {
        no("chroot(\"/proc/self/fdinfo/\") failed, errno=%d -- this is the "
           "Chromium sandbox call at credentials.cc", c);
        return;
    }
    ok(4, "chroot(\"/proc/self/fdinfo/\") + chdir(/) succeed -- the "
          "ChrootToSafeEmptyDir path");
}

/* ---- bit3: mkdirat ------------------------------------------------------ */
static void check_mkdirat(void)
{
    const char *dir = "/data/gapfill.d";
    rmdir(dir);
    if (syscall(SYS_mkdirat, AT_FDCWD, dir, 0700) != 0) {
        no("mkdirat(AT_FDCWD, %s) errno=%d", dir, errno);
        return;
    }
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        no("mkdirat returned 0 but %s is not a directory", dir);
        return;
    }
    /* The refusal is the other half: a dirfd-relative call must NOT quietly
     * resolve against the cwd and create the directory somewhere else. */
    int dfd = open("/data", O_RDONLY);
    long rc = (dfd >= 0)
        ? syscall(SYS_mkdirat, dfd, "gapfill.rel", 0700) : -1;
    int e = errno;
    if (dfd >= 0) close(dfd);
    rmdir(dir);
    if (dfd >= 0 && rc == 0) {
        rmdir("/data/gapfill.rel");
        no("mkdirat: a dirfd-relative call SUCCEEDED -- it was resolved "
           "against something, and this kernel cannot express which");
        return;
    }
    ok(8, "mkdirat: AT_FDCWD creates a real directory; dirfd-relative is "
          "refused (errno=%d) rather than guessed", e);
}

/* ---- bit4: getcpu ------------------------------------------------------- */
static void check_getcpu(void)
{
    unsigned cpu = 0xdeadbeef, node = 0xdeadbeef;
    if (syscall(SYS_getcpu, &cpu, &node, NULL) != 0) {
        no("getcpu errno=%d", errno);
        return;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu == 0xdeadbeef || node == 0xdeadbeef) {
        no("getcpu returned 0 but did not write both outputs "
           "(cpu=0x%x node=0x%x)", cpu, node);
        return;
    }
    if (n > 0 && (long)cpu >= n) {
        no("getcpu says cpu=%u but only %ld are online", cpu, n);
        return;
    }
    ok(16, "getcpu: cpu=%u node=%u, both written, cpu < %ld online",
       cpu, node, n);
}

/* ---- bit5: the honest no-ops -------------------------------------------- */
static void check_hints(void)
{
    void *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { no("hints: mmap errno=%d", errno); return; }
    long r1 = syscall(SYS_mlock2, m, 8192, 0);
    int e1 = errno;
    int fd = open("/proc/uptime", O_RDONLY);
    long r2 = (fd >= 0) ? syscall(SYS_fadvise64, fd, (long)0, (long)0, 0) : -1;
    int e2 = errno;
    if (fd >= 0) close(fd);
    munmap(m, 8192);
    if (r1 != 0) { no("mlock2 returned %ld errno=%d", r1, e1); return; }
    if (r2 != 0) { no("fadvise64 returned %ld errno=%d", r2, e2); return; }
    ok(32, "mlock2 and fadvise64 both succeed (both are true no-ops here: "
           "nothing pages out, and fadvise is a hint)");
}

/* ---- bits 6 and 7: openat2 ---------------------------------------------- */
static void check_openat2(void)
{
    struct lx_open_how how;
    memset(&how, 0, sizeof how);
    how.flags = O_RDONLY;

    long fd = syscall(SYS_openat2, AT_FDCWD, "/proc/uptime", &how, sizeof how);
    if (fd < 0) {
        no("openat2(resolve=0) errno=%d", errno);
    } else {
        char b[64];
        ssize_t n = read((int)fd, b, sizeof b);
        close((int)fd);
        if (n <= 0) no("openat2: opened fd %ld but it read %ld bytes",
                       fd, (long)n);
        else ok(64, "openat2(resolve=0): opened and read %ld bytes", (long)n);
    }

    /* THE bit. A RESOLVE_* flag is a restriction; opening anyway would be a
     * sandbox that is not there. */
    memset(&how, 0, sizeof how);
    how.flags = O_RDONLY;
    how.resolve = RESOLVE_NO_SYMLINKS;
    long fd2 = syscall(SYS_openat2, AT_FDCWD, "/proc/uptime", &how, sizeof how);
    int e = errno;
    if (fd2 >= 0) {
        close((int)fd2);
        no("openat2: RESOLVE_NO_SYMLINKS was ACCEPTED and the open succeeded "
           "-- the restriction was silently dropped");
        return;
    }
    if (e != EINVAL) {
        no("openat2: RESOLVE_NO_SYMLINKS refused with errno=%d, want EINVAL "
           "(%d)", e, EINVAL);
        return;
    }
    ok(128, "openat2: an unenforceable RESOLVE_* bit is refused with EINVAL, "
            "not silently ignored");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("gapfill: start pid=%d uid=%u\n", (int)getpid(), (unsigned)getuid());
    check_fdinfo_dir();
    check_fdinfo_pos();
    check_chroot_fdinfo();
    check_mkdirat();
    check_getcpu();
    check_hints();
    check_openat2();
    printf("gapfill: BITS=0x%x (want 0xff)\n", g_bits);
    return g_bits;
}
