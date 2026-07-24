/* programs/linux-xstub/main.c
 *
 * Minimal fake X server for the chromium render tier (ledger slice 38,
 * render-GL prompt "solution path 1").
 *
 * WHY: ANGLE's Vulkan backend (--use-angle=vulkan routed through the real
 * Khronos loader) requires the WSI instance extensions VK_KHR_surface +
 * VK_KHR_xcb_surface at vkCreateInstance. The loader only advertises the
 * xcb surface when an X connection is possible; with no X server,
 * VerifyExtensionsPresent fails -> eglInitialize fails -> chrome crashes
 * through a NULL GL dispatch. Headless chrome never creates a real X
 * window, so the extension only needs to be PRESENT: all xcb_connect()
 * needs is the X11 connection-setup handshake.
 *
 * WHAT: listens on /tmp/.X11-unix/X0 (AF_UNIX, DISPLAY=:0), answers the
 * xConnClientPrefix -> xConnSetup success reply (1 screen, 1 TrueColor
 * 24-bit visual, 800x600), then stub-answers core requests:
 *   QueryExtension  -> "not present"
 *   InternAtom      -> fake atom 68
 *   GetProperty     -> empty
 *   ListExtensions  -> zero names
 *   GetInputFocus   -> None (also the xcb "sync" request)
 *   anything else   -> BadImplementation error (legal on any request)
 * Every request opcode is logged as [xstub] so the serial log shows
 * exactly what the client needed -- extend the stub from that evidence.
 *
 * One fork()ed child per client connection; the parent accepts forever.
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long  u64;
typedef long           i64;

static inline i64 lx6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}
#define SC1(n,a)         lx6(n,(i64)(a),0,0,0,0,0)
#define SC2(n,a,b)       lx6(n,(i64)(a),(i64)(b),0,0,0,0)
#define SC3(n,a,b,c)     lx6(n,(i64)(a),(i64)(b),(i64)(c),0,0,0)
#define SC4(n,a,b,c,d)   lx6(n,(i64)(a),(i64)(b),(i64)(c),(i64)(d),0,0)

#define SYS_read         0
#define SYS_write        1
#define SYS_close        3
#define SYS_socket       41
#define SYS_accept       43
#define SYS_bind         49
#define SYS_listen       50
#define SYS_clone        56
#define SYS_exit         60
#define SYS_wait4        61
#define SYS_unlink       87
#define SYS_mkdir        83
#define SYS_exit_group   231

#define AF_UNIX          1
#define SOCK_STREAM      1
#define SIGCHLD          17
#define WNOHANG          1

static void put(const char *s){ long n=0; while(s[n])n++; SC3(SYS_write,1,s,n); }
static void putdec(u64 v){
    char b[24]; int i=23; b[i--]='\n';
    if (!v) b[i--]='0';
    while (v) { b[i--]=(char)('0'+(v%10)); v/=10; }
    SC3(SYS_write,1,b+i+1,23-i-1+1);
}

struct sockaddr_un { u16 family; char path[108]; };

/* ---- little-endian store helpers (build wire messages in a byte buf) ---- */
static void pu16(u8 *p, u16 v){ p[0]=(u8)v; p[1]=(u8)(v>>8); }
static void pu32(u8 *p, u32 v){ p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }
static u16 gu16(const u8 *p){ return (u16)(p[0] | (p[1]<<8)); }

static long read_full(int fd, u8 *buf, long n) {
    long got = 0;
    while (got < n) {
        long r = SC3(SYS_read, fd, buf+got, n-got);
        if (r <= 0) return r;   /* 0 = EOF, <0 = error */
        got += r;
    }
    return got;
}
static long write_full(int fd, const u8 *buf, long n) {
    long done = 0;
    while (done < n) {
        long r = SC3(SYS_write, fd, buf+done, n-done);
        if (r <= 0) return r;
        done += r;
    }
    return done;
}
static long pad4(long n){ return (n + 3) & ~3L; }

