/* linux-ctrprobe -- the payload that runs INSIDE the container, and the thing
 * that decides whether slice 16's capstone passes.
 *
 * ===========================================================================
 * WHY THIS IS A C PROGRAM AND NOT A SHELL SCRIPT
 *
 * The obvious capstone is `run busybox in a container and see that it prints
 * something`. That proves almost nothing: this arc has already been burned
 * three times by a green verdict over a real failure, every time because the
 * test asserted that a thing SUCCEEDED rather than what its VALUE was --
 * `chroot /jail /bin/busybox echo hi` running the HOST's binary, `arp_resolve`
 * returning the HOST NIC's MAC, `chown` refusing with the wrong errno.
 *
 * So every check below names an exact value, and the ones that can be
 * two-sided are: a thing that must be visible AND a thing that must not.
 *
 * Exit status is an 8-bit mask; 255 means the container really is one.
 *
 *   bit0  pid namespace   -- getpid() == 1 (we were exec'd as the container's
 *                            init), and /proc agrees
 *   bit1  uts namespace   -- hostname is the one config.json asked for, which
 *                            the HOST must therefore not be called
 *   bit2  credentials     -- uid AND gid are the configured non-root pair, and
 *                            a root-only operation now fails
 *   bit3  root filesystem -- TWO-SIDED: a file that exists only in the host
 *                            root is GONE, and a file that exists only in the
 *                            Alpine rootfs is READABLE. Either half alone can
 *                            pass by accident; together they cannot.
 *   bit4  net namespace   -- connect() fails with ENETUNREACH exactly. Not
 *                            "networking failed" -- the errno IS the assertion,
 *                            because a missing route and a denied namespace
 *                            look identical if you only check for failure.
 *   bit5  seccomp         -- the syscall the profile blocks returns exactly the
 *                            configured errno, AND an unblocked syscall still
 *                            works (a filter that broke everything would pass
 *                            the first half)
 *   bit6  cgroup pids.max -- fork ACTUALLY starts returning EAGAIN, below the
 *                            configured limit. Reading pids.max back would only
 *                            prove the file remembers what was written to it.
 *   bit7  it is ALPINE    -- exec the rootfs's OWN busybox and require its
 *                            version banner. The whole container is worthless
 *                            if the binaries came from the host, and this is
 *                            the only check that can tell.
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
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define WANT_HOST   "toby-container"
#define WANT_UID    1000
#define WANT_GID    1000
#define PIDS_MAX    8            /* must match config.json linux.resources.pids */
#define BLOCKED_ERR EPERM        /* must match config.json seccomp errnoRet    */

/* Only in the HOST root (staged into the initrd), never in an Alpine rootfs. */
#define HOST_ONLY   "/alpine.tar.gz"
/* Only in an Alpine rootfs. */
#define ALPINE_ONLY "/etc/alpine-release"

static int g_bits;

/* Properly variadic, and it was not the first time round. The first version
 * took a fixed (const char *, long) pair and handed BOTH to printf whatever the
 * format actually wanted -- so a message carrying only a %ld consumed the
 * POINTER as its number. Every value in the first capstone run printed as
 * 2100823, including the errnos and counts that ARE the assertions here. A
 * diagnostic that prints a wrong number is worse than one that prints none:
 * a reader cannot tell a wrong value from a wrong printf, and this whole file
 * exists on the premise that the value is the evidence. */
