/* linux-crashpad -- a crashpad handler stub that actually ANSWERS (slice 90).
 *
 * WHY THIS EXISTS
 * ---------------
 * /opt/chrome/chrome_crashpad_handler used to be staged as a copy of the
 * xdg-settings stub, whose entire body is `exit_group(0)`. So the handler
 * exec'd and died in the same millisecond, its AF_UNIX endpoint went with
 * it, and EVERY later client sendmsg on that socket returned -EPIPE. The
 * client treats that as unrecoverable: INT3 -> exit_group(191). Slice 89
 * mis-read this as an execve/auxv bug because the handler "exits 0 after
 * four syscalls" -- it does, because we told it to.
 *
 * chrome passes --disable-crash-reporter, but the crashpad CLIENT HANDSHAKE
 * RUNS ANYWAY (established in slice 75), so the handler cannot simply be
 * absent or short-lived: something must hold that socket open and reply.
 *
 * THE CONTRACT (decoded from the WSL control's strace in slice 76b -- this
 * is measured, not guessed):
 *   browser -> handler : sendmsg(iov = 40 bytes, msg_controllen = 0)
 *   handler -> browser : reply of 8 bytes; the kernel attaches
 *                        SOL_SOCKET/SCM_CREDENTIALS {pid,uid,gid} because
 *                        the browser set SO_PASSCRED on its end
 *   browser            : prctl(PR_SET_PTRACER, pid) using the pid FROM
 *                        THOSE CREDENTIALS
 * tobyOS synthesises the credentials kernel-side (slice 77), so this stub
 * only has to read the request and write 8 bytes back.
 *
 * The reply BODY is zeros: what the browser consumes from this exchange is
 * the credentials (for PR_SET_PTRACER), and the 8 payload bytes are a
 * status word whose non-zero values all mean "something is wrong". If a
 * later chrome build starts inspecting them, the failure will name itself.
 *
 * STAYING ALIVE IS THE POINT. Replying once and exiting would recreate the
 * original bug the moment the next client connects, so this loops forever
 * and answers every request. It only exits if the socket reports EOF (peer
 * gone = browser is done with us) or an unrecoverable error.
 *
 * Freestanding + static, raw syscalls, no libc -- same recipe as
 * linux-scmsg / linux-futex / linux-xdg-settings. Must be brandelf'd
 * EI_OSABI=3 (Linux) by the Makefile so a Linux-personality chrome child
 * can exec it.
 */

typedef unsigned long u64;
typedef long          s64;

static s64 sys(long n, long a, long b, long c, long d, long e) {
    s64 r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_write        1
#define SYS_sendmsg     46
#define SYS_recvmsg     47
#define SYS_exit_group 231

#define MSG_NOSIGNAL  0x4000

struct iovec  { void *base; u64 len; };
struct msghdr {
    void *name; unsigned namelen; unsigned _pad;
    struct iovec *iov; u64 iovlen;
    void *control; u64 controllen;
    int flags; int _pad2;
};

static void say(const char *s) {
    u64 n = 0; while (s[n]) n++;
    (void)sys(SYS_write, 2, (long)s, (long)n, 0, 0);
}

static void die(int code) { sys(SYS_exit_group, code, 0, 0, 0, 0); for (;;) { } }

/* Tiny helpers -- no libc. */
static int str_eq_prefix(const char *s, const char *pfx) {
    while (*pfx) { if (*s != *pfx) return 0; s++; pfx++; }
    return 1;
}
static int atoi_simple(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

/* _start receives the raw SysV process stack: [argc][argv...][NULL][envp...].
 * It MUST be naked -- reading rsp from inside a normal C function is a lie
 * the moment the compiler emits any prologue. Capture rsp as the first
 * instruction and hand it to C as an ordinary argument. */
static void __attribute__((used, noreturn)) crashpad_main(long *sp);

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile("xor %rbp, %rbp\n\t"
                     "mov %rsp, %rdi\n\t"      /* sp -> first C argument */
                     "and $-16, %rsp\n\t"      /* SysV alignment for the call */
                     "call crashpad_main");
}

static void __attribute__((used, noreturn)) crashpad_main(long *sp) {
    long argc = sp[0];
    char **argv = (char **)&sp[1];

    int client_fd = -1;
    if (argc > 0 && argc < 4096) {
        for (long i = 1; i < argc; i++) {
            if (!argv[i]) break;
            if (str_eq_prefix(argv[i], "--initial-client-fd=")) {
                client_fd = atoi_simple(argv[i] + 20);
                break;
            }
        }
    }

    if (client_fd < 0) {
        /* No client socket to serve. Do NOT exit(0) into the old trap --
         * park instead, so any endpoint we inherited stays open and no
         * client sees EPIPE. Loud, because it means the argv contract
         * changed. */
        say("[crashpad-stub] no --initial-client-fd; parking\n");
        for (;;) (void)sys(SYS_recvmsg, 0, 0, 0, 0, 0);
    }

    say("[crashpad-stub] serving client fd\n");

    unsigned char rxbuf[256];
    unsigned char txbuf[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    for (;;) {
        struct iovec riov = { rxbuf, sizeof rxbuf };
        struct msghdr rmsg;
        unsigned char *z = (unsigned char *)&rmsg;
        for (u64 i = 0; i < sizeof rmsg; i++) z[i] = 0;
        rmsg.iov = &riov; rmsg.iovlen = 1;

        s64 r = sys(SYS_recvmsg, client_fd, (long)&rmsg, 0, 0, 0);
        if (r == 0) {
            /* Peer closed: the browser is finished with us. */
            say("[crashpad-stub] client EOF; exiting\n");
            die(0);
        }
        if (r < 0) {
            if (r == -4 /* EINTR */ || r == -11 /* EAGAIN */) continue;
            say("[crashpad-stub] recvmsg error; parking\n");
            /* Never exit on a transient error -- exiting is what breaks the
             * browser. Park holding the fd open. */
            for (;;) (void)sys(SYS_recvmsg, client_fd, (long)&rmsg, 0, 0, 0);
        }

        struct iovec siov = { txbuf, sizeof txbuf };
        struct msghdr smsg;
        z = (unsigned char *)&smsg;
        for (u64 i = 0; i < sizeof smsg; i++) z[i] = 0;
        smsg.iov = &siov; smsg.iovlen = 1;
        (void)sys(SYS_sendmsg, client_fd, (long)&smsg, MSG_NOSIGNAL, 0, 0);
        /* A failed reply is not fatal either: keep serving. */
    }
}