/* ---- the connection-setup success reply ---- */
static long build_setup(u8 *o) {
    long i = 0;
    /* header (8) -- additional length patched at the end */
    o[i++] = 1;  o[i++] = 0;                 /* success, pad */
    pu16(o+i, 11); i += 2;                   /* protocol major */
    pu16(o+i, 0);  i += 2;                   /* protocol minor */
    long lenpos = i; i += 2;                 /* additional length (4B units) */
    /* fixed body (32) */
    pu32(o+i, 1);          i += 4;           /* release number */
    pu32(o+i, 0x00400000); i += 4;           /* resource id base */
    pu32(o+i, 0x003fffff); i += 4;           /* resource id mask */
    pu32(o+i, 256);        i += 4;           /* motion buffer size */
    pu16(o+i, 6);          i += 2;           /* vendor length ("tobyOS") */
    pu16(o+i, 65535);      i += 2;           /* max request length */
    o[i++] = 1;                              /* number of screens */
    o[i++] = 1;                              /* number of pixmap formats */
    o[i++] = 0;                              /* image byte order: LSB */
    o[i++] = 0;                              /* bitmap bit order */
    o[i++] = 32;                             /* scanline unit */
    o[i++] = 32;                             /* scanline pad */
    o[i++] = 8;                              /* min keycode */
    o[i++] = 255;                            /* max keycode */
    pu32(o+i, 0); i += 4;                    /* pad */
    /* vendor, padded to 4 */
    o[i++]='t'; o[i++]='o'; o[i++]='b'; o[i++]='y'; o[i++]='O'; o[i++]='S';
    o[i++]=0; o[i++]=0;
    /* one pixmap format (8) */
    o[i++] = 24; o[i++] = 32; o[i++] = 32;   /* depth, bpp, scanline pad */
    o[i++]=0; o[i++]=0; o[i++]=0; o[i++]=0; o[i++]=0;
    /* one screen (40) + 1 depth (8) + 1 visual (24) */
    pu32(o+i, 0x53);       i += 4;           /* root window */
    pu32(o+i, 0x20);       i += 4;           /* default colormap */
    pu32(o+i, 0x00ffffff); i += 4;           /* white pixel */
    pu32(o+i, 0x00000000); i += 4;           /* black pixel */
    pu32(o+i, 0);          i += 4;           /* current input masks */
    pu16(o+i, 800);        i += 2;           /* width px */
    pu16(o+i, 600);        i += 2;           /* height px */
    pu16(o+i, 211);        i += 2;           /* width mm */
    pu16(o+i, 158);        i += 2;           /* height mm */
    pu16(o+i, 1);          i += 2;           /* min installed maps */
    pu16(o+i, 1);          i += 2;           /* max installed maps */
    pu32(o+i, 0x21);       i += 4;           /* root visual id */
    o[i++] = 0;                              /* backing stores: Never */
    o[i++] = 0;                              /* save unders */
    o[i++] = 24;                             /* root depth */
    o[i++] = 1;                              /* number of depths */
    /* depth 24 */
    o[i++] = 24; o[i++] = 0;
    pu16(o+i, 1); i += 2;                    /* 1 visual */
    pu32(o+i, 0); i += 4;                    /* pad */
    /* visual 0x21: TrueColor 8/8/8 */
    pu32(o+i, 0x21);       i += 4;           /* visual id */
    o[i++] = 4;                              /* class TrueColor */
    o[i++] = 8;                              /* bits per rgb */
    pu16(o+i, 256); i += 2;                  /* colormap entries */
    pu32(o+i, 0x00ff0000); i += 4;           /* red mask */
    pu32(o+i, 0x0000ff00); i += 4;           /* green mask */
    pu32(o+i, 0x000000ff); i += 4;           /* blue mask */
    pu32(o+i, 0);          i += 4;           /* pad */

    pu16(o+lenpos, (u16)((i - 8) / 4));      /* additional length */
    return i;
}

