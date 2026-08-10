/* seccomp.h -- seccomp-bpf (Phase 3 slice 13).
 *
 * The last thing slice 14 (Chromium without --no-sandbox) waits on. Chromium
 * installs a cBPF filter in every renderer after the namespaces are set up; with
 * seccomp(2) returning -ENOSYS its sandbox refuses to start.
 *
 * The plan called this "more tractable than it sounds", and that is right for a
 * specific reason: classic BPF is a 1990s-era fixed-width instruction set with no
 * loops, no backward jumps and no memory beyond 16 scratch words, so a verifier
 * and an interpreter are both small and both total. The whole thing is decidable
 * by inspection -- which is exactly why seccomp uses cBPF rather than eBPF for
 * the filter language.
 */
#ifndef TOBYOS_SECCOMP_H
#define TOBYOS_SECCOMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct proc;

/* prctl options (include/uapi/linux/prctl.h). */
#define PR_GET_SECCOMP        21
#define PR_SET_SECCOMP        22
#define PR_SET_NO_NEW_PRIVS   38
#define PR_GET_NO_NEW_PRIVS   39

/* seccomp(2) operations. */
#define SECCOMP_SET_MODE_STRICT      0
#define SECCOMP_SET_MODE_FILTER      1
#define SECCOMP_GET_ACTION_AVAIL     2

/* Modes, as PR_GET_SECCOMP reports them. */
#define SECCOMP_MODE_DISABLED  0
#define SECCOMP_MODE_STRICT    1
#define SECCOMP_MODE_FILTER    2

/* seccomp(2) flags. */
#define SECCOMP_FILTER_FLAG_TSYNC        (1u << 0)
#define SECCOMP_FILTER_FLAG_LOG          (1u << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW   (1u << 2)

/* Return actions. NOTE THE ORDERING RULE: when several filters are installed,
 * every one runs and the NUMERICALLY LOWEST return wins. That is what makes the
 * action values look arbitrary -- they are chosen so "more restrictive" sorts
 * lower, and it is why a filter can never be loosened by adding another. */
#define SECCOMP_RET_KILL_PROCESS  0x80000000u
#define SECCOMP_RET_KILL_THREAD   0x00000000u
#define SECCOMP_RET_TRAP          0x00030000u
#define SECCOMP_RET_ERRNO         0x00050000u
#define SECCOMP_RET_USER_NOTIF    0x7fc00000u
#define SECCOMP_RET_TRACE         0x7ff00000u
#define SECCOMP_RET_LOG           0x7ffc0000u
#define SECCOMP_RET_ALLOW         0x7fff0000u
#define SECCOMP_RET_ACTION_FULL   0xffff0000u
#define SECCOMP_RET_DATA          0x0000ffffu

/* What the filter program sees. Layout is ABI -- offsets are hard-coded by
 * every filter generator in existence (libseccomp, Chromium's own, minijail). */
struct seccomp_data {
    int32_t  nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

#define AUDIT_ARCH_X86_64 0xC000003Eu
#define BPF_MAXINSNS      4096

/* One classic-BPF instruction. */
struct sock_filter {
    uint16_t code;
    uint8_t  jt;
    uint8_t  jf;
    uint32_t k;
};

/* Install a filter from a userspace `struct sock_fprog *`. Returns 0 or a
 * negative ABI errno. */
long seccomp_set_mode_filter(uint64_t uprog, uint32_t flags);
long seccomp_set_mode_strict(void);
long seccomp_get_action_avail(uint64_t uaction);

/* prctl(PR_SET_SECCOMP, ...) -- the older entry point Chromium still uses on
 * some paths. */
long seccomp_prctl_set(long which, uint64_t arg);

/* THE HOOK. Called at the head of every Linux syscall for a filtered process.
 * Returns:
 *    0                -> allow, carry on
 *    negative errno   -> return that to userspace WITHOUT running the syscall
 * and may not return at all (a KILL action terminates the process). */
long seccomp_check(struct proc *p, long nr, long a1, long a2, long a3,
                   long a4, long a5, long a6);

/* Lifecycle: filters are refcounted and shared across fork (a child inherits
 * its parent's filters and cannot escape them). */
void seccomp_fork_inherit(struct proc *child);
void seccomp_release(struct proc *p);

#endif /* TOBYOS_SECCOMP_H */
