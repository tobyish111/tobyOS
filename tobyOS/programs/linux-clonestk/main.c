/* linux-clonestk -- clone(2) with a CHILD STACK, i.e. glibc's clone() LIBRARY
 * function, which is a different shape from the raw syscall and was never tested.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS TEST EXISTS
 *
 * `clone` has two callers with different requirements, and this arc tested only
 * one of them:
 *
 *   syscall(SYS_clone, flags, NULL, 0,0,0)   -- raw, NULL child stack.
 *       The child resumes on the parent's stack at the instruction after the
 *       syscall, exactly like fork(2). programs/linux-clonens covers this and
 *       it has always worked.
 *
 *   clone(fn, child_stack, flags, arg)       -- the glibc LIBRARY function.
 *       __clone stores fn and arg at the top of child_stack, issues the syscall
 *       with the adjusted pointer as a2, and the CHILD side of the wrapper is:
 *
 *           xor  %ebp, %ebp          <- child's frame pointer is deliberately 0
 *           pop  %rax                <- fn,  off the CHILD STACK
 *           pop  %rdi                <- arg, off the CHILD STACK
 *           call *%rax
 *
 *       so if the kernel ignores a2 the child runs those pops against the
 *       PARENT's stack. %rax then holds the return address of the caller's
 *       `call clone@plt`, the child "calls" into the middle of its parent's
 *       caller with %rbp == 0, and the first frame-relative access dies.
 *
 * tobyOS's clone arm dropped a2 on the floor, and the second form is what
 * Chromium's sandbox uses. Its child died at cr2 = 0xffffffffffffffd8 (= -0x28,
 * the stack-canary reload `cmp -0x28(%rbp),%rcx` with %rbp == 0) having executed
 * ZERO syscalls, so wait4 found nothing to reap and the sandbox CHECKed out with
 *
 *     FATAL:sandbox/linux/services/credentials.cc:309]
 *     Check failed: . : No child processes (10)
 *
 * A previous session concluded from linux-clonens passing 8/8 that the namespace
 * path was exonerated and the fault must lie in CoW-forking chrome's 327-VMA
 * address space. The first half was right and the second was wrong: linux-clonens
 * passes because it uses the RAW form, for which "resume where the parent was" is
 * the CORRECT behaviour. Nothing about chrome's size or its VMAs is involved.
 *
 * ---------------------------------------------------------------------------
 * WHAT EACH BIT ASSERTS, AND WHY IT IS NOT REDUNDANT
 *
 * bit0 CONTROL: clone(fn, stack, SIGCHLD, arg) -- no namespaces at all. If this
 *      fails, the bug is in child-stack handling generally and has nothing to do
 *      with namespaces. Everything below is meaningless without it.
 * bit1 the child really ran ON THE SUPPLIED STACK: its own frame address must lie
 *      inside the buffer we passed. "The child ran" is NOT the assertion -- a
 *      kernel that pointed the child at some other valid stack would pass that
 *      and still be wrong. The VALUE is the assertion.
 * bit2 `arg` arrived intact -- that is the SECOND pop, so it proves the child
 *      stack was right for the whole prologue and not just the first word.
 * bit3 CLONE_NEWUSER + a child stack: Chromium's exact call shape.
 * bit4 that NEWUSER child is really unmapped (getuid() == 65534), so bit3 cannot
 *      pass by the flag being accepted and quietly ignored.
 * bit5 the child does REAL work on that stack: deep recursion (forces frame use
 *      well past one page), libc formatting, and a TLS read. The lesson is
 *      borrowed from linux-clonens: a child that only calls _exit() reads almost
 *      no state and would sail through a mangled frame pointer.
 * bit6 waitpid() reaps it and does NOT return ECHILD -- literally the check
 *      Chromium fails at credentials.cc:309. This is the end-to-end shape.
 * bit7 all of it again after dropping to uid 1000, which is the state chromewin
 *      leaves chrome in -- AND with Chromium's real second flag set,
 *      CLONE_NEWUSER|CLONE_NEWPID|CLONE_NEWNET (0x70000011), which is the only
 *      combination unprivileged containers ever use. Two cases on one bit
 *      because the mask is 8 bits wide and both are the same claim ("an
 *      unprivileged process may build a container"); each prints its own named
 *      line, so a failure still says which half broke.
 *
 *      That second case found a second bug, of exactly the same species as the
 *      first: nsproxy_apply_clone_flags measured CAP_SYS_ADMIN against the
 *      PARENT's user namespace even when CLONE_NEWUSER was in the same request,
 *      so the combination was EPERM for anyone but root. unshare(2) had always
 *      been right. Two entry points to one feature, one of them wrong -- again.
 *

 * exit status is the bitmask; 255 = all eight.
 * ---------------------------------------------------------------------------
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <sys/wait.h>

#define KID_OK      7    /* child ran, on the right stack, with the right arg */
#define KID_NS      9    /* ...and observed itself unmapped in a new user ns  */

