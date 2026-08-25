/* fsprobe -- does an ordinary user actually get a usable machine?
 *
 * WHY THIS EXISTS. Every gate in this tree so far has run its probes from
 * pid 0, which means every probe ran as ROOT. The EliteDesk boot of
 * 2026-08-24 logged in as `toby` (uid 1000) and lspci exited 1, while the
 * PKGPROBE run of the very same tree, on the very same day, called it
 * green. A gate that only ever tests uid 0 cannot see a permission bug,
 * and permissions are exactly what separates "boots" from "usable".
 *
 * So this harness has two phases in one binary:
 *
 *   fsprobe            root phase, then re-spawns itself as --user
 *   fsprobe --user     setgid(1000) + setuid(1000), then the user phase
 *
 * Phase one asserts the filesystem CRUD contract (create, read, update,
 * rename, truncate, chmod, delete, mkdir/rmdir) on every mount a user
 * writes to. Phase two re-runs the parts a non-root user is entitled to,
 * and walks /sys the way lspci/lsusb/sensors/dmidecode/cpupower do.
 *
 * Every check asserts a VALUE. "exit 0" is not evidence -- lspci exited 1
 * on hardware while returning perfectly well-formed output to root. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define TAG "[FSPROBE]"

static int g_pass = 0, g_fail = 0;
static const char *g_phase = "root";
static const char *g_where = "";

static void ok(const char *what, const char *detail) {
    g_pass++;
    printf("%s   ok   %-5s %-8s %-26s %s\n", TAG, g_phase, g_where, what,
           detail ? detail : "");
}
static void bad(const char *what, const char *detail) {
    g_fail++;
    printf("%s  FAIL  %-5s %-8s %-26s %s\n", TAG, g_phase, g_where, what,
           detail ? detail : "");
}
static void check(int cond, const char *what, const char *detail) {
    if (cond) ok(what, detail); else bad(what, detail);
}

/* ---- small helpers --------------------------------------------------- */

static long file_size(const char *p) {
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long)st.st_size;
}
static int write_file(const char *p, const char *s, int mode) {
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    long n = (long)strlen(s);
    long w = write(fd, s, (size_t)n);
    close(fd);
    return (w == n) ? 0 : -1;
}
static int read_file(const char *p, char *buf, size_t cap) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = 0;
    return (int)n;
}
static int dir_has(const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e; int found = 0;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, name) == 0) { found = 1; break; }
    closedir(d);
    return found;
}

/* ---- the CRUD matrix, run against one directory ---------------------- */

