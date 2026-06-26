/* programs/linux-httpget/main.c
 *
 * A GENUINE Linux x86-64 static ELF (raw Linux syscalls, no libc) that proves
 * tobyOS's B16 networking-client capstone: a Linux binary RESOLVES a hostname
 * over DNS (UDP socket) and FETCHES it over HTTP (TCP active-open) entirely via
 * the Linux BSD-socket syscalls -- the client half B14 left out.
 *
 * Flow (all raw syscalls, would run unmodified on real Linux):
 *   1. read /etc/resolv.conf -> nameserver IP (fallback 10.0.2.3, SLIRP's DNS).
 *   2. socket(AF_INET,SOCK_DGRAM) + sendto a DNS A query for HOST to ns:53,
 *      recvfrom the reply, parse the first A record  -> server IP.    [UDP]
 *   3. socket(AF_INET,SOCK_STREAM) + connect(serverIP:80),
 *      send "GET / HTTP/1.0...", recv the reply, check it starts "HTTP/". [TCP]
 *
 * Exit 55 only if BOTH the DNS resolve and the HTTP fetch succeed. Over QEMU
 * SLIRP this reaches the REAL internet via the host. Distinct codes (2..9)
 * pinpoint the failing stage.
 */

typedef unsigned long      u64;
typedef long               i64;
typedef unsigned int       u32;
typedef unsigned short     u16;
typedef unsigned char      u8;
typedef unsigned long      uintptr_t;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f) {
    register i64 r10 __asm__("r10") = d;
    register i64 r8  __asm__("r8")  = e;
    register i64 r9  __asm__("r9")  = f;
    i64 r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}

#define SYS_read       0
#define SYS_write      1
#define SYS_open       2
#define SYS_close      3
#define SYS_socket    41
#define SYS_connect   42
#define SYS_sendto    44
#define SYS_recvfrom  45
#define SYS_setsockopt 54
#define SYS_exit_group 231

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOL_SOCKET   1
#define SO_RCVTIMEO  20

#define HOST "example.com"

struct sockaddr_in { u16 sin_family; u16 sin_port; u32 sin_addr; u8 pad[8]; };

static u64 slen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
static void put(const char *s){ sc(SYS_write,1,(i64)s,(i64)slen(s),0,0,0); }

static u16 hns(u16 p){ return (u16)((p<<8)|(p>>8)); }   /* host->net short */

/* Print an IPv4 (network-order u32) as a.b.c.d for the serial log. */
static void put_ip(u32 ip_be){
    u8 *b = (u8 *)&ip_be;
    char out[16]; int o=0;
    for (int i=0;i<4;i++){
        int v=b[i]; char t[3]; int ti=0;
        if (v==0) t[ti++]='0'; else { while(v){ t[ti++]='0'+v%10; v/=10; } }
        while(ti) out[o++]=t[--ti];
        if (i<3) out[o++]='.';
    }
    out[o++]='\n'; out[o]=0; put(out);
}

/* Minimal dotted-decimal "a.b.c.d" -> network-order u32. Stops at any
 * non-digit/non-dot. Returns 0 on parse failure. */
static u32 parse_ip(const char *s){
    u32 oct[4]={0,0,0,0}; int idx=0, any=0;
    for (const char *p=s; *p; p++){
        if (*p>='0'&&*p<='9'){ oct[idx]=oct[idx]*10+(u32)(*p-'0'); any=1; }
        else if (*p=='.'){ if(++idx>3) return 0; }
        else break;
    }
    if (!any || idx!=3) return 0;
    return (oct[0])|(oct[1]<<8)|(oct[2]<<16)|(oct[3]<<24);   /* network order */
}

/* Read the first "nameserver <ip>" from /etc/resolv.conf. Fallback 10.0.2.3. */
static u32 resolver_ip(void){
    u32 fallback = (10)|(0<<8)|(2<<16)|(3<<24);
    int fd = (int)sc(SYS_open,(i64)"/etc/resolv.conf",0,0,0,0,0);
    if (fd < 0) return fallback;
    char buf[512];
    long r = sc(SYS_read,fd,(i64)buf,sizeof(buf)-1,0,0,0);
    sc(SYS_close,fd,0,0,0,0,0);
    if (r <= 0) return fallback;
    buf[r]=0;
    /* find "nameserver " */
    const char *key="nameserver";
    for (long i=0;i<r;i++){
        int m=1;
        for (int k=0;key[k];k++){ if (buf[i+k]!=key[k]){ m=0; break; } }
        if (m){
            long j=i+10; while (j<r && (buf[j]==' '||buf[j]=='\t')) j++;
            u32 ip = parse_ip(&buf[j]);
            if (ip) return ip;
        }
    }
    return fallback;
}

/* Encode "example.com" as DNS labels into out; returns bytes written. */
static int dns_encode(const char *name, u8 *out){
    int o=0; const char *p=name;
    while (*p){
        const char *s=p; while (*p && *p!='.') p++;
        int len=(int)(p-s);
        out[o++]=(u8)len;
        for (int i=0;i<len;i++) out[o++]=(u8)s[i];
        if (*p=='.') p++;
    }
    out[o++]=0;
    return o;
}

/* Skip a DNS name at off, honoring a single compression pointer. */
static int dns_skip_name(const u8 *b, int len, int off){
    while (off<len){
        u8 c=b[off];
        if (c==0) return off+1;
        if ((c&0xC0)==0xC0) return off+2;
        off += c+1;
    }
    return -1;
}

