/* programs/sigregs/main.c -- /bin/sigregs
 *
 * Do the user's rcx and r11 survive an ASYNCHRONOUS signal?
 *
 * WHY THIS EXISTS. A caught signal that lands while a process is running
 * user code is delivered from the timer IRQ (pit.c -> signal_deliver_irq ->
 * signal_deliver_fault), which builds the on-stack sig_context from the IRQ
 * trapframe. That frame deliberately omitted rcx and r11 -- "the syscall
 * instruction clobbers them by ABI, so the interrupted code treats them as
 * dead" -- and sys_sigreturn resumed the interrupted context through the
 * syscall exit's SYSRETQ, which loads RIP from rcx and RFLAGS from r11. Both
 * claims are true ONLY when the interrupted context was a syscall. An IRQ
 * interrupts arbitrary user code, where rcx and r11 are as live as any other
 * register, so every asynchronous signal handed the resumed code
 * rcx = its own RIP and r11 = its RFLAGS.
 *
 * Found by the Oils corpus, not by inspection: the guest's GNU bash 5.2 took
 * a user-mode page fault in expand_word_internal at
 *
 *     cmpb $0, ifs_cmap(%rdi,%rax,1)      rax = movslq(ecx) = 0x4326b6
 *
 * with ecx -- the character being classified -- holding a code address 89
 * bytes before the fault, r11 equal to rflags, and the last two syscalls of
 * that thread rt_sigreturn: two SIGCHLDs for two children that had just
 * exited, the second delivered while bash was between syscalls.
 *
 * WHAT IT DOES. The parent installs a SIGUSR1 handler and spins in user
 * code holding known constants in rcx and r11, checking them after every
 * spin. A forked child sends SIGUSR1 to the parent N times, waiting for a
 * one-byte acknowledgement from the handler over a pipe before sending the
 * next, so every signal is a distinct delivery and the count is exact. The
 * parent spins between syscalls, so nearly every delivery is the
 * timer-IRQ kind. One clobbered register is one failure.
 *
 * Output is one line, fixed text, for the distro fixture's `eq` check:
 *
 *     sigregs: 200 signals, 0 clobbers
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define N_SIGNALS 200
#define RCX_MAGIC 0x1122334455667788ULL
#define R11_MAGIC 0x8877665544332211ULL

static volatile int g_hits;
static int g_ack_w = -1;

static void on_usr1(int sig) {
    (void)sig;
    g_hits++;
    char b = 1;
    (void)write(g_ack_w, &b, 1);
}

/* Load the magic values, spin `iters` times in pure user code, then read the
 * two registers back. rcx/r11 are in the clobber list so the compiler keeps
 * every operand out of them. */
static void spin_and_read(uint64_t iters, uint64_t *rcx_out, uint64_t *r11_out) {
    uint64_t c = RCX_MAGIC, e = R11_MAGIC, oc, oe;
    __asm__ volatile(
        "mov %[c], %%rcx\n\t"
        "mov %[e], %%r11\n\t"
        "1:\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        "mov %%rcx, %[oc]\n\t"
        "mov %%r11, %[oe]\n\t"
        : [oc] "=r"(oc), [oe] "=r"(oe), [n] "+r"(iters)
        : [c] "r"(c), [e] "r"(e)
        : "rcx", "r11", "cc", "memory");
    *rcx_out = oc;
    *r11_out = oe;
}

int main(void) {
    int ack[2];
    if (pipe(ack) != 0) { fprintf(stderr, "sigregs: pipe failed\n"); return 2; }
    g_ack_w = ack[1];

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        fprintf(stderr, "sigregs: sigaction failed\n");
        return 2;
    }

    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0) { fprintf(stderr, "sigregs: fork failed\n"); return 2; }
    if (child == 0) {
        close(ack[1]);
        for (int i = 0; i < N_SIGNALS; i++) {
            /* A few hundred microseconds so the parent is back in its spin
             * (not in the handler's write, nor in sigreturn) when the next
             * one lands -- that is the delivery path under test. */
            usleep(300);
            if (kill(parent, SIGUSR1) != 0) _exit(3);
            char b;
            if (read(ack[0], &b, 1) != 1) _exit(4);
        }
        _exit(0);
    }
    close(ack[0]);

    long clobbers = 0, checks = 0;
    uint64_t first_rcx = 0, first_r11 = 0;
    while (g_hits < N_SIGNALS) {
        uint64_t rcx, r11;
        spin_and_read(20000, &rcx, &r11);
        checks++;
        if (rcx != RCX_MAGIC || r11 != R11_MAGIC) {
            if (clobbers == 0) { first_rcx = rcx; first_r11 = r11; }
            clobbers++;
        }
    }
    int st = 0;
    (void)waitpid(child, &st, 0);

    if (clobbers) {
        fprintf(stderr, "sigregs: FIRST CLOBBER rcx=0x%llx (want 0x%llx) "
                "r11=0x%llx (want 0x%llx) after %ld checks\n",
                (unsigned long long)first_rcx, (unsigned long long)RCX_MAGIC,
                (unsigned long long)first_r11, (unsigned long long)R11_MAGIC,
                checks);
    }
    printf("sigregs: %d signals, %ld clobbers\n", g_hits, clobbers);
    return clobbers ? 1 : 0;
}
