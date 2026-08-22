/* linux-procdev -- /proc and /dev fidelity batch (2026-08-22).
 *
 *   bit0  readlink(/proc/self) names OUR pid (it wrote the HOST pid,
 *         which inside a pid namespace is a process the reader cannot see)
 *   bit1  /proc/self/mounts exists and lists the mount table -- it was
 *         ENOENT while /proc/mounts worked, and self/mounts is what
 *         libmount/findmnt/Go actually read
 *   bit2  RAW stat(2) -- syscall 4 -- on /dev/null works and is S_IFCHR
 *         (the dev-synth fix covered statx/newfstatat/access and the
 *         handoff claimed stat too; raw 4/6 never had the arm)
 *   bit3  opendir("/dev") lists null and zero -- /dev was ENOENT to ls
 *         since forever
 *   bit4  POSIX shm: shm_open + ftruncate + MAP_SHARED write/read +
 *         shm_unlink round-trip (glibc shm_open IS open("/dev/shm/..");
 *         with no mount there it never existed for Linux binaries)
 *   bit5  /proc/self/cmdline is the REAL argv, NUL-separated -- it was
 *         name+'\n', which no argv reader parses
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0: /proc/self names our (namespace) pid ---- */
    {
        char lnk[64] = {0}, want[32];
        ssize_t n = readlink("/proc/self", lnk, sizeof lnk - 1);
        snprintf(want, sizeof want, "/proc/%d", (int)getpid());
        printf("procdev: readlink(/proc/self)='%s' want='%s'\n", lnk, want);
        if (n > 0 && strcmp(lnk, want) == 0) bits |= 1;
    }

    /* ---- bit1: /proc/self/mounts readable, mentions a mount ---- */
    {
        char mb[512] = {0};
        int fd = open("/proc/self/mounts", O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, mb, sizeof mb - 1) : -1;
        if (fd >= 0) close(fd);
        printf("procdev: self/mounts fd=%d n=%zd has-tobyos=%d\n",
               fd, n, strstr(mb, "tobyos") != 0);
        if (fd >= 0 && n > 0 && strstr(mb, "tobyos")) bits |= 2;
    }

    /* ---- bit2: raw stat(2) on a synthesised node ---- */
    {
        struct stat st;
        memset(&st, 0, sizeof st);
        long r = syscall(4 /* stat */, "/dev/null", &st);
        printf("procdev: raw stat(/dev/null) rc=%ld mode=%o ischr=%d\n",
               r, st.st_mode, S_ISCHR(st.st_mode));
        if (r == 0 && S_ISCHR(st.st_mode)) bits |= 4;
    }

    /* ---- bit3: /dev is listable ---- */
    {
        DIR *d = opendir("/dev");
        int saw_null = 0, saw_zero = 0, cnt = 0;
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != 0) {
                cnt++;
                if (strcmp(e->d_name, "null") == 0) saw_null = 1;
                if (strcmp(e->d_name, "zero") == 0) saw_zero = 1;
            }
            closedir(d);
        }
        printf("procdev: /dev entries=%d null=%d zero=%d\n",
               cnt, saw_null, saw_zero);
        if (d && saw_null && saw_zero) bits |= 8;
    }

    /* ---- bit4: POSIX shared memory round-trip ---- */
    {
        const char *nm = "/pd-shm-test";
        int fd = shm_open(nm, O_CREAT | O_RDWR, 0600);
        int ok = 0;
        if (fd >= 0) {
            if (ftruncate(fd, 4096) == 0) {
                void *m = mmap(0, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
                if (m != MAP_FAILED) {
                    memcpy(m, "SHMOK", 6);
                    ok = (memcmp(m, "SHMOK", 6) == 0);
                    munmap(m, 4096);
                }
            }
            close(fd);
        }
        int ul = shm_unlink(nm);
        printf("procdev: shm_open fd=%d roundtrip=%d unlink=%d\n", fd, ok, ul);
        if (fd >= 0 && ok && ul == 0) bits |= 16;
    }

    /* ---- bit5: cmdline is real argv ---- */
    {
        char cb[256] = {0};
        int fd = open("/proc/self/cmdline", O_RDONLY);
        ssize_t n = fd >= 0 ? read(fd, cb, sizeof cb - 1) : -1;
        if (fd >= 0) close(fd);
        /* argv[0] then a NUL; no trailing newline anywhere. */
        size_t a0 = strlen(cb);
        printf("procdev: cmdline n=%zd argv0='%s' nulsep=%d\n",
               n, cb, n > 0 && (size_t)n >= a0 + 1 && cb[a0] == '\0' &&
                      memchr(cb, '\n', (size_t)n) == 0);
        if (n > 0 && strstr(cb, "linux-procdev") &&
            memchr(cb, '\n', (size_t)n) == 0 &&
            cb[a0] == '\0') bits |= 32;
    }

    printf("LXPROCDEV: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