static void crud(const char *dir) {
    char f[256], f2[256], sub[256], inner[256], buf[128], msg[400];
    const char *sep = (strcmp(dir, "/") == 0) ? "" : "/";
    g_where = dir;
    snprintf(f,     sizeof f,     "%s%sfsprobe.txt",  dir, sep);
    snprintf(f2,    sizeof f2,    "%s%sfsprobe2.txt", dir, sep);
    snprintf(sub,   sizeof sub,   "%s%sfsprobedir",   dir, sep);
    snprintf(inner, sizeof inner, "%s%sfsprobedir/in.txt", dir, sep);

    /* Leave no wreckage from an earlier run behind. */
    unlink(f); unlink(f2); unlink(inner); rmdir(sub);

    /* C -- create */
    if (write_file(f, "hello\n", 0644) != 0) {
        snprintf(msg, sizeof msg, "%s: errno=%d", f, errno);
        bad("create+write", msg);
        return;                       /* the rest is meaningless without it */
    }
    ok("create+write", f);

    /* R -- read back the exact bytes */
    int n = read_file(f, buf, sizeof buf);
    snprintf(msg, sizeof msg, "%d bytes", n);
    check(n == 6 && strcmp(buf, "hello\n") == 0, "read-back", msg);

    /* R -- and the name is in the directory listing */
    {
        const char *base = strrchr(f, '/'); base = base ? base + 1 : f;
        int has = dir_has(dir, base);
        snprintf(msg, sizeof msg, "found=%d", has);
        check(has == 1, "listed by readdir", msg);
    }

    /* U -- overwrite in place */
    {
        int fd = open(f, O_WRONLY);
        long w = (fd >= 0) ? write(fd, "HELLO", 5) : -1;
        if (fd >= 0) close(fd);
        n = read_file(f, buf, sizeof buf);
        snprintf(msg, sizeof msg, "w=%ld n=%d", w, n);
        check(w == 5 && n == 6 && strcmp(buf, "HELLO\n") == 0,
              "update in place", msg);
    }

    /* U -- append past the old end: the file must GROW */
    {
        int fd = open(f, O_WRONLY | O_APPEND);
        long w = (fd >= 0) ? write(fd, "world\n", 6) : -1;
        if (fd >= 0) close(fd);
        long sz = file_size(f);
        snprintf(msg, sizeof msg, "w=%ld size=%ld want 12", w, sz);
        check(w == 6 && sz == 12, "append grows the file", msg);
    }

    /* U -- truncate by path, then by fd */
    {
        int rc = truncate(f, 5);
        long sz = file_size(f);
        snprintf(msg, sizeof msg, "rc=%d size=%ld want 5", rc, sz);
        check(rc == 0 && sz == 5, "truncate(path)", msg);
    }
    {
        int fd = open(f, O_WRONLY);
        int rc = (fd >= 0) ? ftruncate(fd, 2) : -1;
        if (fd >= 0) close(fd);
        long sz = file_size(f);
        snprintf(msg, sizeof msg, "rc=%d size=%ld want 2", rc, sz);
        check(rc == 0 && sz == 2, "ftruncate(fd)", msg);
    }

    /* U -- chmod, read back through stat */
    {
        int rc = chmod(f, 0600);
        struct stat st; st.st_mode = 0;
        int src = stat(f, &st);
        snprintf(msg, sizeof msg, "rc=%d mode=%04o want 0600",
                 rc, (unsigned)(st.st_mode & 07777));
        check(rc == 0 && src == 0 && (st.st_mode & 07777) == 0600, "chmod", msg);
    }

    /* rmdir MUST NOT REMOVE A FILE. libtoby's rmdir() was literally
     * `return unlink(path)`, so `rmdir somefile` deleted it -- the check
     * every "try rmdir, fall back to unlink" script relies on was silently
     * destructive. Assert both halves: the call fails AND the file lives. */
    {
        int rc = rmdir(f);
        long sz = file_size(f);
        snprintf(msg, sizeof msg, "rc=%d want nonzero, file-still-there=%d",
                 rc, sz >= 0);
        check(rc != 0 && sz >= 0, "rmdir refuses a file", msg);
    }

    /* Restate the contents before the rename check so it measures RENAME
     * and nothing else. Chaining it off the truncate result made one
     * failure report as two. */
    write_file(f, "ab", 0644);

    /* U -- rename: the old name must be GONE and the new one present */
    {
        int rc = rename(f, f2);
        long oldsz = file_size(f), newsz = file_size(f2);
        snprintf(msg, sizeof msg, "rc=%d old=%ld want -1  new=%ld want 2",
                 rc, oldsz, newsz);
        check(rc == 0 && oldsz < 0 && newsz == 2, "rename", msg);
    }

    /* D -- delete. Whichever name survived rename is the one to remove. */
    {
        const char *victim = (file_size(f2) >= 0) ? f2 : f;
        int rc = unlink(victim);
        long sz = file_size(victim);
        snprintf(msg, sizeof msg, "rc=%d size-after=%ld want -1", rc, sz);
        check(rc == 0 && sz < 0, "unlink", msg);
        unlink(f); unlink(f2);
    }

    /* directories: mkdir / populate / rmdir */
    {
        int rc = mkdir(sub, 0755);
        struct stat st; st.st_mode = 0;
        int src = stat(sub, &st);
        snprintf(msg, sizeof msg, "rc=%d isdir=%d", rc,
                 (src == 0 && S_ISDIR(st.st_mode)) ? 1 : 0);
        check(rc == 0 && src == 0 && S_ISDIR(st.st_mode), "mkdir", msg);
    }
    {
        int rc = write_file(inner, "x\n", 0644);
        snprintf(msg, sizeof msg, "rc=%d", rc);
        check(rc == 0, "create inside new dir", msg);
    }
    {
        /* A non-empty directory MUST refuse to go. Silently succeeding
         * here would orphan the child. */
        int rc = rmdir(sub);
        snprintf(msg, sizeof msg, "rc=%d want nonzero", rc);
        check(rc != 0, "rmdir refuses non-empty", msg);
    }
    {
        unlink(inner);
        int rc = rmdir(sub);
        struct stat st;
        int src = stat(sub, &st);
        snprintf(msg, sizeof msg, "rc=%d stat-after=%d want -1", rc, src);
        check(rc == 0 && src != 0, "rmdir", msg);
    }
    g_where = "";
}

