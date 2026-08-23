/* linux-vfork -- TRUE vfork: shared stack + suspended parent (2026-08-23).
 *
 * The child used to get a private zeroed stack in the shared CR3 -- so an
 * rsp-relative local written by the child was invisible to the parent, and
 * rsp-relative argument reads in the child found zeros. These bits are the
 * contract, each one broken under the old shim:
 *
 *   bit0  a volatile local written by the child is visible to the parent
 *   bit1  the parent is really SUSPENDED: the child sleeps 100 ms and
 *         finishes its sequence before vfork() returns in the parent
 *   bit2  vfork + execve with argv built in the SHARED FRAME (the read
 *         that used to find zeroed memory)
 *   bit3  exec-failure errno reported through a parent-frame local --
 *         the classic `err = errno` channel every spawner uses
 *   bit4  posix_spawn: success (exit 42 observed) AND failure (ENOENT
 *         returned, not a lost child)
 *   bit5  raw clone(CLONE_VM|CLONE_VFORK|SIGCHLD, stack=NULL): the
 *         busybox/`unshare -f` shape, child on the parent's stack
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>

extern char **environ;
static volatile int g_raw_marker;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "kid") == 0) _exit(42);

    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    /* ---- bit0 + bit1: shared frame, suspended parent ---- */
    {
        volatile int mark = 0;
        volatile int seq = 0;
        long t0 = now_ms();
        pid_t p = vfork();
        if (p == 0) {
            mark = 1234;
            seq = 1;
            usleep(100000);
            seq = 2;
            _exit(5);
        }
        long dt = now_ms() - t0;
        int st = 0;
        waitpid(p, &st, 0);
        printf("vf: shared mark=%d seq=%d dt=%ldms exit=%d\n",
               mark, seq, dt, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        if (p > 0 && mark == 1234) bits |= 1;
        if (seq == 2 && dt >= 80) bits |= 2;
    }

    /* ---- bit2: exec with argv in the shared frame ---- */
    {
        pid_t p = vfork();
        if (p == 0) {
            char *av[] = { (char *)"/bin/linux-vfork", (char *)"kid", 0 };
            execve("/bin/linux-vfork", av, environ);
            _exit(9);
        }
        int st = 0;
        waitpid(p, &st, 0);
        int e = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        printf("vf: exec kid exit=%d (42=ok)\n", e);
        if (e == 42) bits |= 4;
    }

    /* ---- bit3: exec-failure errno through the parent's frame ---- */
    {
        volatile int save_err = 0;
        pid_t p = vfork();
        if (p == 0) {
            char *av[] = { (char *)"/nonexistent", 0 };
            execve("/nonexistent", av, environ);
            save_err = errno;
            _exit(127);
        }
        int st = 0;
        waitpid(p, &st, 0);
        printf("vf: exec-fail err=%d (ENOENT=%d) exit=%d\n",
               save_err, ENOENT,
               WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        if (save_err == ENOENT && WIFEXITED(st) && WEXITSTATUS(st) == 127)
            bits |= 8;
    }

    /* ---- bit4: posix_spawn success + reported failure ---- */
    {
        pid_t sp = 0;
        char *av[] = { (char *)"/bin/linux-vfork", (char *)"kid", 0 };
        int r1 = posix_spawn(&sp, "/bin/linux-vfork", 0, 0, av, environ);
        int st = 0;
        if (r1 == 0) waitpid(sp, &st, 0);
        int ok1 = (r1 == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 42);
        char *av2[] = { (char *)"/nonexistent", 0 };
        int r2 = posix_spawn(&sp, "/nonexistent", 0, 0, av2, environ);
        printf("vf: spawn ok=%d fail-rc=%d (ENOENT=%d)\n", ok1, r2, ENOENT);
        if (ok1 && r2 == ENOENT) bits |= 16;
    }

    /* ---- bit5: raw NULL-stack clone (the unshare -f shape) ----
     *
     * WRITTEN CALL-FREE ON THE CHILD SIDE, and that is the lesson this
     * bit taught (diagnosed live, three runs of "every term prints true,
     * the AND is false"): the child shares the parent's stack, so any
     * child CALL at the clone call's depth overwrites the return-address
     * slot the suspended parent will later RET through -- the parent
     * then resumes after the CHILD's call site, silently skipping its
     * own rax->r store, and r keeps the child's 0. glibc's vfork()
     * wrapper pops its return address into a register before the
     * syscall for EXACTLY this reason (which is why bits 0-3 never saw
     * it). Identical behaviour on real Linux; the raw form must use
     * inline asm with a call-free child, as below. */
    {
        volatile int rawlocal = 0;
        register long r10c __asm__("r10") = 0;
        register long r8c  __asm__("r8")  = 0;
        long r;
        __asm__ volatile("syscall"
                         : "=a"(r)
                         : "0"(56L /* SYS_clone */),
                           "D"(0x4100L /* CLONE_VM|CLONE_VFORK */ | 17),
                           "S"(0L), "d"(0L), "r"(r10c), "r"(r8c)
                         : "rcx", "r11", "memory");
        if (r == 0) {
            rawlocal = 77;
            g_raw_marker = 1;
            __asm__ volatile("syscall"
                             :: "a"(60L /* SYS_exit */), "D"(7L)
                             : "rcx", "r11", "memory");
            __builtin_unreachable();
        }
        int st = 0;
        pid_t w = waitpid((pid_t)r, &st, 0);
        printf("vf: rawclone r=%ld w=%d local=%d marker=%d exit=%d\n",
               r, (int)w, rawlocal, g_raw_marker,
               WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        if (r > 0 && w == (pid_t)r && rawlocal == 77 && g_raw_marker == 1 &&
            WIFEXITED(st) && WEXITSTATUS(st) == 7)
            bits |= 32;
    }

    printf("LXVFORK: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