/* ---- per-connection request loop ---- */
static void serve(int cfd) {
    u8 buf[4096];

    /* xConnClientPrefix: 12 bytes, then auth name/data (padded). */
    if (read_full(cfd, buf, 12) != 12) { put("[xstub] short prefix\n"); return; }
    if (buf[0] != 'l') { put("[xstub] big-endian client -- unsupported\n"); return; }
    long an = gu16(buf+6), ad = gu16(buf+8);
    long skip = pad4(an) + pad4(ad);
    if (skip > 0 && skip <= (long)sizeof(buf)) {
        if (read_full(cfd, buf, skip) != skip) { put("[xstub] short auth\n"); return; }
    }

    u8 setup[512];
    long slen = build_setup(setup);
    if (write_full(cfd, setup, slen) != slen) { put("[xstub] setup write failed\n"); return; }
    put("[xstub] client connected, setup sent\n");

    u16 seq = 0;
    for (;;) {
        if (read_full(cfd, buf, 4) != 4) { put("[xstub] client closed\n"); return; }
        seq++;
        u8  major = buf[0];
        long words = gu16(buf+2);
        long body  = words * 4 - 4;
        if (body < 0 || body > (long)sizeof(buf)) { put("[xstub] bad request length\n"); return; }
        if (body > 0 && read_full(cfd, buf+4, body) != body) { put("[xstub] short request\n"); return; }

        put("[xstub] request opcode="); putdec(major);

        u8 rep[32];
        for (int k = 0; k < 32; k++) rep[k] = 0;
        switch (major) {
        case 98:                              /* QueryExtension -> absent */
            rep[0] = 1; pu16(rep+2, seq);     /* reply, sequence */
            rep[8] = 0;                       /* present = false */
            write_full(cfd, rep, 32);
            break;
        case 16:                              /* InternAtom -> fake atom */
            rep[0] = 1; pu16(rep+2, seq);
            pu32(rep+8, 68);
            write_full(cfd, rep, 32);
            break;
        case 20:                              /* GetProperty -> empty */
            rep[0] = 1; pu16(rep+2, seq);
            write_full(cfd, rep, 32);
            break;
        case 99:                              /* ListExtensions -> none */
            rep[0] = 1; pu16(rep+2, seq);
            write_full(cfd, rep, 32);
            break;
        case 43:                              /* GetInputFocus (xcb sync) */
            rep[0] = 1; pu16(rep+2, seq);
            write_full(cfd, rep, 32);
            break;
        default:                              /* BadImplementation error */
            rep[0] = 0; rep[1] = 17; pu16(rep+2, seq);
            rep[10] = major;
            write_full(cfd, rep, 32);
            break;
        }
    }
}

extern void _start(void);
__asm__(".globl _start\n_start:\n  call main_c\n  mov %rax,%rdi\n  mov $231,%rax\n  syscall\n");

int main_c(void);
int main_c(void) {
    put("[xstub] fake X server starting on /tmp/.X11-unix/X0\n");

    SC2(SYS_mkdir, "/tmp", 0777);
    SC2(SYS_mkdir, "/tmp/.X11-unix", 0777);
    SC1(SYS_unlink, "/tmp/.X11-unix/X0");

    int sfd = (int)SC3(SYS_socket, AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { put("[xstub] socket FAILED\n"); return 2; }

    struct sockaddr_un sa;
    for (unsigned k = 0; k < sizeof(sa); k++) ((char *)&sa)[k] = 0;
    sa.family = AF_UNIX;
    const char *p = "/tmp/.X11-unix/X0";
    for (int k = 0; p[k]; k++) sa.path[k] = p[k];
    if (SC3(SYS_bind, sfd, &sa, sizeof(sa)) < 0) { put("[xstub] bind FAILED\n"); return 2; }
    if (SC2(SYS_listen, sfd, 8) < 0) { put("[xstub] listen FAILED\n"); return 2; }
    put("[xstub] listening\n");

    for (;;) {
        int cfd = (int)SC3(SYS_accept, sfd, 0, 0);
        if (cfd < 0) { put("[xstub] accept FAILED\n"); return 2; }
        i64 pid = lx6(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);
        if (pid == 0) {                       /* child: serve this client */
            SC1(SYS_close, sfd);
            serve(cfd);
            SC1(SYS_close, cfd);
            SC1(SYS_exit, 0);
        }
        SC1(SYS_close, cfd);
        /* reap any finished children without blocking the accept loop */
        while (SC4(SYS_wait4, -1, 0, WNOHANG, 0) > 0) { }
    }
}
