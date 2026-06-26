/* programs/linux-poll/main.c
 *
 * A genuine Linux x86-64 static ELF (raw syscalls, no libc) proving tobyOS's
 * B13 readiness multiplexing: poll / ppoll / select / epoll on a real pipe.
 * It exercises the kernel syscalls directly (no libc wrappers):
 *
 *   - poll(POLLIN, timeout=0) on an EMPTY pipe (writer alive) -> 0 ready
 *   - poll(POLLIN, timeout=30ms) on an empty pipe            -> 0 (timeout path)
 *   - write a byte, then poll(POLLIN)                        -> 1, revents&POLLIN
 *   - poll(POLLOUT) on the write end                         -> 1, revents&POLLOUT
 *   - select() with the read fd in readfds                   -> 1, fd set
 *   - epoll_create1 + epoll_ctl(ADD,EPOLLIN) + epoll_wait     -> 1, data matches
 *   - close the write end, poll the read end                 -> POLLHUP/POLLIN (EOF)
 *
 * Exit 0 iff every check passes; otherwise the exit code pinpoints the step.
 */

typedef unsigned long  u64;
typedef long           i64;
typedef unsigned int   u32;
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
#define SYS_poll           7
#define SYS_select         23
#define SYS_pipe2          293
#define SYS_epoll_create1  291
#define SYS_epoll_ctl      233
#define SYS_epoll_wait     232
#define SYS_exit_group     231

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLHUP  0x010

#define EPOLLIN  0x001
#define EPOLL_CTL_ADD 1

struct pollfd { int fd; i16 events; i16 revents; };
struct ep_ev  { u32 events; u64 data; } __attribute__((packed));

static u64 slen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
static void put(const char *s){ sc(SYS_write,1,(i64)s,(i64)slen(s),0,0); }

__attribute__((noreturn))
static void done(int code){ sc(SYS_exit_group,code,0,0,0,0); for(;;){} }

__attribute__((force_align_arg_pointer))
void _start(void) {
    int fds[2];
    if (sc(SYS_pipe2, (i64)fds, 0, 0, 0, 0) != 0) { put("pipe2 FAIL\n"); done(10); }
    int rfd = fds[0], wfd = fds[1];

    struct pollfd pf;

    /* (1) empty pipe, non-blocking poll -> not readable yet. */
    pf.fd = rfd; pf.events = POLLIN; pf.revents = 0;
    if (sc(SYS_poll, (i64)&pf, 1, 0, 0, 0) != 0) { put("poll empty FAIL\n"); done(11); }

    /* (2) empty pipe, 30ms timeout -> still 0 (the timeout path runs). */
    pf.fd = rfd; pf.events = POLLIN; pf.revents = 0;
    if (sc(SYS_poll, (i64)&pf, 1, 30, 0, 0) != 0) { put("poll timeout FAIL\n"); done(12); }

    /* (3) write a byte; now the read end must be POLLIN-ready. */
    char b = 'Z';
    if (sc(SYS_write, wfd, (i64)&b, 1, 0, 0) != 1) { put("write FAIL\n"); done(13); }
    pf.fd = rfd; pf.events = POLLIN; pf.revents = 0;
    if (sc(SYS_poll, (i64)&pf, 1, 100, 0, 0) != 1 || !(pf.revents & POLLIN)) {
        put("poll readable FAIL\n"); done(14);
    }

    /* (4) the write end must be POLLOUT-ready (buffer not full). */
    pf.fd = wfd; pf.events = POLLOUT; pf.revents = 0;
    if (sc(SYS_poll, (i64)&pf, 1, 0, 0, 0) != 1 || !(pf.revents & POLLOUT)) {
        put("poll writable FAIL\n"); done(15);
    }

    /* (5) select(): read fd in readfds, must report ready (data present). */
    {
        unsigned long rd[16]; for (int i=0;i<16;i++) rd[i]=0;
        rd[rfd >> 6] |= (1UL << (rfd & 63));
        long r = sc(SYS_select, rfd + 1, (i64)rd, 0, 0, 0);  /* NULL timeout arg5=0? */
        /* arg5 is timeval*; pass NULL via a 6th arg. sc() only sets 5 args, but
         * select's timeout is arg5 (e) -- we passed e=0 (NULL) => infinite, but
         * data is present so it returns immediately. */
        if (r < 1 || !(rd[rfd >> 6] & (1UL << (rfd & 63)))) { put("select FAIL\n"); done(16); }
    }

    /* (6) epoll: ADD the read fd for EPOLLIN, wait -> 1 event w/ our data tag. */
    {
        int ep = (int)sc(SYS_epoll_create1, 0, 0, 0, 0, 0);
        if (ep < 0) { put("epoll_create1 FAIL\n"); done(17); }
        struct ep_ev ev; ev.events = EPOLLIN; ev.data = 0xC0FFEEUL;
        if (sc(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, rfd, (i64)&ev, 0) != 0) {
            put("epoll_ctl FAIL\n"); done(18);
        }
        struct ep_ev out[4];
        long n = sc(SYS_epoll_wait, ep, (i64)out, 4, 100, 0);
        if (n != 1 || !(out[0].events & EPOLLIN) || out[0].data != 0xC0FFEEUL) {
            put("epoll_wait FAIL\n"); done(19);
        }
        sc(SYS_close, ep, 0, 0, 0, 0);
    }

    /* (7) EOF: drain the byte, close the writer, poll -> POLLHUP (or POLLIN EOF). */
    { char tmp; sc(SYS_read, rfd, (i64)&tmp, 1, 0, 0); }
    sc(SYS_close, wfd, 0, 0, 0, 0);
    pf.fd = rfd; pf.events = POLLIN; pf.revents = 0;
    if (sc(SYS_poll, (i64)&pf, 1, 100, 0, 0) != 1 ||
        !(pf.revents & (POLLHUP | POLLIN))) {
        put("poll EOF FAIL\n"); done(20);
    }

    put("linux poll/select/epoll readiness multiplexing OK\n");
    done(0);
}
