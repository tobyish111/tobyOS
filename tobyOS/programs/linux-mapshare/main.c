/* programs/linux-mapshare/main.c
 *
 * Targeted unit test for the remaining Chromium DOM blocker (ledger slice 33):
 * chrome reports VALIDATION_ERROR_ILLEGAL_MEMORY_RANGE on a Mojo/ipcz parcel
 * that lives in a MAP_SHARED shared-memory FRAGMENT -- even though the socket
 * bytes are perfect and the [shm] log says the region "attached". The paradox:
 * if two processes truly map the SAME physical pages, a reader sees exactly what
 * the writer wrote. This proves (or breaks) that assumption in ISOLATION, in
 * chrome's exact shape:
 *
 *   parent: openat(O_RDWR|O_CREAT) a temp file, ftruncate(8192),
 *           mmap#1 MAP_SHARED, write MAGIC1, then UNLINK the path
 *           (chrome unlinks the region immediately after creating it),
 *           then fork().
 *   child : independently mmap#2 MAP_SHARED the SAME inherited fd, then
 *           (a) READ mmap2 -> must see parent's MAGIC1  (forward coherence)
 *           (b) WRITE MAGIC2 into mmap2,
 *           (c) POLL for the parent's POST-FORK write MAGIC3 (see below),
 *           (d) WRITE MAGIC4 back, exit.
 *   parent: AFTER fork, WRITE MAGIC3 through the PRE-FORK mapping #1 --
 *           this is chrome's exact hot path: the browser maps NodeLinkMemory
 *           BEFORE forking children and keeps WRITING parcels into it AFTER.
 *           A CoW-at-fork kernel silently diverts this write into a PRIVATE
 *           COPY (fork write-protects every writable page; the COW fault
 *           handler sees refcount>1 -- the shm cache's own ref guarantees
 *           that -- and copies), so the child never sees MAGIC3.
 *           Then wait for child, then READ mmap1 -> must see MAGIC2 + MAGIC4.
 *
 * Two INDEPENDENT mmaps of one unlinked file in two processes must be coherent
 * BOTH directions, INCLUDING writes made after a fork() -- exactly what ipcz
 * NodeLinkMemory fragments require of the browser (the ipcz broker).
 *
 * exit_group():
 *   3 = PASS  (coherent both directions incl. post-fork writes)
 *   1 = FAIL  (forward: child's independent mmap did NOT see parent's write)
 *   4 = FAIL  (reverse: parent did NOT see child's pre-exit write)
 *   6 = FAIL  (post-fork: parent's write AFTER fork() never became visible to
 *              the child -- the CoW-divergence bug, chrome's DOM blocker)
 *   7 = FAIL  (post-fork reverse: child's late write invisible to parent --
 *              the parent is reading a diverged private copy)
 *   2 = ERROR (open/ftruncate/mmap/fork setup problem)
 */

typedef unsigned long u64;
typedef long          i64;

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

#define SYS_write        1
#define SYS_openat       257
#define SYS_close        3
#define SYS_ftruncate    77
#define SYS_mmap         9
#define SYS_unlink       87
#define SYS_clone        56
#define SYS_wait4        61
#define SYS_exit         60
#define SYS_exit_group   231
#define SYS_nanosleep    35

#define AT_FDCWD        (-100)
#define O_RDWR           2
#define O_CREAT          0100
#define O_TRUNC          01000
#define PROT_READ        1
#define PROT_WRITE       2
#define MAP_SHARED       1
#define SIGCHLD          17

#define MAGIC1 0xCAFEBABEu
#define MAGIC2 0xF00DF00Du
#define MAGIC3 0xBEEFBEEFu      /* parent's POST-FORK write (the CoW trap) */
#define MAGIC4 0x0DDB0B0Bu      /* child's reply after seeing MAGIC3 */
#define MAPLEN 8192

struct timespec { i64 sec; i64 nsec; };
static void put(const char *s){ long n=0; while(s[n])n++; SC3(SYS_write,1,s,n); }
static void msleep(long ms){ struct timespec t={ms/1000,(ms%1000)*1000000L}; SC2(SYS_nanosleep,&t,0); }
static void puthex(u64 v){
    char b[19]; b[0]='0'; b[1]='x'; for(int i=0;i<16;i++){int s=(15-i)*4;
        int d=(int)((v>>s)&0xf); b[2+i]=(char)(d<10?'0'+d:'a'+d-10);} b[18]='\n';
    SC3(SYS_write,1,b,19);
}

