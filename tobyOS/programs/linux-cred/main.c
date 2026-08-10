/* linux-cred -- Linux slice 2 acceptance test: POSIX credentials.
 *
 * Runs as root and walks the credential state machine, checking both that the
 * allowed transitions work AND that the forbidden ones are refused. The
 * refusals matter more than the successes: a setuid implementation that lets
 * you climb back up is worse than none at all, because callers rely on a
 * privilege drop actually dropping.
 *
 * Each check sets a bit; all pass => exit 63.
 *
 *   bit0  getresuid/getresgid report the real/effective/saved triple
 *   bit1  seteuid-style drop via setresuid, then RESTORE from saved
 *   bit2  getgroups/setgroups round-trip
 *   bit3  capget reports a capability set and rejects a bad version
 *   bit4  PERMANENT drop: setuid(N) as root sets all three
 *   bit5  after the permanent drop, climbing back to root is REFUSED
 *
 * bit5 is the security-critical one and is deliberately last: everything
 * before it runs while still privileged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/syscall.h>
#include <grp.h>

#define NOBODY 65534

static int getres_u(unsigned *r, unsigned *e, unsigned *s) {
    return (int)syscall(SYS_getresuid, r, e, s);
}

int main(void) {
    int bits = 0;
    unsigned r, e, s;

    /* ---- bit0: the triple is reported ---- */
    if (getres_u(&r, &e, &s) == 0) {
        printf("cred: start ruid=%u euid=%u suid=%u\n", r, e, s);
        if (r == 0 && e == 0 && s == 0) bits |= 1;
    }

    /* ---- bit1: temporary drop, then restore from the saved id ----
     * setresuid(-1, NOBODY, -1) leaves real and saved at 0, so root is
     * recoverable -- this is the "drop privilege for a moment" idiom. */
    if (syscall(SYS_setresuid, (unsigned)-1, NOBODY, (unsigned)-1) == 0 &&
        getres_u(&r, &e, &s) == 0 && r == 0 && e == NOBODY && s == 0) {
        /* Now climb back: allowed, because saved uid is still 0. */
        if (syscall(SYS_setresuid, (unsigned)-1, 0u, (unsigned)-1) == 0 &&
            getres_u(&r, &e, &s) == 0 && e == 0) {
            bits |= 2;
            printf("cred: temp drop + restore OK\n");
        }
    }

    /* ---- bit2: supplementary groups round-trip ---- */
    {
        gid_t set[3] = { 10, 20, 30 };
        gid_t got[8];
        if (setgroups(3, set) == 0) {
            int ng = getgroups(8, got);
            if (ng == 3 && got[0] == 10 && got[1] == 20 && got[2] == 30) {
                bits |= 4;
                printf("cred: groups round-trip OK (%d)\n", ng);
            } else {
                printf("cred: groups readback ng=%d\n", ng);
            }
        } else {
            printf("cred: setgroups failed errno=%d\n", errno);
        }
    }

    /* ---- bit3: capget speaks the v3 ABI and rejects a bad version ---- */
    {
        struct { unsigned version; int pid; } hdr;
        struct { unsigned eff, perm, inh; } data[2];
        hdr.version = 0x20080522u; hdr.pid = 0;
        int ok1 = (syscall(SYS_capget, &hdr, data) == 0);
        /* A bogus version must fail AND report the supported one back. */
        hdr.version = 0xdeadbeefu;
        int ok2 = (syscall(SYS_capget, &hdr, data) != 0 &&
                   hdr.version == 0x20080522u);
        if (ok1 && ok2) {
            bits |= 8;
            printf("cred: capget v3 OK (eff=0x%x) + version negotiation OK\n",
                   data[0].eff);
        } else {
            printf("cred: capget ok1=%d ok2=%d\n", ok1, ok2);
        }
    }

    /* ---- bit4: PERMANENT drop. As root, setuid(N) sets all three. ---- */
    if (syscall(SYS_setuid, NOBODY) == 0 &&
        getres_u(&r, &e, &s) == 0) {
        printf("cred: after setuid(%d): ruid=%u euid=%u suid=%u\n",
               NOBODY, r, e, s);
        if (r == NOBODY && e == NOBODY && s == NOBODY) bits |= 16;
    }

    /* ---- bit5: the drop must be IRREVERSIBLE ----
     * Every route back to uid 0 has to be refused now that no id in the
     * triple is 0. If any of these succeeds the whole model is broken. */
    {
        int back1 = (int)syscall(SYS_setuid, 0u);
        int back2 = (int)syscall(SYS_setresuid, 0u, 0u, 0u);
        int back3 = (int)syscall(SYS_setreuid, 0u, 0u);
        if (back1 != 0 && back2 != 0 && back3 != 0) {
            if (getres_u(&r, &e, &s) == 0 && r == NOBODY && e == NOBODY) {
                bits |= 32;
                printf("cred: climb-back REFUSED (setuid=%d setresuid=%d "
                       "setreuid=%d) -- drop is irreversible\n",
                       back1, back2, back3);
            }
        } else {
            printf("cred: SECURITY FAIL -- climbed back to root! "
                   "(%d %d %d)\n", back1, back2, back3);
        }
    }

    printf("LXCRED: VERDICT bits=%d (63=all)\n", bits);
    fflush(stdout);
    return bits;
}
