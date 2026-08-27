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
 * bit8 fchdir(2) actually MOVES the cwd (getcwd confirms) and REFUSES a
 *      non-directory fd with ENOTDIR. It was missing from the kernel and from
 *      libtoby entirely, so every openat(dirfd)+fchdir traversal got -ENOSYS.
 *
 * bit9 pause() is NOT woken by an ignored signal (a reaped child's SIGCHLD);
 *      it must sleep on until a signal that is actually acted on.
 *
 * exit status is 255 for all TEN, 0 otherwise; the BITS= line is the
 * diagnosis (bits 0..7 filled a byte, so the exit can no longer be the mask
 * -- see the note in main()).
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
#include <signal.h>

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
static int g_fdinfo_dir_ok;
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


/* ---- bit0 (shared): the /dev nodes must OPEN and STAT alike --------------
 *
 * They were synthesised in the open path only, so this held:
 *
 *     open("/dev/null")  -> a working descriptor
 *     stat("/dev/null")  -> ENOENT
 *
 * which quietly broke `test -e /dev/null`, access(), `ls -l`, and Chromium's
 * base::PathExists -- the last of which PCHECKs in its zygote host and killed
 * the browser AFTER the entire sandbox had come up. The asymmetry is the bug,
 * so the assertion is the SYMMETRY: every node must do both, and must report
 * itself as a character device. Checking only stat() would pass on a kernel
 * that had lost open(). */
static const char *g_devnodes[] = {
    "/dev/null", "/dev/zero", "/dev/full", "/dev/tty",
    "/dev/random", "/dev/urandom",
};

static void check_devnodes(void)
{
    int n = (int)(sizeof g_devnodes / sizeof g_devnodes[0]);
    for (int i = 0; i < n; i++) {
        const char *d = g_devnodes[i];
        struct stat st;
        if (stat(d, &st) != 0) {
            no("dev: stat(%s) errno=%d -- openable but not stat-able is the "
               "asymmetry this check exists for", d, errno);
            return;
        }
        if (!S_ISCHR(st.st_mode)) {
            no("dev: %s stats as mode 0%o, not a character device", d,
               (unsigned)st.st_mode);
            return;
        }
        if (access(d, F_OK) != 0) {
            no("dev: access(%s, F_OK) errno=%d while stat() succeeded", d,
               errno);
            return;
        }
        /* /dev/tty needs a controlling terminal, so its open is allowed to
         * fail -- but only that one, and only that way. */
        int fd = open(d, O_RDONLY);
        if (fd < 0 && strcmp(d, "/dev/tty") != 0) {
            no("dev: %s stats fine but open() errno=%d -- the two paths "
               "disagree in the other direction now", d, errno);
            return;
        }
        if (fd >= 0) close(fd);
    }
    if (!g_fdinfo_dir_ok) {
        no("dev: the nodes are fine but the fdinfo directory check did not "
           "pass -- bit0 covers both");
        return;
    }
    ok(1, "dev: all %d synthesised nodes stat AND open and report S_ISCHR "
          "(+ fdinfo dir tracks the fd table)", n);
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
    g_fdinfo_dir_ok = 1;
    printf("gapfill:   ok   fdinfo: a directory that TRACKS the fd table "
           "(fd %d appeared among %d entries, gone among %d after close)\n",
           fd, before, after);
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

/* ---- bit 8: fchdir --------------------------------------------------------
 *
 * fchdir(2) was absent from BOTH the kernel dispatcher and libtoby until
 * 2026-08-26, so it fell through to the ENOSYS logger. Not exotic: the
 * openat(dirfd)+fchdir idiom is how find, tar, rsync, git and glibc's nftw(3)
 * walk a tree without rebuilding path strings, so real directory traversal was
 * quietly getting -ENOSYS.
 *
 * Two-sided on purpose. "fchdir returned 0" proves nothing -- a stub that
 * returns success would pass it. The claim is that the CWD ACTUALLY MOVED, so
 * getcwd() must report the new directory afterwards; and a NON-directory fd
 * must be REFUSED with ENOTDIR rather than silently accepted, because a
 * chdir to nowhere is worse than an error. */
static void check_fchdir(void)
{
    char before[4096], after[4096];
    if (!getcwd(before, sizeof before)) { no("fchdir: getcwd errno=%d", errno); return; }

    if (mkdir("/data/fchd", 0755) != 0 && errno != EEXIST) {
        no("fchdir: mkdir errno=%d", errno); return;
    }
    int dfd = open("/data/fchd", O_RDONLY | O_DIRECTORY);
    if (dfd < 0) { no("fchdir: open(dir) errno=%d", errno); return; }

    if (fchdir(dfd) != 0) {
        no("fchdir: returned errno=%d (was -ENOSYS before 2026-08-26)", errno);
        close(dfd); return;
    }
    if (!getcwd(after, sizeof after)) {
        no("fchdir: getcwd after errno=%d", errno); close(dfd); return;
    }
    if (strcmp(after, "/data/fchd") != 0) {
        no("fchdir: returned 0 but cwd is '%s', not /data/fchd -- the call "
           "succeeded without moving anything", after);
        close(dfd); (void)chdir(before); return;
    }
    close(dfd);

    /* The refusal is the other half of the claim. */
    int ffd = open("/data/fchd/f", O_RDWR | O_CREAT, 0644);
    if (ffd < 0) { no("fchdir: could not make a plain file (errno=%d)", errno);
                   (void)chdir(before); return; }
    int r = fchdir(ffd);
    int e = errno;
    close(ffd);
    (void)chdir(before);
    if (r == 0) {
        no("fchdir: ACCEPTED a non-directory fd -- cwd is now undefined");
        return;
    }
    if (e != ENOTDIR) {
        no("fchdir: refused a plain file with errno=%d, want ENOTDIR(%d)",
           e, ENOTDIR);
        return;
    }
    ok(0x100, "fchdir moves the cwd and refuses a non-directory (ENOTDIR)");
}

/* ---- bit 9: pause() is not woken by an IGNORED signal ---------------------
 *
 * POSIX: pause() returns only when a signal is CAUGHT (a handler runs) or
 * terminates the process. A signal being IGNORED is not a reason to return.
 * Before 2026-08-27 every blocking primitive here tested pending_signals raw,
 * so a reaped child's SIGCHLD -- default action ignore -- returned EINTR out
 * of a pause() that should have kept sleeping.
 *
 * DISCRIMINATES BY FLAG, NOT BY CLOCK. An earlier gate in this tree became
 * flaky because it judged a correctness property against a wall-clock
 * deadline; here the SIGALRM handler sets g_alarmed, so the assertion is
 * "which signal woke us", and the timings only need to be far apart
 * (~0.3 s child exit vs a 3 s alarm), not accurate. The alarm also
 * guarantees the case cannot hang the gate if the fix regresses the other
 * way and pause() sleeps forever. */
static volatile sig_atomic_t g_alarmed;
static void gapfill_on_alarm(int s) { (void)s; g_alarmed = 1; }

static void check_pause_ignored(void)
{
    if (signal(SIGALRM, gapfill_on_alarm) == SIG_ERR) {
        no("pause: cannot install SIGALRM handler (errno=%d)", errno); return;
    }
    /* SIGCHLD left at SIG_DFL == ignore, which is the whole point. */
    pid_t k = fork();
    if (k < 0) { no("pause: fork errno=%d", errno); return; }
    if (k == 0) { usleep(300 * 1000); _exit(0); }   /* exits while we pause */

    g_alarmed = 0;
    alarm(3);
    pause();
    int woke_by_alarm = (int)g_alarmed;
    alarm(0);
    int st = 0;
    (void)waitpid(k, &st, 0);
    signal(SIGALRM, SIG_DFL);

    if (!woke_by_alarm) {
        no("pause: returned WITHOUT SIGALRM having fired -- an ignored "
           "SIGCHLD from the reaped child woke it, which POSIX does not allow");
        return;
    }
    ok(0x200, "pause() ignored a child's SIGCHLD and slept until SIGALRM");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("gapfill: start pid=%d uid=%u\n", (int)getpid(), (unsigned)getuid());
    check_fdinfo_dir();
    check_devnodes();      /* shares bit0: both are "the two paths agree" */
    check_fdinfo_pos();
    check_chroot_fdinfo();
    check_mkdirat();
    check_getcpu();
    check_hints();
    check_openat2();
    check_fchdir();
    check_pause_ignored();
    /* THE EXIT CONTRACT CHANGED 2026-08-27, and the reason matters. Bits 0..7
     * filled a byte exactly (0xff) and the gate asserts exit==255, so a ninth
     * check had nowhere to go. Returning `g_bits & 0xff` would have been a
     * FALSE PASS: bits 0-7 green with bit8 red still yields 255. So the exit is
     * now PASS/FAIL and the BITS= line carries the diagnosis. */
    printf("gapfill: BITS=0x%x (want 0x3ff)\n", g_bits);
    return (g_bits == 0x3ff) ? 255 : 0;
}
