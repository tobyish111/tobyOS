/* programs/linux-msgsock/main.c
 *
 * A GENUINE Linux x86-64 static ELF (raw Linux syscalls, no libc) that proves
 * tobyOS's B18 socket-depth additions: a REAL getsockopt (SO_TYPE / SO_ERROR)
 * and scatter-gather sendmsg/recvmsg -- the syscalls real DNS/HTTP libraries
 * use. It re-runs the B16 resolve-then-fetch flow, but every socket transfer
 * goes through sendmsg/recvmsg with MULTIPLE iovecs, and it asserts getsockopt
 * answers along the way.
 *
 * Flow (all raw syscalls, would run unmodified on real Linux):
 *   1. socket(UDP); getsockopt(SO_TYPE)==SOCK_DGRAM, getsockopt(SO_ERROR)==0.
 *   2. sendmsg a DNS A query split across 2 iovecs (header + question) to
 *      ns:53; recvmsg the reply scattered across 2 iovecs; parse the A record.
 *   3. socket(TCP); connect(serverIP:80); getsockopt(SO_ERROR)==0;
 *      sendmsg "GET /" (2 iovecs); recvmsg the reply; check it starts "HTTP/".
 *
 * Exit 56 only if the getsockopt assertions hold AND the DNS+HTTP round-trip
 * succeeds. Over QEMU SLIRP this reaches the REAL internet. Codes 2..29
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
#define SYS_sendmsg   46
#define SYS_recvmsg   47
#define SYS_setsockopt 54
#define SYS_getsockopt 55
#define SYS_exit_group 231

#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOL_SOCKET   1
#define SO_TYPE      3
#define SO_ERROR     4
#define SO_RCVTIMEO  20

#define HOST "example.com"

struct sockaddr_in { u16 sin_family; u16 sin_port; u32 sin_addr; u8 pad[8]; };
struct iovec  { void *iov_base; u64 iov_len; };
struct msghdr {
    void *msg_name;  u32 msg_namelen; u32 _p0;
    struct iovec *msg_iov; u64 msg_iovlen;
    void *msg_control; u64 msg_controllen;
    int msg_flags; u32 _p1;
};

static u64 slen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
static void put(const char *s){ sc(SYS_write,1,(i64)s,(i64)slen(s),0,0,0); }
static u16 hns(u16 p){ return (u16)((p<<8)|(p>>8)); }

static void put_ip(u32 ip_be){
    u8 *b=(u8*)&ip_be; char out[16]; int o=0;
    for (int i=0;i<4;i++){ int v=b[i]; char t[3]; int ti=0;
        if(v==0)t[ti++]='0'; else{ while(v){t[ti++]='0'+v%10; v/=10;} }
        while(ti)out[o++]=t[--ti]; if(i<3)out[o++]='.'; }
    out[o++]='\n'; out[o]=0; put(out);
}
static u32 parse_ip(const char *s){
    u32 oct[4]={0,0,0,0}; int idx=0, any=0;
    for (const char *p=s; *p; p++){
        if(*p>='0'&&*p<='9'){ oct[idx]=oct[idx]*10+(u32)(*p-'0'); any=1; }
        else if(*p=='.'){ if(++idx>3) return 0; } else break; }
    if(!any||idx!=3) return 0;
    return (oct[0])|(oct[1]<<8)|(oct[2]<<16)|(oct[3]<<24);
}
static u32 resolver_ip(void){
    u32 fb=(10)|(0<<8)|(2<<16)|(3<<24);
    int fd=(int)sc(SYS_open,(i64)"/etc/resolv.conf",0,0,0,0,0);
    if(fd<0) return fb;
    char buf[512]; long r=sc(SYS_read,fd,(i64)buf,sizeof(buf)-1,0,0,0);
    sc(SYS_close,fd,0,0,0,0,0);
    if(r<=0) return fb; buf[r]=0;
    const char *key="nameserver";
    for(long i=0;i<r;i++){ int m=1;
        for(int k=0;key[k];k++){ if(buf[i+k]!=key[k]){m=0;break;} }
        if(m){ long j=i+10; while(j<r&&(buf[j]==' '||buf[j]=='\t'))j++;
               u32 ip=parse_ip(&buf[j]); if(ip) return ip; } }
    return fb;
}
static int dns_encode(const char *name,u8 *out){
    int o=0; const char *p=name;
    while(*p){ const char *s=p; while(*p&&*p!='.')p++; int len=(int)(p-s);
        out[o++]=(u8)len; for(int i=0;i<len;i++)out[o++]=(u8)s[i]; if(*p=='.')p++; }
    out[o++]=0; return o;
}
static int dns_skip_name(const u8 *b,int len,int off){
    while(off<len){ u8 c=b[off]; if(c==0)return off+1;
        if((c&0xC0)==0xC0)return off+2; off+=c+1; }
    return -1;
}

/* getsockopt one int. Returns the value, or -1 on syscall error. */
static i64 getopt_int(int s,int name){
    int v=0; u32 l=4;
    i64 rc=sc(SYS_getsockopt,s,SOL_SOCKET,name,(i64)&v,(i64)&l,0);
    if(rc<0) return -1;
    return v;
}