#define E_ARGPTR   30    /* arg pointer did not survive -> second pop was wrong */
#define E_MAGIC    31    /* arg pointed somewhere, but not at our struct        */
#define E_STACK    32    /* child frame is NOT inside the buffer we supplied    */
#define E_WORK     33    /* deep recursion / snprintf failed                    */
#define E_ERRNO    34    /* TLS read came back wrong                            */
#define E_UID      35    /* getuid() failed outright                            */
#define E_NS_ROOT  20    /* NEWUSER accepted and IGNORED: child still sees uid 0 */
#define E_NS_OTHER 21    /* NEWUSER child sees some third uid                   */
#define E_GK_FORK  36    /* the child's OWN fork() was refused                   */
#define E_GK_WAIT  37    /* ...forked, but waitpid could not reap it            */
#define E_GK_STAT  38    /* ...reaped, but it did not run correctly             */

#define MAGIC 0x5354414b4944ULL   /* "STAKID" */

/* 256 KiB: comfortably more than the deep() chain below needs, and large enough
 * that "the child landed inside it" is not something a wrong pointer hits by
 * accident. In BSS rather than on main's stack so the bounds are far away from
 * the parent's own frames -- if the child wrongly resumed on the PARENT stack,
 * the bounds check must not accidentally pass. */
#define STK_SZ (256u * 1024u)
static char g_stack[STK_SZ] __attribute__((aligned(64)));

struct kidinfo {
    uint64_t magic;
    uintptr_t lo, hi;      /* [lo, hi) -- the stack we handed the child */
    int       want_nsuid;  /* also report on the user-namespace mapping */
};
static struct kidinfo g_info;

/* Real work, so a child with a damaged frame pointer or TLS base fails HERE, in
 * a 100-line test, rather than 60 MB into a browser. Deliberately deep enough to
 * walk several pages of the supplied stack -- a child running on a stack that is
 * merely valid but too small would fault instead of returning a wrong answer. */
static int deep(int n, volatile int *sink) {
    char pad[96];
    memset(pad, (char)n, sizeof pad);
    *sink += pad[n & 63];
    if (n <= 0) return *sink & 1;
    return deep(n - 1, sink) + (pad[0] ? 0 : 1);
}

static int kid(void *arg) {
    volatile int local = 0;
    uintptr_t sp = (uintptr_t)&local;
    struct kidinfo *ki = (struct kidinfo *)arg;
    char b[80];

    if (!ki)                 return E_ARGPTR;
    if (ki->magic != MAGIC)  return E_MAGIC;
    if (sp < ki->lo || sp >= ki->hi) {
        /* Print it: the number is the diagnosis. An sp far ABOVE hi means the
         * child resumed on the parent's stack (a2 ignored); anything else means
         * it was pointed somewhere new but wrong. */
        printf("clonestk:   child sp=%p OUTSIDE supplied stack [%p,%p)\n",
               (void *)sp, (void *)ki->lo, (void *)ki->hi);
        return E_STACK;
    }

    {
        volatile int sink = 0;
        errno = 0;
        (void)deep(40, &sink);
        int n = snprintf(b, sizeof b, "%d/%s/%u", 42, "clonestk",
                         (unsigned)getpid());
        if (n <= 0 || strncmp(b, "42/clonestk/", 12) != 0) return E_WORK;
        if (errno != 0)                                    return E_ERRNO;
    }

    uid_t u = getuid();
    if (u == (uid_t)-1) return E_UID;

    /* THE CHILD'S OWN fork() MUST STILL BE ORDINARY.
     *
     * The kernel stages child_stack on the CALLING process and consumes it
     * inside the fork; the child is built by a whole-PCB memcpy, so a staging
     * field that is not explicitly reset arrives in the child STILL ARMED. Its
     * next plain fork() then hands the grandchild an RSP belonging to a
     * different program -- and after an execve that address means nothing at
     * all. This exact leak shipped in the first version of the fix and was
     * caught by the container capstone rather than here, which is the wrong way
     * round: the leak belongs to clone, so its test should own it.
     *
     * A grandchild whose RSP is wrong dies on its first push, so "it exited 0"
     * is a sufficient assertion here. */
    {
        pid_t g = fork();
        if (g < 0) {
            printf("clonestk:   grandchild: fork() refused, errno=%d\n", errno);
            return E_GK_FORK;
        }
        if (g == 0) {
            volatile int s = 0;
            (void)deep(8, &s);          /* touch a real stack, several frames */
            _exit(23);
        }
        int gst = 0;
        pid_t gw = waitpid(g, &gst, 0);
        if (gw != g) {
            printf("clonestk:   grandchild: fork()=%d but waitpid returned %d "
                   "errno=%d\n", (int)g, (int)gw, errno);
            return E_GK_WAIT;
        }
        if (!WIFEXITED(gst) || WEXITSTATUS(gst) != 23) {
            printf("clonestk:   grandchild: pid=%d status=0x%x "
                   "(exited=%d code=%d, want 23)\n", (int)g, gst,
                   WIFEXITED(gst), WIFEXITED(gst) ? WEXITSTATUS(gst) : -1);
            return E_GK_STAT;
        }
    }

    if (ki->want_nsuid) {
        if (u == 65534) return KID_NS;
        return (u == 0) ? E_NS_ROOT : E_NS_OTHER;
    }
    return KID_OK;
}