extern void _start(void);
__asm__(".globl _start\n_start:\n  call main_c\n  mov %rax,%rdi\n  mov $231,%rax\n  syscall\n");

int main_c(void);
int main_c(void) {
    put("[maptest] start\n");

    const char *path = "/data/mapshare.bin";
    int fd = (int)lx6(SYS_openat, AT_FDCWD, (i64)path, O_RDWR|O_CREAT|O_TRUNC, 0600, 0, 0);
    if (fd < 0) { put("[maptest] openat FAILED\n"); return 2; }
    if (SC2(SYS_ftruncate, fd, MAPLEN) < 0) { put("[maptest] ftruncate FAILED\n"); return 2; }

    /* mmap#1 -- parent's mapping. */
    i64 m1 = lx6(SYS_mmap, 0, MAPLEN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (m1 < 0 || m1 == 0) { put("[maptest] mmap#1 FAILED\n"); return 2; }
    volatile unsigned *p1 = (volatile unsigned *)(unsigned long)m1;
    p1[0] = MAGIC1;                     /* parent writes forward magic */

    /* chrome UNLINKS the region file immediately after creating it -- the fd
     * (and its mapping) must keep working, and a second independent mmap of the
     * same inode identity must still share. Stress that here. */
    SC1(SYS_unlink, (i64)path);

    long pid = lx6(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);   /* fork() */
    if (pid < 0) { put("[maptest] clone/fork FAILED\n"); return 2; }

    if (pid == 0) {
        /* ---- CHILD ---- independent second mmap of the inherited fd ---- */
        i64 m2 = lx6(SYS_mmap, 0, MAPLEN, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
        if (m2 < 0 || m2 == 0) { put("[maptest] child mmap#2 FAILED\n"); SC1(SYS_exit,2); }
        volatile unsigned *p2 = (volatile unsigned *)(unsigned long)m2;
        unsigned saw = p2[0];
        if (saw != MAGIC1) {
            put("[maptest] child: forward-coherence FAIL, mmap2[0]=");
            puthex(saw);
            SC1(SYS_exit, 1);           /* child conveys forward failure via 1 */
        }
        p2[1] = MAGIC2;                 /* child writes reverse magic */

        /* Poll for the parent's POST-FORK write (chrome's browser-keeps-
         * writing-after-fork shape). On a CoW-at-fork kernel the parent's
         * write lands in a diverged private copy and this never appears. */
        int seen = 0;
        for (int i = 0; i < 400; i++) {         /* up to ~2 s */
            if (p2[2] == MAGIC3) { seen = 1; break; }
            msleep(5);
        }
        if (!seen) {
            put("[maptest] child: parent's POST-FORK write never arrived, mmap2[2]=");
            puthex(p2[2]);
            SC1(SYS_exit, 6);
        }
        p2[3] = MAGIC4;                 /* reply so the parent can check its view */
        SC1(SYS_exit, 0);               /* clean: fwd OK, rev written, post-fork OK */
    }

    /* ---- PARENT ---- the CoW trap: write through the PRE-FORK mapping AFTER
     * the fork. If fork write-protected this page for CoW, this write faults
     * and (refcount>1, the cache holds a ref) gets diverted into a private
     * copy invisible to the child. */
    msleep(50);
    p1[2] = MAGIC3;

    int status = 0;
    lx6(SYS_wait4, pid, (i64)&status, 0, 0, 0, 0);
    int cexit = (status >> 8) & 0xff;   /* WEXITSTATUS */

    if (cexit == 1) { put("[maptest] FAIL: child's independent mmap did NOT see parent's write\n"); return 1; }
    if (cexit == 2) { put("[maptest] ERROR: child setup failed\n"); return 2; }
    if (cexit == 6) { put("[maptest] FAIL: POST-FORK parent write invisible to child (CoW divergence)\n"); return 6; }

    unsigned saw = p1[1];               /* did parent's mapping see child's write? */
    if (saw != MAGIC2) {
        put("[maptest] FAIL: parent did NOT see child's write, mmap1[1]=");
        puthex(saw);
        return 4;
    }
    saw = p1[3];                        /* child's LATE write, after parent's
                                         * own post-fork write: if the parent
                                         * diverged, its copy misses this. */
    if (saw != MAGIC4) {
        put("[maptest] FAIL: child's late write invisible to parent (parent on diverged copy), mmap1[3]=");
        puthex(saw);
        return 7;
    }
    put("[maptest] PASS: MAP_SHARED coherent both directions incl. post-fork writes\n");
    return 3;
}
