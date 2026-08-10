/* linux-alpinerun -- Linux slice 7: run an ALPINE binary as a NON-ROOT user.
 *
 * This is the milestone's second half, made explicit rather than delegated to
 * busybox `su`. Doing the transition ourselves means the proof states exactly
 * what happened, in order, and cannot be satisfied by a fallback:
 *
 *   1. chroot("/data/alp")      -- the Alpine rootfs becomes /
 *   2. chdir("/")               -- required; chroot alone leaves cwd outside
 *   3. setgid + setuid(65534)   -- a PERMANENT, irreversible drop (slice 2)
 *   4. execve("/bin/busybox", {"id"})
 *
 * Step 4 is the interesting one: after step 1 that path names ALPINE's
 * busybox, and after step 3 the caller is unprivileged -- so a success proves
 * chroot, the credential machinery, and exec-honours-chroot all at once.
 *
 * The child's own `id` output is the evidence; the parent re-checks the exit
 * status so a silent failure cannot read as a pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/syscall.h>

#define JAIL   "/data/alp"
#define NOBODY 65534

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    pid_t pid = fork();
    if (pid == 0) {
        if (chroot(JAIL) != 0) {
            printf("[ALPRUN] chroot failed errno=%d\n", errno);
            _exit(101);
        }
        if (chdir("/") != 0) {
            printf("[ALPRUN] chdir failed errno=%d\n", errno);
            _exit(102);
        }
        /* Drop group first, then user: after setuid(2) we are unprivileged
         * and setgid would be refused. */
        if (syscall(SYS_setgid, NOBODY) != 0) {
            printf("[ALPRUN] setgid failed errno=%d\n", errno);
            _exit(103);
        }
        if (syscall(SYS_setuid, NOBODY) != 0) {
            printf("[ALPRUN] setuid failed errno=%d\n", errno);
            _exit(104);
        }
        /* Sanity: the drop must be real before we hand off. */
        if (getuid() != NOBODY || geteuid() != NOBODY) {
            printf("[ALPRUN] still privileged: uid=%d euid=%d\n",
                   (int)getuid(), (int)geteuid());
            _exit(105);
        }
        printf("[ALPRUN] dropped to uid=%d, exec'ing ALPINE /bin/busybox id\n",
               (int)getuid());
        char *av[] = { (char *)"id", 0 };
        char *ev[] = { (char *)"PATH=/bin:/sbin:/usr/bin", 0 };
        execve("/bin/busybox", av, ev);
        printf("[ALPRUN] execve failed errno=%d\n", errno);
        _exit(106);
    }
    if (pid < 0) { printf("[ALPRUN] fork failed\n"); return 1; }

    int st = 0;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    printf("[ALPRUN] child exit=%d\n", code);
    if (code != 0) {
        printf("[ALPRUN] VERDICT: FAIL\n");
        return 1;
    }
    printf("[ALPRUN] VERDICT: PASS -- alpine binary ran as non-root in chroot\n");
    return 0;
}
