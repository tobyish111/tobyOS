/* linux-clonens -- clone(2) with CLONE_NEW* flags: creating a CHILD in a new
 * namespace, as opposed to unshare(2) moving the CALLER into one.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS TEST EXISTS, AND WHAT THE FIRST VERSION GOT WRONG
 *
 * The namespace arc (slices 8-12) tests namespaces exclusively through unshare(2).
 * Nothing tested clone(2) with CLONE_NEW* bits -- and that is the form Chromium's
 * sandbox and every container runtime use. The gap was found the expensive way:
 * Chromium, launched non-root by slice 14, built its sandbox and then died with
 *
 *   FATAL:sandbox/linux/services/credentials.cc:309]
 *   Check failed: . : No child processes (10)
 *
 * because its clone(CLONE_NEWUSER|SIGCHLD) child faulted before executing a single
 * syscall (cr2=-0x28, cpu=0ms, syscalls=0) and wait4 then found nothing to reap.
 *
 * THE FIRST VERSION OF THIS TEST PASSED 8/8 AND THEREFORE DISPROVED MY DIAGNOSIS:
 * clone(CLONE_NEWUSER) is NOT broken on its own. So the fault needs whatever else
 * was true of Chromium at that moment. Two differences are testable, and this
 * version isolates them instead of guessing:
 *
 *   - Chromium had ALREADY DROPPED to uid 1000. Every earlier namespace test ran
 *     as root. Unprivileged user-namespace creation is a different authorisation
 *     path (slice 11 hardened it).
 *   - Chromium was MULTI-THREADED. Forking from a multi-threaded process copies
 *     only the calling thread, and this kernel's fork does a CoW clone under a
 *     stop-the-world quiesce -- a live sibling thread is exactly the condition
 *     that makes that interesting.
 *
 * The clone is issued RAW -- syscall(SYS_clone, flags, 0, 0, 0, 0), NULL child
 * stack.
 *
 * CORRECTION (added after this test's 8/8 was read as exonerating the whole
 * namespace path): the sentence that used to end the line above -- "because
 * that is precisely what Chromium's ForkWithFlags does" -- WAS WRONG, and it is
 * the reason this test passed over a real bug. Disassembling the faulting rip in
 * chrome-headless-shell shows `call clone@plt`: Chromium goes through glibc's
 * clone() LIBRARY function, which always supplies a child stack and whose
 * child-side wrapper pops its entry function off it. This kernel's clone arm
 * dropped that argument, so the child popped the PARENT's stack. The raw form
 * tested here is the one case where ignoring child_stack is CORRECT, which is
 * exactly why 8/8 here meant nothing about Chromium.
 *
 * That shape now has its own test: programs/linux-clonestk. What this file
 * proves stands -- CLONE_NEW* on the raw entry point works, as root and as uid
 * 1000, single- and multi-threaded -- it is just not the entry point Chromium
 * uses.
 *
 * bit0 root  + plain clone (THE CONTROL: if this fails nothing else means anything)
 * bit1 root  + CLONE_NEWUSER
 * bit2 root  + NEWUTS/NEWIPC/NEWNS/NEWPID/NEWNET (all must pass)
 * bit3 the NEWUSER child is REALLY unmapped (getuid()==65534), so bit1 cannot pass
 *      by the flag being accepted and quietly ignored
 * bit4 root  + CLONE_NEWUSER with a LIVE SIBLING THREAD
 * bit5 uid1000 + plain clone
 * bit6 uid1000 + CLONE_NEWUSER
 * bit7 uid1000 + CLONE_NEWUSER with a LIVE SIBLING THREAD   <- closest to Chromium
 *
 * Ordering matters: the root cases run first, because the drop is irreversible.
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
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define KID_OK 7
#define KID_NS 9

static volatile int g_thread_park = 1;

static void *sibling(void *arg) {
    (void)arg;
    /* Stay alive and mostly asleep: the point is to exist as a second thread in
     * the address space while the main thread clones, not to contend. */
    while (g_thread_park) usleep(2000);
    return 0;
}

static long raw_clone(unsigned long flags) {
    return syscall(SYS_clone, flags, 0UL, 0UL, 0UL, 0UL);
}

/* What the child does before exiting -- and the reason it is not just _exit().
 *
 * The FIRST version of this test had the child call _exit() immediately, and it
 * passed 8/8 while Chromium's child was still faulting. _exit() reads almost no
 * state: a child whose callee-saved registers, frame pointer or TLS base were
 * mangled on the way back to user mode would sail through it. Chromium's child ran
 * REAL code and faulted at cr2=-0x28 -- a dereference through a base register
 * holding zero, which is exactly the class of damage _exit() cannot see.
 *
 * So the child now: walks a deep call chain (forces rbp/callee-saved use), formats
 * through libc (touches TLS and locale state), and reads its own errno (a TLS
 * access). If any of that is broken the child faults here, in a 60-line test,
 * instead of 60 MB into a browser. */
static int deep(int n, volatile int *sink) {
    char pad[96];
    memset(pad, (char)n, sizeof pad);
    *sink += pad[n & 63];
    if (n <= 0) return *sink & 1;
    return deep(n - 1, sink) + (pad[0] ? 0 : 1);
}