static const char *why(int code) {
    switch (code) {
    case E_ARGPTR:   return "arg pointer was NULL in the child "
                            "(clone's second pop read the wrong stack)";
    case E_MAGIC:    return "arg pointed at the wrong memory";
    case E_STACK:    return "child did NOT run on the supplied stack";
    case E_WORK:     return "deep recursion / snprintf failed on that stack";
    case E_ERRNO:    return "errno (TLS) read wrong in the child";
    case E_UID:      return "getuid() failed in the child";
    case E_NS_ROOT:  return "CLONE_NEWUSER was accepted and IGNORED "
                            "(child still sees uid 0)";
    case E_NS_OTHER: return "child in the new user ns sees an unexpected uid";
    case E_GK_FORK:  return "the child's own fork() was REFUSED";
    case E_GK_WAIT:  return "the child forked but could not reap its own "
                            "child (a pid-translation or reparenting problem, "
                            "not a stack one)";
    case E_GK_STAT:  return "the child's grandchild ran wrong -- the classic "
                            "shape is clone's staged child_stack leaking into "
                            "the child through the PCB copy";
    default:         return "unexpected child exit code";
    }
}

/* A separate stack for the CLONE_VM cases: with a shared address space the
 * child writes to the SAME memory the parent can see, so reusing g_stack would
 * let one case's leftovers explain the next one's result. */
static char g_stack2[STK_SZ] __attribute__((aligned(64)));

/* Run one case through the LIBRARY clone(). Returns the child's exit code, or
 * -1 if it never produced one. Every failure mode is named separately: "it
 * didn't work" is what made the Chromium failure cost a whole boot to read. */
static int one(const char *name, int flags, int want_nsuid, int want)
{
    g_info.magic      = MAGIC;
    g_info.lo         = (uintptr_t)g_stack;
    g_info.hi         = (uintptr_t)g_stack + STK_SZ;
    g_info.want_nsuid = want_nsuid;

    /* The top of the buffer. glibc's __clone subtracts 16 from this for fn/arg
     * and hands the result to the kernel as child_stack. */
    void *top = (void *)(g_stack + STK_SZ);

    pid_t pid = clone(kid, top, flags, &g_info);
    if (pid < 0) {
        printf("clonestk: %-24s FAILED -- clone refused, errno=%d\n",
               name, errno);
        return -1;
    }

    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    if (w != pid) {
        printf("clonestk: %-24s FAILED -- waitpid(%d) returned %d errno=%d%s\n",
               name, (int)pid, (int)w, errno,
               errno == ECHILD
                 ? "   <-- ECHILD: THE credentials.cc:309 SHAPE. The child was"
                   " created and then died before it could be reaped."
                 : "");
        return -1;
    }
    if (WIFSIGNALED(st)) {
        printf("clonestk: %-24s FAILED -- child KILLED by signal %d "
               "(it faulted instead of running)\n", name, WTERMSIG(st));
        return -1;
    }
    if (!WIFEXITED(st)) {
        printf("clonestk: %-24s FAILED -- child did not exit, status=0x%x\n",
               name, st);
        return -1;
    }
    int code = WEXITSTATUS(st);
    if (code != want) {
        printf("clonestk: %-24s FAILED -- child exited %d (want %d): %s\n",
               name, code, want, why(code));
        return code;
    }
    printf("clonestk: %-24s ok   (child exit=%d)\n", name, code);
    return code;
}