/* ---- /sys, walked the way the native tools walk it ------------------- */

static void sys_read(const char *path, const char *what) {
    char buf[256], msg[400];
    int n = read_file(path, buf, sizeof buf);
    for (int i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = 0;
    snprintf(msg, sizeof msg, "%s -> %s", path, n > 0 ? buf : "(unreadable)");
    check(n > 0, what, msg);
}

static void sys_dir(const char *path, const char *what, int want_min) {
    char msg[400];
    DIR *d = opendir(path);
    if (!d) {
        snprintf(msg, sizeof msg, "opendir(%s) failed errno=%d", path, errno);
        bad(what, msg);
        return;
    }
    int n = 0; struct dirent *e; char first[64]; first[0] = 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!first[0]) snprintf(first, sizeof first, "%s", e->d_name);
        n++;
    }
    closedir(d);
    snprintf(msg, sizeof msg, "%d entries want >=%d, first=%s",
             n, want_min, first[0] ? first : "(none)");
    check(n >= want_min, what, msg);
}

/* The exact bit that was wrong on hardware.
 *
 * sysfs defaulted EVERY node to 0444 -- directories included. vfs_opendir
 * asks vfs_perm_check for READ|EXEC and vfs_perm_check short-circuits for
 * uid 0, so root walked the tree and every gate was green while a
 * logged-in user got VFS_ERR_PERM on the first component. Asserting the
 * MODE, not just that the walk happens to work, is what makes this a
 * regression guard rather than a coincidence. */
static void sysfs_modes(void) {
    static const char *dirs[] = { "/sys", "/sys/bus", "/sys/bus/pci",
                                  "/sys/bus/pci/devices", "/sys/class", 0 };
    for (int i = 0; dirs[i]; i++) {
        struct stat st; char msg[400];
        if (stat(dirs[i], &st) != 0) {
            snprintf(msg, sizeof msg, "stat(%s) failed errno=%d", dirs[i], errno);
            bad("sysfs dir is traversable", msg);
            continue;
        }
        snprintf(msg, sizeof msg, "%s mode=%04o isdir=%d (needs o+x)",
                 dirs[i], (unsigned)(st.st_mode & 07777), S_ISDIR(st.st_mode) ? 1 : 0);
        check(S_ISDIR(st.st_mode) && (st.st_mode & 0001), "sysfs dir is traversable", msg);
    }
}

static void sysfs_walk(void) {
    g_where = "/sys";
    sysfs_modes();
    sys_dir("/sys",                 "opendir /sys",            3);
    sys_dir("/sys/bus/pci/devices", "opendir pci devices",     1);
    sys_dir("/sys/bus/usb/devices", "opendir usb devices",     0);
    sys_dir("/sys/class",           "opendir /sys/class",      1);
    sys_read("/sys/kernel/version", "read kernel version");
    /* One leaf three directories deep: traversal, not just the top. */
    {
        DIR *d = opendir("/sys/bus/pci/devices");
        char leaf[256]; leaf[0] = 0;
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') continue;
                snprintf(leaf, sizeof leaf,
                         "/sys/bus/pci/devices/%s/vendor", e->d_name);
                break;
            }
            closedir(d);
        }
        if (leaf[0]) sys_read(leaf, "read a pci vendor leaf");
        else bad("read a pci vendor leaf", "no device directory to read");
    }
    g_where = "";
}