/* Resolve HOST via the UDP nameserver. Returns A-record IP (net order) or 0. */
static u32 dns_resolve(u32 ns_ip_be){
    int s=(int)sc(SYS_socket,AF_INET,SOCK_DGRAM,0,0,0,0);
    if (s<0){ put("[b16] udp socket FAIL\n"); return 0; }
    /* 5s recv timeout so a lost reply doesn't wedge the proof. */
    i64 tv[2]={5,0};
    sc(SYS_setsockopt,s,SOL_SOCKET,SO_RCVTIMEO,(i64)tv,16,0);

    u8 q[300]; int o=0;
    u16 id=0x1d16;
    q[o++]=(u8)(id>>8); q[o++]=(u8)id;
    q[o++]=0x01; q[o++]=0x00;     /* RD=1 */
    q[o++]=0;    q[o++]=1;         /* QDCOUNT=1 */
    q[o++]=0;    q[o++]=0;         /* ANCOUNT */
    q[o++]=0;    q[o++]=0;         /* NSCOUNT */
    q[o++]=0;    q[o++]=0;         /* ARCOUNT */
    o += dns_encode(HOST, q+o);
    q[o++]=0; q[o++]=1;            /* QTYPE=A */
    q[o++]=0; q[o++]=1;            /* QCLASS=IN */

    struct sockaddr_in ns; 
    for (int i=0;i<(int)sizeof(ns);i++) ((u8*)&ns)[i]=0;
    ns.sin_family=AF_INET; ns.sin_port=hns(53); ns.sin_addr=ns_ip_be;

    long sent=sc(SYS_sendto,s,(i64)q,o,0,(i64)&ns,16);
    if (sent<0){ put("[b16] dns sendto FAIL\n"); sc(SYS_close,s,0,0,0,0,0); return 0; }

    u8 resp[600];
    long n=sc(SYS_recvfrom,s,(i64)resp,sizeof(resp),0,0,0);
    sc(SYS_close,s,0,0,0,0,0);
    if (n<12){ put("[b16] dns recv FAIL/timeout\n"); return 0; }

    int an=((int)resp[6]<<8)|resp[7];
    int off=12;
    /* skip question (one) */
    off=dns_skip_name(resp,(int)n,off);
    if (off<0||off+4>(int)n) return 0;
    off+=4;
    for (int i=0;i<an;i++){
        off=dns_skip_name(resp,(int)n,off);
        if (off<0||off+10>(int)n) return 0;
        int type=((int)resp[off]<<8)|resp[off+1];
        int cls =((int)resp[off+2]<<8)|resp[off+3];
        int rdl =((int)resp[off+8]<<8)|resp[off+9];
        off+=10;
        if (off+rdl>(int)n) return 0;
        if (type==1 && cls==1 && rdl==4){
            u32 ip; for (int k=0;k<4;k++) ((u8*)&ip)[k]=resp[off+k];
            return ip;                    /* network order */
        }
        off+=rdl;
    }
    return 0;
}

/* HTTP GET / from server_ip:80; return 1 if the reply starts with "HTTP/". */
static int http_get(u32 server_ip_be){
    int s=(int)sc(SYS_socket,AF_INET,SOCK_STREAM,0,0,0,0);
    if (s<0){ put("[b16] tcp socket FAIL\n"); return 2; }
    struct sockaddr_in sa;
    for (int i=0;i<(int)sizeof(sa);i++) ((u8*)&sa)[i]=0;
    sa.sin_family=AF_INET; sa.sin_port=hns(80); sa.sin_addr=server_ip_be;
    if (sc(SYS_connect,s,(i64)&sa,16,0,0,0)!=0){ put("[b16] connect FAIL\n"); sc(SYS_close,s,0,0,0,0,0); return 3; }
    put("[b16] connected; sending GET\n");
    const char *req="GET / HTTP/1.0\r\nHost: " HOST "\r\nConnection: close\r\nUser-Agent: tobyOS-b16\r\n\r\n";
    long sent=sc(SYS_sendto,s,(i64)req,(i64)slen(req),0,0,0);
    if (sent<0){ put("[b16] http send FAIL\n"); sc(SYS_close,s,0,0,0,0,0); return 4; }
    char buf[256];
    long n=sc(SYS_recvfrom,s,(i64)buf,sizeof(buf)-1,0,0,0);
    sc(SYS_close,s,0,0,0,0,0);
    if (n<5){ put("[b16] http recv FAIL\n"); return 5; }
    buf[n]=0;
    if (buf[0]=='H'&&buf[1]=='T'&&buf[2]=='T'&&buf[3]=='P'&&buf[4]=='/'){
        /* echo the status line */
        int e=0; while (e<(int)n && buf[e]!='\r' && buf[e]!='\n') e++;
        put("[b16] reply: "); sc(SYS_write,1,(i64)buf,e,0,0,0); put("\n");
        return 0;
    }
    put("[b16] reply not HTTP\n");
    return 6;
}

__attribute__((noreturn)) static void done(int code){ sc(SYS_exit_group,code,0,0,0,0,0); for(;;){} }

__attribute__((force_align_arg_pointer))
void _start(void){
    u32 ns=resolver_ip();
    put("[b16] nameserver "); put_ip(ns);

    u32 ip=dns_resolve(ns);
    if (!ip){ put("[b16] DNS resolve FAILED\n"); done(4); }
    put("[b16] " HOST " -> "); put_ip(ip);

    int hr=http_get(ip);
    if (hr!=0){ done(10+hr); }

    put("[b16] DNS + HTTP GET over SLIRP: PASS\n");
    done(55);
}