int main(void)
{
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("clonestk: start pid=%d uid=%u stack=[%p,%p)\n",
           (int)getpid(), (unsigned)getuid(),
           (void *)g_stack, (void *)(g_stack + STK_SZ));

    /* bits 0/1/2/5/6 all ride on this one call, because they are properties of
     * the SAME child: it ran, it ran on our stack, it got our arg, it did real
     * work there, and it was reapable. Splitting them into separate clones
     * would test five different children and prove less. */
    {
        int code = one("plain + child stack", SIGCHLD, 0, KID_OK);
        if (code == KID_OK) {
            bits |= 1;    /* bit0 control: the child ran at all      */
            bits |= 2;    /* bit1 on the supplied stack (E_STACK)    */
            bits |= 4;    /* bit2 with our arg    (E_ARGPTR/E_MAGIC) */
            bits |= 32;   /* bit5 real work there (E_WORK/E_ERRNO)   */
            bits |= 64;   /* bit6 waitpid reaped it, no ECHILD       */
        }
    }

    /* THE SHARE ARM. clone(CLONE_VM|CLONE_VFORK|SIGCHLD) with an explicit
     * stack goes down a completely different kernel path from everything above
     * -- sys_fork_share rather than sys_fork -- and that path maps the child a
     * PRIVATE stack of its own. For a plain vfork() (no stack supplied) that is
     * a deliberate, documented deviation. For this call it is fatal in exactly
     * the same way as the CoW arm was: __clone's child pops its entry function
     * off the stack it was GIVEN, and a fresh zeroed page makes that `call *0`.
     *
     * Chromium's ChrootToSafeEmptyDir is precisely this call, and once the CoW
     * arm was fixed the sandbox got one step further and died here instead
     * (credentials.cc:118, the same "No child processes" it used to hit at
     * :309). Not bitted separately -- it is part of the same claim as bit0,
     * that a supplied child stack is honoured -- but it is named and it is
     * fatal, because bits 0/1/2/5/6 are cleared if it fails.
     *
     * NOTE the child here shares the address space, so it must NOT clobber the
     * parent's view: it runs on g_stack2 and only reads g_info. */
    {
        g_info.magic = MAGIC;
        g_info.lo = (uintptr_t)g_stack2;
        g_info.hi = (uintptr_t)g_stack2 + STK_SZ;
        g_info.want_nsuid = 0;
        pid_t p = clone(kid, (void *)(g_stack2 + STK_SZ),
                        CLONE_VM | CLONE_VFORK | SIGCHLD, &g_info);
        int st = 0, good = 0;
        if (p < 0) {
            printf("clonestk: %-24s FAILED -- clone refused, errno=%d\n",
                   "VM|VFORK + child stack", errno);
        } else if (waitpid(p, &st, 0) != p) {
            printf("clonestk: %-24s FAILED -- waitpid errno=%d%s\n",
                   "VM|VFORK + child stack", errno,
                   errno == ECHILD ? "   <-- ECHILD, the credentials.cc shape"
                                   : "");
        } else if (WIFSIGNALED(st)) {
            printf("clonestk: %-24s FAILED -- child KILLED by signal %d\n",
                   "VM|VFORK + child stack", WTERMSIG(st));
        } else if (!WIFEXITED(st) || WEXITSTATUS(st) != KID_OK) {
            printf("clonestk: %-24s FAILED -- child exited %d (want %d): %s\n",
                   "VM|VFORK + child stack",
                   WIFEXITED(st) ? WEXITSTATUS(st) : -1, KID_OK,
                   why(WIFEXITED(st) ? WEXITSTATUS(st) : -1));
        } else {
            printf("clonestk: %-24s ok   (child exit=%d)\n",
                   "VM|VFORK + child stack", WEXITSTATUS(st));
            good = 1;
        }
        if (!good) bits &= ~(1 | 2 | 4 | 32 | 64);
    }

    /* bit3 -- Chromium's exact shape. */
    if (one("NEWUSER + child stack", CLONE_NEWUSER | SIGCHLD, 0, KID_OK)
        == KID_OK)
        bits |= 8;

    /* bit4 -- and the flag was honoured, not accepted-and-ignored. A fresh user
     * namespace has no id map, so the child must report the overflow uid. */
    if (one("NEWUSER child unmapped", CLONE_NEWUSER | SIGCHLD, 1, KID_NS)
        == KID_NS)
        bits |= 16;

    /* bit7 -- now as uid 1000, which is where chromewin leaves chrome. Last,
     * because the drop is irreversible. */
    if (setgid(1000) != 0)
        printf("clonestk: note setgid(1000) errno=%d\n", errno);
    if (setuid(1000) != 0) {
        printf("clonestk: could not setuid(1000) errno=%d -- bit7 unreachable\n",
               errno);
    } else {
        printf("clonestk: dropped to uid=%u gid=%u (chromium's state)\n",
               (unsigned)getuid(), (unsigned)getgid());
        int a = (one("uid1000/NEWUSER+stack", CLONE_NEWUSER | SIGCHLD, 0,
                     KID_OK) == KID_OK);
        /* Chromium's second clone, verbatim: flags 0x70000011. An unprivileged
         * caller asking for a user namespace AND the namespaces that user
         * namespace is meant to authorise. */
        int b = (one("uid1000/USER+PID+NET", CLONE_NEWUSER | CLONE_NEWPID |
                                             CLONE_NEWNET | SIGCHLD, 0, KID_OK)
                 == KID_OK);
        if (a && b) bits |= 128;
    }

    printf("clonestk: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