/* ---- the pty line editor -------------------------------------------
 *
 * The GUI terminal's Backspace goes out as DEL (0x7F) because the line
 * discipline's VERASE is DEL. It used to go out as BS (0x08), which
 * matched nothing: the byte was not an erase, it went into the command as
 * a literal control character, and the terminal drew it. The kernel end
 * was half-broken too -- the erase was applied when the slave ASSEMBLED
 * the line, far too late to echo a rub-out, so the master saw a raw 0x7F
 * and left the rubbed-out character on screen.
 *
 * This asserts BOTH halves on a real pty, which is the only way to check
 * them without a human at a keyboard: what the program READS, and what
 * the terminal is TOLD TO DRAW. */

#define TIOCSPTLCK  0x40045431UL
#define TIOCGPTN    0x80045430UL
#define FIONREAD    0x541BUL

static int drain_master(int mfd, char *buf, size_t cap) {
    int avail = 0, got = 0;
    /* FIONREAD first, always: a read() on a quiet master blocks. */
    while (ioctl(mfd, FIONREAD, &avail) == 0 && avail > 0 && (size_t)got < cap - 1) {
        int want = avail;
        if ((size_t)want > cap - 1 - (size_t)got) want = (int)(cap - 1 - (size_t)got);
        long n = read(mfd, buf + got, (size_t)want);
        if (n <= 0) break;
        got += (int)n;
    }
    buf[got] = 0;
    return got;
}

/* Render control bytes so a mismatch is readable in the serial log. */
static void visible(const char *in, int n, char *out, size_t cap) {
    size_t o = 0;
    for (int i = 0; i < n && o + 5 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\b')      { out[o++] = '<'; out[o++] = 'B'; out[o++] = 'S'; out[o++] = '>'; }
        else if (c == '\r') { out[o++] = '<'; out[o++] = 'C'; out[o++] = 'R'; out[o++] = '>'; }
        else if (c == '\n') { out[o++] = '<'; out[o++] = 'L'; out[o++] = 'F'; out[o++] = '>'; }
        else if (c == 0x7f) { out[o++] = '<'; out[o++] = 'D'; out[o++] = 'L'; out[o++] = '>'; }
        else if (c < 0x20)  { out[o++] = '^'; out[o++] = (char)('@' + c); }
        else                { out[o++] = (char)c; }
    }
    out[o] = 0;
}