static int child_work(void) {
    volatile int sink = 0;
    char b[80];
    errno = 0;
    (void)deep(24, &sink);                       /* frame pointer + callee-saved */
    int n = snprintf(b, sizeof b, "%d/%s/%u", 42, "clonens", (unsigned)getpid());
    if (n <= 0 || strncmp(b, "42/clonens/", 11) != 0) return 30;
    if (errno != 0) return 31;                   /* TLS read */
    if (getuid() == (uid_t)-1) return 32;
    return KID_OK;
}

/* One case. Returns 1 on success; every failure mode is named separately, because
 * "it didn't work" is what made the Chromium failure take a whole boot to read. */
static int one(const char *name, unsigned long flags) {
    long rc = raw_clone(flags);
    if (rc < 0) {
        printf("clonens: %-22s FAILED -- clone refused, errno=%d\n", name, errno);
        return 0;
    }
    if (rc == 0) _exit(child_work());

    int st = 0;
    pid_t w = waitpid((pid_t)rc, &st, 0);
    if (w < 0) {
        printf("clonens: %-22s FAILED -- waitpid(%ld) errno=%d%s\n", name, rc, errno,
               errno == ECHILD ? "  (ECHILD -- the chromium credentials.cc:309 shape:"
                                 " the child vanished before it could be reaped)" : "");
        return 0;
    }
    if (WIFSIGNALED(st)) {
        printf("clonens: %-22s FAILED -- child KILLED by signal %d (faulted instead "
               "of running)\n", name, WTERMSIG(st));
        return 0;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != KID_OK) {
        printf("clonens: %-22s FAILED -- child exited %d (want %d) status=0x%x\n",
               name, WIFEXITED(st) ? WEXITSTATUS(st) : -1, KID_OK, st);
        return 0;
    }
    printf("clonens: %-22s ok\n", name);
    return 1;
}

/* Same, but with a live sibling thread for the duration. */
static int one_threaded(const char *name, unsigned long flags) {
    pthread_t t;
    g_thread_park = 1;
    if (pthread_create(&t, 0, sibling, 0) != 0) {
        printf("clonens: %-22s FAILED -- pthread_create errno=%d\n", name, errno);
        return 0;
    }
    usleep(20000);                     /* let it reach the park loop */
    int ok = one(name, flags);
    g_thread_park = 0;
    pthread_join(t, 0);
    return ok;
}

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("clonens: start pid=%d uid=%u\n", (int)getpid(), (unsigned)getuid());

    /* ---- root cases (must come first: the drop below is irreversible) ---- */
    if (one("root/plain", 0x11UL))                            bits |= 1;
    if (one("root/NEWUSER", CLONE_NEWUSER | 0x11UL))           bits |= 2;
    {
        int all = 1;
        all &= one("root/NEWUTS", CLONE_NEWUTS | 0x11UL);
        all &= one("root/NEWIPC", CLONE_NEWIPC | 0x11UL);
        all &= one("root/NEWNS",  CLONE_NEWNS  | 0x11UL);
        all &= one("root/NEWPID", CLONE_NEWPID | 0x11UL);
        all &= one("root/NEWNET", CLONE_NEWNET | 0x11UL);
        if (all) bits |= 4;
    }

    /* bit3 -- the flag is honoured, not accepted-and-ignored. A fresh user
     * namespace has no id mapping, so getuid() must report the overflow uid. */
    {
        long rc = raw_clone(CLONE_NEWUSER | 0x11UL);
        if (rc == 0) { if (child_work() != KID_OK) _exit(33);
                       uid_t u = getuid();
                       _exit(u == 65534 ? KID_NS : (u == 0 ? 20 : 21)); }
        int st = 0;
        if (rc > 0 && waitpid((pid_t)rc, &st, 0) > 0 &&
            WIFEXITED(st) && WEXITSTATUS(st) == KID_NS) {
            bits |= 8;
            printf("clonens: NEWUSER child really unmapped (getuid()=65534)\n");
        } else if (rc > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 20) {
            printf("clonens: bit3 FAILED -- child still sees uid 0: CLONE_NEWUSER "
                   "was accepted and IGNORED\n");
        } else {
            printf("clonens: bit3 FAILED -- rc=%ld status=0x%x\n", rc, st);
        }
    }

    /* bit4 -- root, but with a live sibling thread. */
    if (one_threaded("root/NEWUSER+thread", CLONE_NEWUSER | 0x11UL)) bits |= 16;

    /* ---- now drop privileges, as chromewin does before exec'ing chrome ---- */
    if (setgid(1000) != 0)
        printf("clonens: note setgid(1000) errno=%d\n", errno);
    if (setuid(1000) != 0) {
        printf("clonens: could not setuid(1000) errno=%d -- bits 5-7 unreachable\n",
               errno);
    } else {
        printf("clonens: dropped to uid=%u gid=%u (chromium's state)\n",
               (unsigned)getuid(), (unsigned)getgid());
        if (one("uid1000/plain",   0x11UL))                          bits |= 32;
        if (one("uid1000/NEWUSER", CLONE_NEWUSER | 0x11UL))          bits |= 64;
        if (one_threaded("uid1000/NEWUSER+thr", CLONE_NEWUSER | 0x11UL)) bits |= 128;
    }

    printf("clonens: BITS=0x%x (want 0xff)\n", bits);
    return bits;
}