/* sendmsg over 2 iovecs (hdr + body). dest may be 0 (connected TCP). */
static long send2(int s,const void *p0,u64 l0,const void *p1,u64 l1,
                  struct sockaddr_in *dst){
    struct iovec iov[2]={ {(void*)p0,l0}, {(void*)p1,l1} };
    struct msghdr mh; for(u64 i=0;i<sizeof(mh);i++) ((u8*)&mh)[i]=0;
    mh.msg_iov=iov; mh.msg_iovlen= l1? 2:1;
    if(dst){ mh.msg_name=dst; mh.msg_namelen=16; }
    return sc(SYS_sendmsg,s,(i64)&mh,0,0,0,0);
}
/* recvmsg scattering into 2 iovecs over a single contiguous buffer. */
static long recv2(int s,void *buf,u64 split,u64 cap){
    struct iovec iov[2]={ {buf,split}, {(u8*)buf+split,cap-split} };
    struct msghdr mh; for(u64 i=0;i<sizeof(mh);i++) ((u8*)&mh)[i]=0;
    mh.msg_iov=iov; mh.msg_iovlen=2;
    return sc(SYS_recvmsg,s,(i64)&mh,0,0,0,0);
}

static u32 dns_resolve(u32 ns_ip_be){
    int s=(int)sc(SYS_socket,AF_INET,SOCK_DGRAM,0,0,0,0);
    if(s<0){ put("[b18] udp socket FAIL\n"); return 0; }

    i64 ty=getopt_int(s,SO_TYPE), er=getopt_int(s,SO_ERROR);
    if(ty!=SOCK_DGRAM){ put("[b18] SO_TYPE != SOCK_DGRAM\n"); sc(SYS_close,s,0,0,0,0,0); return 0; }
    if(er!=0){ put("[b18] SO_ERROR != 0 on fresh udp\n"); sc(SYS_close,s,0,0,0,0,0); return 0; }
    put("[b18] udp getsockopt OK (SO_TYPE=DGRAM, SO_ERROR=0)\n");

    i64 tv[2]={5,0};
    sc(SYS_setsockopt,s,SOL_SOCKET,SO_RCVTIMEO,(i64)tv,16,0);

    u8 hdr[12]; int ho=0;
    u16 id=0x1d18;
    hdr[ho++]=(u8)(id>>8); hdr[ho++]=(u8)id;
    hdr[ho++]=0x01; hdr[ho++]=0x00;   /* RD=1 */
    hdr[ho++]=0; hdr[ho++]=1;          /* QDCOUNT=1 */
    hdr[ho++]=0; hdr[ho++]=0; hdr[ho++]=0; hdr[ho++]=0; hdr[ho++]=0; hdr[ho++]=0;
    u8 qn[300]; int qo=dns_encode(HOST,qn);
    qn[qo++]=0; qn[qo++]=1;            /* QTYPE=A */
    qn[qo++]=0; qn[qo++]=1;            /* QCLASS=IN */

    struct sockaddr_in ns; for(int i=0;i<(int)sizeof(ns);i++)((u8*)&ns)[i]=0;
    ns.sin_family=AF_INET; ns.sin_port=hns(53); ns.sin_addr=ns_ip_be;

    /* sendmsg: header + question as two iovecs. */
    if(send2(s,hdr,12,qn,(u64)qo,&ns)<0){ put("[b18] dns sendmsg FAIL\n"); sc(SYS_close,s,0,0,0,0,0); return 0; }

    u8 resp[600];
    long n=recv2(s,resp,12,sizeof(resp));   /* scatter: [0..12) then [12..) */
    sc(SYS_close,s,0,0,0,0,0);
    if(n<12){ put("[b18] dns recvmsg FAIL/timeout\n"); return 0; }

    int an=((int)resp[6]<<8)|resp[7];
    int off=12; off=dns_skip_name(resp,(int)n,off);
    if(off<0||off+4>(int)n) return 0; off+=4;
    for(int i=0;i<an;i++){
        off=dns_skip_name(resp,(int)n,off);
        if(off<0||off+10>(int)n) return 0;
        int type=((int)resp[off]<<8)|resp[off+1];
        int cls =((int)resp[off+2]<<8)|resp[off+3];
        int rdl =((int)resp[off+8]<<8)|resp[off+9];
        off+=10; if(off+rdl>(int)n) return 0;
        if(type==1&&cls==1&&rdl==4){ u32 ip; for(int k=0;k<4;k++)((u8*)&ip)[k]=resp[off+k]; return ip; }
        off+=rdl;
    }
    return 0;
}