static void pty_edit(void) {
    char msg[400], sbuf[128], mbuf[256], vis[400];
    g_where = "pty";

    int mfd = open("/dev/ptmx", O_RDWR);
    if (mfd < 0) { bad("open /dev/ptmx", "no pty master"); return; }
    int unlock = 0; (void)ioctl(mfd, TIOCSPTLCK, &unlock);
    int idx = -1;
    if (ioctl(mfd, TIOCGPTN, &idx) != 0 || idx < 0) {
        bad("TIOCGPTN", "no slave index"); close(mfd); return;
    }
    char spath[32];
    snprintf(spath, sizeof spath, "/dev/pts/%d", idx);
    int sfd = open(spath, O_RDWR);
    if (sfd < 0) { bad("open pty slave", spath); close(mfd); return; }

    /* Type "abc", Backspace, "d", Enter -- exactly what the GUI terminal
     * now sends for those five keys. */
    const char typed[] = { 'a', 'b', 'c', 0x7f, 'd', '\r' };
    (void)write(mfd, typed, sizeof typed);

    long n = read(sfd, sbuf, sizeof sbuf - 1);
    if (n < 0) n = 0;
    sbuf[n] = 0;
    visible(sbuf, (int)n, vis, sizeof vis);
    snprintf(msg, sizeof msg, "program read '%s' want 'abd<LF>'", vis);
    check(n == 4 && sbuf[0] == 'a' && sbuf[1] == 'b' && sbuf[2] == 'd' &&
          sbuf[3] == '\n', "VERASE erases the buffer", msg);

    int mn = drain_master(mfd, mbuf, sizeof mbuf);
    visible(mbuf, mn, vis, sizeof vis);
    /* ECHOE: the erase must come back as backspace-space-backspace so the
     * character actually leaves the screen. A bare 0x7F in this stream is
     * the old bug. */
    int has_rubout = 0;
    for (int i = 0; i + 2 < mn; i++)
        if (mbuf[i] == '\b' && mbuf[i + 1] == ' ' && mbuf[i + 2] == '\b') has_rubout = 1;
    int has_raw_del = 0;
    for (int i = 0; i < mn; i++) if ((unsigned char)mbuf[i] == 0x7f) has_raw_del = 1;
    snprintf(msg, sizeof msg, "terminal saw '%s' (rubout=%d rawDEL=%d)",
             vis, has_rubout, has_raw_del);
    check(has_rubout && !has_raw_del, "ECHOE echoes a rub-out", msg);

    /* VKILL (^U) discards the whole pending line, and must not reach back
     * past a newline into a line already delivered. */
    const char killed[] = { 'x', 'y', 'z', 0x15, 'q', '\r' };
    (void)write(mfd, killed, sizeof killed);
    n = read(sfd, sbuf, sizeof sbuf - 1);
    if (n < 0) n = 0;
    sbuf[n] = 0;
    visible(sbuf, (int)n, vis, sizeof vis);
    snprintf(msg, sizeof msg, "program read '%s' want 'q<LF>'", vis);
    check(n == 2 && sbuf[0] == 'q' && sbuf[1] == '\n', "VKILL discards the line", msg);
    (void)drain_master(mfd, mbuf, sizeof mbuf);

    close(sfd);
    close(mfd);
    g_where = "";
}

/* ---- the hardware tools, run the way the user runs them -------------
 *
 * lspci exited 1 on the EliteDesk while printing a full listing to root.
 * Asserting that /sys is readable is necessary but not sufficient -- the
 * claim the user cares about is that the COMMAND works, so run it. */
extern pid_t toby_spawn(const char *path, char *const argv[],
                        char *const envp[], int fd0, int fd1, int fd2);

static void run_tool(const char *path, const char *a1, const char *a2,
                     int want_rc, const char *what) {
    char msg[400];
    char *argv[4];
    const char *base = strrchr(path, '/');
    argv[0] = (char *)(base ? base + 1 : path);
    argv[1] = (char *)a1;
    argv[2] = (char *)a2;
    argv[3] = 0;
    char *envp[] = { (char *)"PATH=/bin", (char *)"HOME=/",
                     (char *)"TERM=vt100", (char *)"LANG=C", 0 };
    pid_t pid = toby_spawn(path, argv, envp, 0, 1, 2);
    if (pid < 0) {
        snprintf(msg, sizeof msg, "%s: spawn failed", path);
        bad(what, msg);
        return;
    }
    int st = -1;
    waitpid(pid, &st, 0);
    snprintf(msg, sizeof msg, "%s exit=%d want %d", path, st, want_rc);
    check(st == want_rc, what, msg);
}

