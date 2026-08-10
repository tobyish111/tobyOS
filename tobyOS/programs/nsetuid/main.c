/* nsetuid -- Phase 3 slice 14: does the NATIVE privilege drop actually work?
 *
 * ---------------------------------------------------------------------------
 * WHY THIS TEST EXISTS, AND WHAT IT CAN AND CANNOT PROVE
 *
 * Slice 14's real blocker is that Chromium refuses to enable its sandbox while
 * running as root, and `chromewin` is a NATIVE app that had no way to stop being
 * root -- the native ABI had no setuid at all. This test covers the half of slice 14
 * that CAN be verified in QEMU: that the new native setuid is a genuine privilege
 * drop.
 *
 * It cannot prove the sandbox works. Every QEMU boot here auto-logs-in as root, so
 * the sandboxed-Chromium path needs the EliteDesk. That is why CW_SANDBOX defaults
 * off and why docs/linux-arc-handoff-phase3.md carries a hardware checklist.
 *
 * THE CHECK THAT MATTERS IS bit3. `getuid()` returning 1000 proves only that a
 * field changed -- a "drop" that updated p->uid while leaving the process able to do
 * root things would pass a naive test and be worse than no drop at all, because it
 * would look safe. So bit3 asserts a ROOT-ONLY OPERATION NOW FAILS.
 *
 * bits: 0 start as root | 1 setuid(1000) succeeds and getuid agrees
 *       2 climb-back to root REFUSED | 3 a root-only syscall now FAILS
 *       4 setgid works the same way   | 5 setuid to an absurd id is rejected
 * exit status is the bitmask; 63 = all six.
 * ---------------------------------------------------------------------------
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define TARGET_UID 1000
#define TARGET_GID 1000

int main(void) {
    int bits = 0;

    /* ---- bit0: we begin as root (the harness auto-logs-in as root) ---- */
    uid_t u0 = getuid();
    if (u0 == 0) { bits |= 1; printf("nsetuid: start uid=0 (root)\n"); }
    else printf("nsetuid: bit0 FAILED -- started as uid=%u, not root; the rest of "
                "this test would be meaningless\n", (unsigned)u0);

    /* ---- bit1: the drop is accepted and visible ---- */
    if (bits & 1) {
        if (setuid(TARGET_UID) != 0) {
            printf("nsetuid: bit1 FAILED -- setuid(%d) errno=%d\n",
                   TARGET_UID, errno);
        } else if (getuid() != TARGET_UID) {
            printf("nsetuid: bit1 FAILED -- setuid returned 0 but getuid()=%u\n",
                   (unsigned)getuid());
        } else {
            bits |= 2;
            printf("nsetuid: setuid(%d) OK, getuid()=%u\n",
                   TARGET_UID, (unsigned)getuid());
        }
    }

    /* ---- bit2: and it is IRREVERSIBLE ---- */
    if (bits & 2) {
        if (setuid(0) == 0) {
            printf("nsetuid: bit2 FAILED -- climbed BACK to root after dropping. "
                   "The drop is decorative and the sandbox would be worthless.\n");
        } else {
            bits |= 4;
            printf("nsetuid: climb-back REFUSED (errno=%d) -- drop is one-way\n",
                   errno);
        }
    }

    /* ---- bit3: THE ONE THAT MATTERS -- a root-only operation now fails ----
     *
     * chown(2) is root-only in this kernel. Before the drop it works on a file we
     * own; after the drop it must not. A uid field that changed while authority
     * did not is the failure mode this bit exists to catch. */
    if (bits & 2) {
        errno = 0;
        int rc = chown("/data", 0, 0);
        int e  = errno;
        if (rc == 0) {
            printf("nsetuid: bit3 FAILED -- chown('/data') SUCCEEDED as uid=%u. "
                   "The uid changed but the authority did not.\n",
                   (unsigned)getuid());
        } else if (e != EACCES && e != EPERM) {
            /* THE ERRNO IS PART OF THE ASSERTION.
             *
             * The first version accepted any failure and passed while reporting
             * errno=12 (ENOMEM) -- because sys_chown returned VFS_ERR_PERM (-12)
             * raw, untranslated. The refusal was right and the errno was nonsense,
             * and "it failed" could not tell the difference. Now it must fail FOR
             * THE RIGHT REASON. */
            printf("nsetuid: bit3 FAILED -- chown refused with errno=%d, want "
                   "EACCES(%d) or EPERM(%d). A permission denial reported as "
                   "something else sends a debugger the wrong way.\n",
                   e, EACCES, EPERM);
        } else {
            bits |= 8;
            printf("nsetuid: root-only chown now REFUSED with errno=%d "
                   "(EACCES/EPERM) -- authority really dropped, not just the uid "
                   "field\n", e);
        }
    }

    /* ---- bit4: setgid behaves the same way ---- */
    {
        /* Already non-root by now, so this asserts the UNPRIVILEGED rule: a
         * non-root process may not switch to an arbitrary gid. */
        if (setgid(TARGET_GID + 7) == 0) {
            printf("nsetuid: bit4 FAILED -- non-root setgid(%d) was allowed\n",
                   TARGET_GID + 7);
        } else {
            bits |= 16;
            printf("nsetuid: non-root setgid REFUSED (errno=%d)\n", errno);
        }
    }

    /* ---- bit5: an unmapped/absurd id is rejected, not silently substituted ----
     *
     * Slice 11's rule: an id with no user-namespace mapping is EINVAL. Substituting
     * "some other id" would be a privilege bug rather than a rounding error. */
    {
        if (setuid(0x7ffffffe) == 0) {
            printf("nsetuid: bit5 FAILED -- setuid(0x7ffffffe) accepted\n");
        } else {
            bits |= 32;
            printf("nsetuid: absurd uid rejected (errno=%d)\n", errno);
        }
    }

    printf("nsetuid: BITS=0x%x (want 0x3f)\n", bits);
    return bits;
}
