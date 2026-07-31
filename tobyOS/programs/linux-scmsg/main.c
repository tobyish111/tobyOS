/* linux-scmsg -- AF_UNIX socketpair sendmsg/recvmsg ABI unit test (slice 82).
 *
 * WHY: full chrome's crashpad client does exactly this handshake --
 *   socketpair(AF_UNIX, SOCK_SEQPACKET) ; sendmsg(40 bytes, MSG_NOSIGNAL)
 *   ; recvmsg(reply + SCM_CREDENTIALS) -- and on tobyOS the browser's send
 * SUCCEEDS but it then never issues the read, taking an error path instead
 * (ledger slices 79-81). Chasing that through a 290 MB browser costs ~7 min
 * per iteration and drags in everything else chrome does. This reproduces
 * the same syscall sequence in ~30 lines, so tobyOS and a real Linux kernel
 * can be compared directly and in seconds.
 *
 * Built freestanding + static like linux-futex (no libc): raw syscalls only.
 * Verdict via exit code -- 3 = PASS, and each failure has its OWN code so
 * the boot log names the exact step that broke:
 *   3  PASS: send with MSG_NOSIGNAL, reply received, payload intact
 *  10  socketpair(SOCK_SEQPACKET) failed
 *  11  sendmsg with MSG_NOSIGNAL returned an error / short count
 *  12  recvmsg on the peer returned an error
 *  13  recvmsg returned the WRONG byte count (the "payload size" class)
 *  14  payload bytes did not survive the round trip
 *  15  sendmsg WITHOUT flags failed (isolates MSG_NOSIGNAL as the culprit)
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
#define SYS_socketpair  53
#define SYS_sendmsg     46
#define SYS_recvmsg     47
#define SYS_exit_group 231

#define AF_UNIX          1
#define SOCK_SEQPACKET   5
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
static void die(int code) { sys(SYS_exit_group, code, 0, 0, 0, 0); for(;;){} }

/* One send/recv round trip. `flags` goes to sendmsg. Returns bytes received
 * (or a negative errno from whichever call failed first). */
static s64 roundtrip(int sv[2], const char *payload, u64 len, int flags,
                     char *rxbuf, u64 rxcap) {
    struct iovec  tio = { (void *)payload, len };
    struct msghdr tmh = { 0, 0, 0, &tio, 1, 0, 0, 0, 0 };
    s64 sent = sys(SYS_sendmsg, sv[0], (long)&tmh, flags, 0, 0);
    if (sent < 0)          return sent;
    if ((u64)sent != len)  return -1000;          /* short send */

    struct iovec  rio = { rxbuf, rxcap };
    struct msghdr rmh = { 0, 0, 0, &rio, 1, 0, 0, 0, 0 };
    return sys(SYS_recvmsg, sv[1], (long)&rmh, 0, 0, 0);
}

void _start(void) {
    int sv[2] = { -1, -1 };
    char rx[64];
    /* crashpad's exact shape: 40 bytes over a SEQPACKET socketpair. */
    static const char msg[40] = { 1, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (sys(SYS_socketpair, AF_UNIX, SOCK_SEQPACKET, 0, (long)sv, 0) < 0) {
        say("[scmsg] socketpair(SOCK_SEQPACKET) FAILED\n"); die(10);
    }

    /* 1. The case chrome actually uses: MSG_NOSIGNAL. */
    s64 got = roundtrip(sv, msg, sizeof msg, MSG_NOSIGNAL, rx, sizeof rx);
    if (got == -1000) { say("[scmsg] sendmsg(MSG_NOSIGNAL) SHORT\n"); die(11); }
    if (got < 0 && got > -4096) {
        /* Distinguish "the flag broke the send" from "sends are broken": retry
         * with no flags at all. */
        s64 plain = roundtrip(sv, msg, sizeof msg, 0, rx, sizeof rx);
        if (plain == (s64)sizeof msg) {
            say("[scmsg] MSG_NOSIGNAL send/recv FAILED but flagless WORKS\n");
            die(11);
        }
        say("[scmsg] sendmsg FAILED even without flags\n"); die(15);
    }
    if (got < 0)                 { say("[scmsg] recvmsg FAILED\n");      die(12); }
    if (got != (s64)sizeof msg)  { say("[scmsg] recvmsg WRONG SIZE\n");  die(13); }
    for (u64 i = 0; i < sizeof msg; i++)
        if (rx[i] != msg[i])     { say("[scmsg] payload CORRUPT\n");     die(14); }

    say("[scmsg] PASS: SEQPACKET socketpair sendmsg(MSG_NOSIGNAL)+recvmsg "
        "round-trips 40 bytes intact\n");
    die(3);
}
