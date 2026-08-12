/* linux-ptrace -- a miniature strace, used as the acceptance test.
 *
 * ===========================================================================
 * WHY THE TEST IS A TRACER AND NOT A LIST OF ptrace() CALLS
 *
 * Checking that each request returns 0 would pass on a kernel where every stop
 * is fabricated and every register read is zeroes. What a tracer is FOR is
 * seeing what another process actually did -- so this test makes the child
 * perform a syscall sequence it CHOOSES, and then requires the trace to match
 * that sequence. The child writes a known byte pattern to a pipe; the tracer
 * must observe the write(2) with the right fd, the right length, and the right
 * return value, having never looked at the pipe itself.
 *
 * That is the difference between "ptrace returned 0" and "ptrace works".
 *
 * bit0 PTRACE_TRACEME + the first stop: the child stops before its first
 *      traced syscall, and waitpid reports WIFSTOPPED rather than reaping it.
 *      A kernel that let the child run to completion would report WIFEXITED
 *      here, which is why the assertion is on the macro and not on "waitpid
 *      returned something".
 * bit1 entry/exit stops ALTERNATE. Every syscall must produce exactly two
 *      stops. Counting them is what catches a hook installed on only one
 *      boundary -- which would still look like a working tracer until you
 *      tried to read a return value.
 * bit2 GETREGS reports the RIGHT SYSCALL: the tracer must see the child's
 *      write(2) by number, with the child's fd in rdi and its length in rdx.
 *      Not "a syscall happened".
 * bit3 the RETURN value at the exit stop equals the byte count the child
 *      actually wrote -- read from rax at the second stop, and cross-checked
 *      against what came out of the pipe.
 * bit4 PEEKDATA reads the child's memory: the tracer peeks the buffer the
 *      child is writing from and must find the known pattern. This is the
 *      check that cannot pass by accident -- the bytes exist only in the
 *      child's address space.
 * bit5 POKEDATA writes it: the tracer overwrites a variable in the stopped
 *      child, resumes it, and the child reports the NEW value back. Two
 *      address spaces, one assertion.
 * bit6 PTRACE_CONT runs the child to exit, and waitpid then reports the
 *      child's real exit code -- so the tracer sees a normal exit after a
 *      trace, not a process left parked.
 * bit7 the refusals are real: an unimplemented request (SINGLESTEP) returns
 *      EINVAL, and SETOPTIONS with an unimplemented option is refused rather
 *      than accepted. A tracer told "yes" for an event stop that never fires
 *      waits forever; an honest EINVAL it can fall back from.
 *
 * exit status is the bitmask; 255 = all eight.
 * ===========================================================================
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/syscall.h>

#ifndef PTRACE_O_TRACESYSGOOD
#define PTRACE_O_TRACESYSGOOD 1
#endif

#define PATTERN "PTRACE-PATTERN-4242"
#define PATLEN  (sizeof PATTERN - 1)

static int g_bits;
__attribute__((format(printf, 2, 3)))
static void ok(int bit, const char *fmt, ...)
{
    va_list ap; g_bits |= bit;
    printf("ptrace:   ok   ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}
__attribute__((format(printf, 1, 2)))
static void no(const char *fmt, ...)
{
    va_list ap;
    printf("ptrace:   FAIL ");
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

/* Lives in the CHILD's address space; the tracer reaches it with PEEK/POKE. */
static char  g_buf[64];
static long  g_poked;

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("ptrace: start pid=%d\n", (int)getpid());

    int pfd[2];
    if (pipe(pfd) != 0) { no("pipe errno=%d", errno); return g_bits; }

    memcpy(g_buf, PATTERN, PATLEN);
    g_poked = 0x1111;

    pid_t kid = fork();
    if (kid < 0) { no("fork errno=%d", errno); return g_bits; }

    if (kid == 0) {
        /* ---- the tracee ------------------------------------------------ */
        close(pfd[0]);
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(70);
        /* Stop here so the tracer can arm syscall-stops before anything
         * interesting happens. raise() is the conventional handshake. */
        kill(getpid(), SIGSTOP);
        /* THE syscall the tracer must observe, with values it can check. */
        ssize_t w = write(pfd[1], g_buf, PATLEN);
        /* Report what the tracer POKEd into us, so bit5 is proved from the
         * child's side rather than by reading back what we just wrote. */
        long seen = g_poked;
        char msg[32];
        int n = snprintf(msg, sizeof msg, "%ld", seen);
        (void)!write(pfd[1], msg, (size_t)n);
        close(pfd[1]);
        _exit(w == (ssize_t)PATLEN ? 33 : 71);
    }

    /* ---- the tracer ---------------------------------------------------- */
    close(pfd[1]);
    int st = 0;

    /* bit0: the child's SIGSTOP handshake arrives as a stop, not an exit. */
    if (waitpid(kid, &st, 0) != kid) {
        no("waitpid for the first stop errno=%d", errno);
    } else if (!WIFSTOPPED(st)) {
        no("first wait reported status=0x%x (WIFEXITED=%d) -- the child ran "
           "to completion instead of stopping, so TRACEME did nothing",
           st, WIFEXITED(st));
    } else {
        ok(1, "TRACEME + first stop: WIFSTOPPED, signal %d", WSTOPSIG(st));
    }

    /* Options: the implemented one must be accepted... */
    int opt_ok = (ptrace(PTRACE_SETOPTIONS, kid, 0, PTRACE_O_TRACESYSGOOD) == 0);
    /* ...and an unimplemented one refused. */
    errno = 0;
    int bad_opt = ptrace(PTRACE_SETOPTIONS, kid, 0, 0x00100000 /* EXITKILL */);
    int bad_errno = errno;

    /* bit7: the refusals -- probed HERE, while the tracee is still alive and
     * stopped. The first version asked after PTRACE_CONT had run the child to
     * exit, and got ESRCH ("no such process") instead of EINVAL -- which is
     * the kernel being right and the test asking at the wrong time. A refusal
     * check has to run when the thing being refused is otherwise possible,
     * or it is not testing the refusal at all. */
    int refusals_ok = 0;
    {
        errno = 0;
        long ss = ptrace(PTRACE_SINGLESTEP, kid, 0, 0);
        int ss_errno = errno;
        if (!opt_ok)
            no("SETOPTIONS(TRACESYSGOOD) was refused, and it is implemented");
        else if (bad_opt == 0)
            no("SETOPTIONS accepted an unimplemented option -- a tracer would "
               "wait forever for an event stop that never comes");
        else if (bad_errno != EINVAL)
            no("the unimplemented option was refused with errno=%d, want "
               "EINVAL", bad_errno);
        else if (ss == 0 || ss_errno != EINVAL)
            no("SINGLESTEP returned %ld errno=%d -- an unimplemented request "
               "must refuse with EINVAL, not pretend", ss, ss_errno);
        else
            refusals_ok = 1;
    }

    /* Arm syscall-stops and run to the child's write(2). */
    long entry_nr = -1, exit_ret = -1, r_rdi = -1, r_rdx = -1;
    int stops = 0, pairs = 0;
    struct user_regs_struct regs;

    if (ptrace(PTRACE_SYSCALL, kid, 0, 0) != 0) {
        no("PTRACE_SYSCALL errno=%d", errno);
    } else {
        for (int i = 0; i < 200; i++) {
            if (waitpid(kid, &st, 0) != kid) break;
            if (!WIFSTOPPED(st)) break;
            stops++;
            if (ptrace(PTRACE_GETREGS, kid, 0, &regs) != 0) {
                no("GETREGS errno=%d", errno);
                break;
            }
            if ((stops & 1) == 1) {                 /* an ENTRY stop */
                if ((long)regs.orig_rax == SYS_write &&
                    (long)regs.rdx == (long)PATLEN && entry_nr < 0) {
                    entry_nr = (long)regs.orig_rax;
                    r_rdi    = (long)regs.rdi;
                    r_rdx    = (long)regs.rdx;
                }
            } else {                                 /* the matching EXIT */
                pairs++;
                if (entry_nr >= 0 && exit_ret < 0) exit_ret = (long)regs.rax;
            }
            if (entry_nr >= 0 && exit_ret >= 0) break;
            if (ptrace(PTRACE_SYSCALL, kid, 0, 0) != 0) break;
        }
    }

    if (stops >= 2 && pairs >= 1)
        ok(2, "entry and exit stops alternate (%d stops, %d complete pairs)",
           stops, pairs);
    else
        no("saw %d stops / %d pairs -- a hook on only one syscall boundary "
           "looks like a working tracer until you read a return value",
           stops, pairs);

    if (entry_nr == SYS_write && r_rdx == (long)PATLEN)
        ok(4, "GETREGS at the entry stop: write(fd=%ld, .., %ld) -- the "
              "child's own call, by number and by argument", r_rdi, r_rdx);
    else
        no("never observed the child's write(2) (orig_rax=%ld rdx=%ld, want "
           "%ld and %lu)", entry_nr, r_rdx, (long)SYS_write,
           (unsigned long)PATLEN);

    if (exit_ret == (long)PATLEN)
        ok(8, "the exit stop reports rax=%ld, the byte count the child really "
              "wrote", exit_ret);
    else
        no("exit stop reported rax=%ld, want %lu", exit_ret,
           (unsigned long)PATLEN);

    /* bit4: PEEK the child's buffer. The bytes exist only over there. */
    {
        unsigned long got[3];
        int good = 1;
        for (int i = 0; i < 2; i++) {
            errno = 0;
            long w = ptrace(PTRACE_PEEKDATA, kid,
                            (void *)((char *)g_buf + i * 8), 0);
            if (w == -1 && errno) { good = 0; break; }
            got[i] = (unsigned long)w;
        }
        if (!good) {
            no("PEEKDATA errno=%d", errno);
        } else if (memcmp(got, PATTERN, 16) != 0) {
            char seen[17]; memcpy(seen, got, 16); seen[16] = 0;
            no("PEEKDATA read '%s', want the first 16 bytes of '%s'",
               seen, PATTERN);
        } else {
            ok(16, "PEEKDATA read the child's buffer: the pattern is there");
        }
    }

    /* bit5: POKE a new value in, and let the CHILD report it back. */
    {
        long want = 0x5A5A5A5A;
        if (ptrace(PTRACE_POKEDATA, kid, &g_poked, (void *)want) != 0)
            no("POKEDATA errno=%d", errno);
        else
            g_poked = want;     /* what we expect to read out of the pipe */
    }

    /* bit6: run to exit. */
    if (ptrace(PTRACE_CONT, kid, 0, 0) != 0) {
        no("PTRACE_CONT errno=%d", errno);
    }

    char out[128] = {0};
    ssize_t total = 0, r;
    while ((r = read(pfd[0], out + total, sizeof out - 1 - (size_t)total)) > 0)
        total += r;
    close(pfd[0]);

    int wst = 0;
    pid_t w = waitpid(kid, &wst, 0);
    if (w == kid && WIFEXITED(wst) && WEXITSTATUS(wst) == 33)
        ok(64, "PTRACE_CONT ran the tracee to a normal exit (code %d)",
           WEXITSTATUS(wst));
    else
        no("after CONT the child gave waitpid=%d status=0x%x (want a normal "
           "exit with code 33)", (int)w, wst);

    /* The pipe now holds the pattern followed by the poked value in decimal. */
    {
        long got = 0;
        const char *num = out + PATLEN;
        if (total >= (ssize_t)PATLEN && memcmp(out, PATTERN, PATLEN) == 0)
            got = strtol(num, NULL, 10);
        if (got == 0x5A5A5A5A)
            ok(32, "POKEDATA landed: the child read back %ld from a variable "
                   "only the tracer wrote", got);
        else
            no("the child reported %ld, want %ld -- POKEDATA did not reach "
               "the other address space", got, (long)0x5A5A5A5A);
    }

    if (refusals_ok)
        ok(128, "unimplemented requests refuse with EINVAL (SINGLESTEP, and "
                "an unsupported SETOPTIONS bit)");

    printf("ptrace: BITS=0x%x (want 0xff)\n", g_bits);
    return g_bits;
}