static void hardware_tools(int as_root) {
    char msg[400], buf[128];
    g_where = "tools";
    /* lspci is THE case: it exited 1 as `toby` on a machine whose
     * /sys/bus/pci/devices held every function. It must work for BOTH. */
    run_tool("/bin/lspci", 0, 0, 0, "lspci");
    run_tool("/bin/lspci", "-m", 0, 0, "lspci -m");
    run_tool("/bin/lspci", "-v", 0, 0, "lspci -v");

    /* dmidecode is ROOT-ONLY, and that is correct rather than a bug: the
     * raw tables under /sys/firmware/dmi/tables are mode 0400, exactly as
     * Linux publishes them. So assert the boundary in BOTH directions --
     * it works as root, it is refused as a user -- instead of asserting
     * success everywhere, which is what a first pass of this gate did.
     * A permission model is only proven by the denial. */
    run_tool("/bin/dmidecode", "-t", "1", as_root ? 0 : 1,
             as_root ? "dmidecode -t 1" : "dmidecode is root-only");

    if (!as_root) {
        /* ...and the SAME uid must still get the non-secret identity
         * files, which are 0444. Otherwise "denied" would be indis-
         * tinguishable from /sys being unreadable again. */
        int n = read_file("/sys/class/dmi/id/sys_vendor", buf, sizeof buf);
        for (int i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = 0;
        snprintf(msg, sizeof msg, "sys_vendor -> %s", n > 0 ? buf : "(unreadable)");
        check(n > 0, "public dmi id readable", msg);

        /* And must NOT get the serial, which is 0400 for the same reason
         * Linux keeps it that way. */
        int s = read_file("/sys/class/dmi/id/product_serial", buf, sizeof buf);
        snprintf(msg, sizeof msg, "product_serial read -> %d (want <=0)", s);
        check(s <= 0, "dmi serial stays root-only", msg);
    }
    /* NOT asserted here: sensors and cpupower. QEMU advertises neither a
     * thermal sensor nor EIST, so the correct behaviour is to decline and
     * exit non-zero -- a "want 0" check would be a lie about what this
     * machine can show. CPUTELEM_SELFTEST covers their arithmetic and the
     * EliteDesk covers the live path. lsusb likewise: no USB device is
     * attached to this QEMU, so there is nothing for it to list. */
    g_where = "";
}

/* ---- the two phases -------------------------------------------------- */

static void phase_root(void) {
    g_phase = "root";
    printf("%s ==== phase 1: uid=%d -- filesystem CRUD ====\n",
           TAG, (int)getuid());
    crud("/");
    crud("/etc");
    crud("/tmp");
    crud("/data");
    sysfs_walk();
    pty_edit();
    hardware_tools(1);
}

static void phase_user(void) {
    char msg[128];
    if (setgid(1000) != 0) { printf("%s  FAIL  setgid errno=%d\n", TAG, errno); g_fail++; }
    if (setuid(1000) != 0) { printf("%s  FAIL  setuid errno=%d\n", TAG, errno); g_fail++; }
    g_phase = "user";
    printf("%s ==== phase 2: uid=%d -- what a logged-in user gets ====\n",
           TAG, (int)getuid());
    snprintf(msg, sizeof msg, "uid=%d gid=%d", (int)getuid(), (int)getgid());
    check(getuid() == 1000, "privilege really dropped", msg);
    /* The two mounts a desktop session actually writes to. */
    crud("/tmp");
    crud("/data");
    /* And the tree every native hardware tool reads. This is the check
     * that was missing when lspci shipped broken. */
    sysfs_walk();
    hardware_tools(0);
}

int main(int argc, char **argv) {
    int user_phase = (argc > 1 && strcmp(argv[1], "--user") == 0);

    if (user_phase) {
        phase_user();
        printf("%s SUBTOTAL user pass=%d fail=%d\n", TAG, g_pass, g_fail);
        return g_fail ? 1 : 0;
    }

    phase_root();
    printf("%s SUBTOTAL root pass=%d fail=%d\n", TAG, g_pass, g_fail);

    /* Re-spawn ourselves unprivileged. A separate process because setuid
     * is one-way: there is no coming back to finish the root phase. */
    int rootfail = g_fail;
    char *cargv[] = { (char *)"fsprobe", (char *)"--user", 0 };
    char *cenvp[] = { (char *)"PATH=/bin", (char *)"HOME=/", 0 };
    pid_t pid = toby_spawn("/bin/fsprobe", cargv, cenvp, 0, 1, 2);
    int st = -1;
    if (pid < 0) {
        printf("%s  FAIL  cannot spawn the user phase\n", TAG);
        st = 1;
    } else {
        waitpid(pid, &st, 0);
    }

    printf("%s VERDICT: %s (root fail=%d, user rc=%d)\n", TAG,
           (rootfail == 0 && st == 0) ? "PASS" : "FAIL", rootfail, st);
    return (rootfail == 0 && st == 0) ? 0 : 1;
}