__attribute__((format(printf, 2, 3)))
static void ok(int bit, const char *fmt, ...)
{
    va_list ap;
    g_bits |= bit;
    printf("[CTR]   ok   ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}
__attribute__((format(printf, 1, 2)))
static void no(const char *fmt, ...)
{
    va_list ap;
    printf("[CTR]   FAIL ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

/* ---- bit0: pid namespace ------------------------------------------------ */
static void check_pid(void)
{
    pid_t me = getpid();
    if (me != 1) { no("pid: getpid()=%ld, want 1 -- not the container's init",
                      (long)me); return; }
    /* Second, independent witness: /proc must agree. If getpid() were being
     * translated but procfs were not, the container would see two different
     * identities for itself and anything reading /proc would escape the
     * illusion. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) { no("pid: getpid()=1 but /proc/self/status is unreadable"); return; }
    char line[160]; long ppid = -1, pid = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "Pid:", 4) == 0)  pid  = strtol(line + 4, 0, 10);
        if (strncmp(line, "PPid:", 5) == 0) ppid = strtol(line + 5, 0, 10);
    }
    fclose(f);
    if (pid != 1) { no("pid: /proc/self/status says Pid:%ld, want 1", pid);
                    return; }
    ok(1, "pid ns: init of its own namespace (getpid=1, /proc Pid:1, PPid:%ld)", ppid);
}

/* ---- bit1: uts namespace ------------------------------------------------ */
static void check_uts(void)
{
    char h[128] = {0};
    if (gethostname(h, sizeof h - 1) != 0) {
        no("uts: gethostname errno=%ld", (long)errno); return;
    }
    if (strcmp(h, WANT_HOST) != 0) {
        no("uts: hostname='%s', want '" WANT_HOST "'", h); return;
    }
    ok(2, "uts ns: hostname is '%s' (set by the runtime, not the host's)", h);
}

/* ---- bit2: credentials -------------------------------------------------- */
static void check_creds(void)
{
    if (getuid() != WANT_UID || geteuid() != WANT_UID) {
        no("creds: uid=%ld euid=%ld, want 1000", (long)getuid(),
           (long)geteuid()); return;
    }
    if (getgid() != WANT_GID) {
        no("creds: gid=%ld, want 1000", (long)getgid()); return;
    }
    /* The field moving is not the point -- the AUTHORITY has to be gone. A
     * drop that renumbered uid and kept root's powers would pass a getuid()
     * check and be worse than no drop, because it would look safe. */
    if (chown(ALPINE_ONLY, 0, 0) == 0) {
        no("creds: uid is 1000 but chown() to root:root still SUCCEEDS -- the "
           "authority did not drop");
        return;
    }
    if (errno != EPERM && errno != EACCES) {
        no("creds: chown refused with errno=%ld, want EPERM/EACCES", (long)errno);
        return;
    }
    ok(4, "creds: uid=gid=1000 and a root-only chown is refused (errno=%ld)", (long)errno);
}

/* ---- bit3: the root filesystem, two-sided ------------------------------- */
static void check_root(void)
{
    if (access(HOST_ONLY, F_OK) == 0) {
        no("root: " HOST_ONLY " is STILL VISIBLE -- the host root was not "
           "detached");
        return;
    }
    if (access("/.oldroot", F_OK) == 0) {
        no("root: /.oldroot is still present -- pivot_root's old root was not "
           "unmounted");
        return;
    }
    int fd = open(ALPINE_ONLY, O_RDONLY);
    if (fd < 0) {
        no("root: " ALPINE_ONLY " unreadable (errno=%ld) -- / is not the "
           "Alpine rootfs", (long)errno);
        return;
    }
    char rel[64] = {0};
    ssize_t n = read(fd, rel, sizeof rel - 1);
    close(fd);
    if (n <= 0) { no("root: " ALPINE_ONLY " is empty"); return; }
    for (char *p = rel; *p; p++) if (*p == '\n') { *p = 0; break; }
    ok(8, "root: host root GONE and / is Alpine %s (both halves)", rel);
}

/* ---- bit4: net namespace ------------------------------------------------ */
static void check_net(void)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        /* Also acceptable evidence, but say which one happened -- "no socket
         * at all" and "no route" are different kernels. */
        if (errno == ENETUNREACH || errno == EAFNOSUPPORT) {
            ok(16, "net ns: socket() itself refused (errno=%ld)", (long)errno);
            return;
        }
        no("net: socket() errno=%ld, want a network-namespace refusal", (long)errno);
        return;
    }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(80);
    sa.sin_addr.s_addr = inet_addr("10.0.2.2");   /* QEMU's gateway */
    int rc = connect(s, (struct sockaddr *)&sa, sizeof sa);
    int e = errno;
    close(s);
    if (rc == 0) {
        no("net: connect() to the host gateway SUCCEEDED from inside the "
           "container");
        return;
    }
    if (e != ENETUNREACH) {
        no("net: connect() failed with errno=%ld, want ENETUNREACH (%ld) -- a "
           "refusal is not evidence unless it is the RIGHT refusal",
           (long)e, (long)ENETUNREACH);
        return;
    }
    ok(16, "net ns: connect(10.0.2.2:80) -> ENETUNREACH exactly (empty ns)");
}

/* ---- bit5: seccomp ------------------------------------------------------ */
static void check_seccomp(void)
{
    /* syscall(SYS_chmod) on purpose, not chmod(3): glibc routes chmod() to
     * fchmodat(2), so a profile naming "chmod" would never be consulted and
     * this check would report a working filter as broken. Name the syscall the
     * profile names. */
    long rc = syscall(SYS_chmod, ALPINE_ONLY, 0666);
    int e = errno;
    if (rc == 0) {
        no("seccomp: the blocked syscall SUCCEEDED -- the filter is not "
           "installed");
        return;
    }
    if (e != BLOCKED_ERR) {
        no("seccomp: blocked syscall failed with errno=%ld, want the "
           "configured %ld", (long)e, (long)BLOCKED_ERR);
        return;
    }
    /* ...and the filter did not simply break everything: an unfiltered call
     * must still work. Without this half, a filter that denied every syscall
     * would score as a pass. */
    if (access("/", F_OK) != 0) {
        no("seccomp: an UNBLOCKED syscall also fails (errno=%ld) -- the filter "
           "is not selective", (long)errno);
        return;
    }
    ok(32, "seccomp: chmod -> errno=%ld as configured, other syscalls "
           "unaffected", (long)e);
}

/* ---- bit6: cgroup pids.max ---------------------------------------------- */
static void check_pids(void)
{
    pid_t kids[PIDS_MAX * 4];
    int n = 0, hit = 0;
    for (int i = 0; i < (int)(sizeof kids / sizeof kids[0]); i++) {
        pid_t p = fork();
        if (p == 0) { pause(); _exit(0); }     /* stay alive, hold a slot */
        if (p < 0) {
            if (errno == EAGAIN) hit = 1;
            else no("pids: fork failed with errno=%ld, want EAGAIN", (long)errno);
            break;
        }
        kids[n++] = p;
    }
    /* Same lesson: check the teardown. kill() and waitpid() both translate a
     * pid through this namespace, so if fork handed back a number the namespace
     * cannot name, BOTH fail -- and the EAGAIN assertion below would still pass
     * while leaving the children alive. */
    int reaped = 0;
    for (int i = 0; i < n; i++) {
        if (kill(kids[i], SIGKILL) != 0) {
            no("pids: kill(%ld) errno=%ld -- the pid fork returned is not one "
               "this namespace can signal", (long)kids[i], (long)errno);
            return;
        }
        int st;
        if (waitpid(kids[i], &st, 0) == kids[i]) reaped++;
    }
    if (reaped != n) {
        no("pids: reaped %ld of %ld children -- waitpid cannot name what fork "
           "returned", (long)reaped, (long)n);
        return;
    }

    if (!hit) {
        no("pids: forked %ld children without ever hitting pids.max=%d -- the "
           "limit is not enforced", (long)n, PIDS_MAX);
        return;
    }
    if (n >= PIDS_MAX) {
        no("pids: EAGAIN arrived only after %ld children, but pids.max is %d",
           (long)n, PIDS_MAX);
        return;
    }
    ok(64, "cgroup pids.max: fork returned EAGAIN after %ld children "
           "(limit %d, enforced not just stored)", (long)n, PIDS_MAX);
}

/* ---- bit7: this is Alpine's userland ------------------------------------ */
static void check_alpine(void)
{
    int fds[2];
    if (pipe(fds) != 0) { no("alpine: pipe errno=%ld", (long)errno); return; }
    pid_t p = fork();
    if (p < 0) { no("alpine: fork errno=%ld", (long)errno); return; }
    if (p == 0) {
        close(fds[0]);
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[1]);
        char *av[] = { (char *)"busybox", NULL };
        char *ev[] = { (char *)"PATH=/bin:/sbin:/usr/bin", NULL };
        execve("/bin/busybox", av, ev);
        _exit(127);
    }
    close(fds[1]);
    char buf[512] = {0};
    ssize_t got = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    /* CHECK the reap. The first version wrote `waitpid(p, &st, 0);` and threw
     * the result away, which let a real bug through: inside a pid namespace
     * fork() was returning the HOST pid, so this waitpid was failing with
     * ECHILD on every run -- and because the pipe still carried busybox's
     * banner, the check reported ok. A call allowed to fail silently is not
     * evidence. linux-clonestk caught what this missed. */
    int st = 0;
    pid_t w = waitpid(p, &st, 0);
    if (w != p) {
        no("alpine: fork()=%ld but waitpid returned %ld errno=%ld -- the pid "
           "fork handed back is not one this namespace can wait on",
           (long)p, (long)w, (long)errno);
        return;
    }
    if (got <= 0) {
        no("alpine: the rootfs's /bin/busybox produced no output (exit "
           "status 0x%lx)", (long)st);
        return;
    }
    for (char *q = buf; *q; q++) if (*q == '\n') { *q = 0; break; }
    if (!strstr(buf, "BusyBox v1.36")) {
        no("alpine: /bin/busybox banner is '%s' -- not Alpine 3.19's v1.36, so "
           "the binary that ran was not the rootfs's", buf);
        return;
    }
    ok(128, "alpine: the container's OWN busybox ran -- '%s'", buf);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[CTR] container probe starting\n");
    check_pid();
    check_uts();
    check_root();      /* before creds: check_creds chowns a file it names */
    check_creds();
    check_net();
    check_alpine();    /* before seccomp: exec/fork stay unfiltered either way */
    check_seccomp();
    check_pids();
    printf("[CTR] BITS=0x%x (want 0xff)\n", g_bits);
    return g_bits;
}