static int http_get(u32 server_ip_be){
    int s=(int)sc(SYS_socket,AF_INET,SOCK_STREAM,0,0,0,0);
    if(s<0){ put("[b18] tcp socket FAIL\n"); return 2; }
    struct sockaddr_in sa; for(int i=0;i<(int)sizeof(sa);i++)((u8*)&sa)[i]=0;
    sa.sin_family=AF_INET; sa.sin_port=hns(80); sa.sin_addr=server_ip_be;
    if(sc(SYS_connect,s,(i64)&sa,16,0,0,0)!=0){ put("[b18] connect FAIL\n"); sc(SYS_close,s,0,0,0,0,0); return 3; }

    i64 er=getopt_int(s,SO_ERROR);
    if(er!=0){ put("[b18] SO_ERROR != 0 after connect\n"); sc(SYS_close,s,0,0,0,0,0); return 7; }
    put("[b18] connected; SO_ERROR=0; sending GET via sendmsg\n");

    const char *line="GET / HTTP/1.0\r\nHost: " HOST "\r\n";
    const char *tail="Connection: close\r\nUser-Agent: tobyOS-b18\r\n\r\n";
    if(send2(s,line,slen(line),tail,slen(tail),0)<0){ put("[b18] http sendmsg FAIL\n"); sc(SYS_close,s,0,0,0,0,0); return 4; }

    char buf[256];
    long n=recv2(s,buf,16,sizeof(buf)-1);    /* scatter recv */
    sc(SYS_close,s,0,0,0,0,0);
    if(n<5){ put("[b18] http recvmsg FAIL\n"); return 5; }
    buf[n]=0;
    if(buf[0]=='H'&&buf[1]=='T'&&buf[2]=='T'&&buf[3]=='P'&&buf[4]=='/'){
        int e=0; while(e<(int)n&&buf[e]!='\r'&&buf[e]!='\n')e++;
        put("[b18] reply: "); sc(SYS_write,1,(i64)buf,e,0,0,0); put("\n");
        return 0;
    }
    put("[b18] reply not HTTP\n");
    return 6;
}

__attribute__((noreturn)) static void done(int c){ sc(SYS_exit_group,c,0,0,0,0,0); for(;;){} }

__attribute__((force_align_arg_pointer))
void _start(void){
    u32 ns=resolver_ip();
    put("[b18] nameserver "); put_ip(ns);
    u32 ip=dns_resolve(ns);
    if(!ip){ put("[b18] DNS resolve FAILED\n"); done(4); }
    put("[b18] " HOST " -> "); put_ip(ip);
    int hr=http_get(ip);
    if(hr!=0){ done(10+hr); }
    put("[b18] getsockopt + sendmsg/recvmsg DNS+HTTP over SLIRP: PASS\n");
    done(56);
}
