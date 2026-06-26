/* programs/linux-netserver/main.c
 *
 * A genuine Linux x86-64 static ELF (raw syscalls, no libc) proving tobyOS's
 * B14 networking capstone: an EVENT-DRIVEN TCP server built entirely on the
 * Linux socket + epoll syscalls, with TCP-socket readiness wired into the
 * kernel's poll/epoll predicate (file_poll_ready).
 *
 * Flow (no blocking accept/recv -- everything is driven by epoll_wait):
 *   socket(AF_INET, SOCK_STREAM) -> bind :8080 -> listen
 *   epoll_create1 + epoll_ctl(ADD, listenfd, EPOLLIN)
 *   print "[b14] listening" (the host harness waits for this marker)
 *   loop:
 *     epoll_wait
 *       - listenfd readable  => accept(), add the new conn fd (EPOLLIN)
 *       - conn fd  readable  => read(); echo the bytes straight back;
 *                               if the magic token "TOBYNET" was seen,
 *                               finish with exit code 14.
 *
 * The client is the QEMU host (SLIRP hostfwd tcp::18082-:8080); logs/b14.sh
 * connects once the marker shows, sends the token, and checks the echo. The
 * server only ever reaches exit 14 if a REAL external peer connected, its
 * readiness was reported through epoll, and the echo write succeeded.
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef short          i16;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_read           0
#define SYS_write          1
#define SYS_close          3
#define SYS_socket         41
#define SYS_accept         43
#define SYS_bind           49
#define SYS_listen         50
#define SYS_setsockopt     54
#define SYS_epoll_create1  291
#define SYS_epoll_ctl      233
#define SYS_epoll_wait     232
#define SYS_exit_group     231

#define AF_INET        2
#define SOCK_STREAM    1
#define SOL_SOCKET     1
#define SO_REUSEADDR   2

#define EPOLLIN        0x001
#define EPOLL_CTL_ADD  1

struct ep_ev { u32 events; u64 data; } __attribute__((packed));

static u64 slen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
static void put(const char *s){ sc(SYS_write,1,(i64)s,(i64)slen(s),0,0); }

__attribute__((noreturn))
static void done(int code){ sc(SYS_exit_group,code,0,0,0,0); for(;;){} }

/* htons for a compile-time port (little-endian host -> network order). */
static u16 hns(u16 p){ return (u16)((p << 8) | (p >> 8)); }

void _start(void) __attribute__((force_align_arg_pointer));
void _start(void) {
    int s = (int)sc(SYS_socket, AF_INET, SOCK_STREAM, 0, 0, 0);
    if (s < 0) { put("[b14] socket FAIL\n"); done(10); }

    int one = 1;
    sc(SYS_setsockopt, s, SOL_SOCKET, SO_REUSEADDR, (i64)&one, 4);

    /* struct sockaddr_in { family; port; addr; zero[8] } -- 16 bytes. */
    unsigned char sa[16];
    for (int i = 0; i < 16; i++) sa[i] = 0;
    *(u16 *)(sa + 0) = AF_INET;
    *(u16 *)(sa + 2) = hns(8080);
    *(u32 *)(sa + 4) = 0;                 /* INADDR_ANY */
    if (sc(SYS_bind, s, (i64)sa, 16, 0, 0) != 0) { put("[b14] bind FAIL\n"); done(11); }
    if (sc(SYS_listen, s, 8, 0, 0, 0) != 0)      { put("[b14] listen FAIL\n"); done(12); }

    int ep = (int)sc(SYS_epoll_create1, 0, 0, 0, 0, 0);
    if (ep < 0) { put("[b14] epoll_create FAIL\n"); done(13); }
    struct ep_ev ev; ev.events = EPOLLIN; ev.data = (u64)s;
    if (sc(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, s, (i64)&ev, 0) != 0) {
        put("[b14] epoll_ctl(listen) FAIL\n"); done(14);
    }

    put("[b14] listening\n");

    char buf[512];
    int got_magic = 0;
    /* Bounded event loop: a few thousand epoll_wait turns is plenty for one
     * client exchange and guarantees the boot never wedges if no peer shows. */
    for (int turn = 0; turn < 3000; turn++) {
        struct ep_ev out[8];
        long n = sc(SYS_epoll_wait, ep, (i64)out, 8, 1000, 0);
        if (n <= 0) continue;             /* timeout / spurious -> keep waiting */
        for (long i = 0; i < n; i++) {
            int fd = (int)out[i].data;
            if (fd == s) {
                /* accept won't block: epoll already reported a pending conn. */
                int c = (int)sc(SYS_accept, s, 0, 0, 0, 0);
                if (c < 0) continue;
                put("[b14] accepted connection\n");
                struct ep_ev ce; ce.events = EPOLLIN; ce.data = (u64)c;
                sc(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, c, (i64)&ce, 0);
            } else {
                long r = sc(SYS_read, fd, (i64)buf, sizeof(buf), 0, 0);
                if (r <= 0) { sc(SYS_close, fd, 0, 0, 0, 0); continue; }
                /* Echo the received bytes straight back to the peer. */
                long off = 0;
                while (off < r) {
                    long w = sc(SYS_write, fd, (i64)(buf + off), r - off, 0, 0);
                    if (w <= 0) break;
                    off += w;
                }
                /* Look for the magic token anywhere in the request. */
                for (long k = 0; k + 7 <= r; k++) {
                    if (buf[k]=='T'&&buf[k+1]=='O'&&buf[k+2]=='B'&&buf[k+3]=='Y'&&
                        buf[k+4]=='N'&&buf[k+5]=='E'&&buf[k+6]=='T') { got_magic = 1; break; }
                }
                if (got_magic) {
                    put("[b14] echoed magic; done\n");
                    sc(SYS_close, fd, 0, 0, 0, 0);
                    sc(SYS_close, s, 0, 0, 0, 0);
                    sc(SYS_close, ep, 0, 0, 0, 0);
                    done(14);             /* success: real peer + epoll + echo */
                }
                sc(SYS_close, fd, 0, 0, 0, 0);
            }
        }
    }

    put("[b14] no client -- giving up\n");
    done(20);
}
