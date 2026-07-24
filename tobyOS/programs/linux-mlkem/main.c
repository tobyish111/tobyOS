/* programs/linux-mlkem/main.c
 *
 * Boot unit test MLKEMTEST (ledger slice 38): does the ML-KEM/Kyber-768
 * computational shape (NTT mod-3329 + Keccak + FO decapsulation -- the exact
 * shape chrome's BoringSSL X25519MLKEM768 dies in) compute CORRECTLY on
 * tobyOS?  Three processes run the deterministic self-test CONCURRENTLY so
 * the computation is preempted constantly (context-switch / FPU-state /
 * demand-paging pressure) -- chrome's network service computes ML-KEM on a
 * preempted thread, and under TCG the computation spans many timeslices.
 *
 *   parent: seed 1, children: seeds 2 and 3, all 40 rounds each of
 *   keygen -> encaps -> decaps(==) -> corrupted-decaps(!=), FNV-chained
 *   checksum printed per process (compare against the HOST reference build:
 *   programs/linux-mlkem/host_main.c -- same code, same seeds).
 *
 * exit_group():
 *   3 = PASS  (all 3 processes self-consistent, all rounds)
 *   1 = FAIL  (self-consistency: decaps != encaps secret -- chrome's shape)
 *   4 = FAIL  (implicit rejection: corrupted ct decapsed to the SAME secret)
 *   5 = FAIL  (a child failed; see its own line)
 *   2 = ERROR (fork failed)
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

#define SYS_write        1
#define SYS_clone        56
#define SYS_wait4        61
#define SYS_exit         60
#define SIGCHLD          17

/* ---- freestanding mem* (the kyber ref + compiler lowering need them) ---- */
#include <stddef.h>
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d < s) for (size_t i = 0; i < n; i++) d[i] = s[i];
    else       for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    return 0;
}

static void put(const char *s){ long n=0; while(s[n])n++; SC3(SYS_write,1,s,n); }
static void puthex(u64 v){
    char b[19]; b[0]='0'; b[1]='x'; for(int i=0;i<16;i++){int s=(15-i)*4;
        int d=(int)((v>>s)&0xf); b[2+i]=(char)(d<10?'0'+d:'a'+d-10);} b[18]='\n';
    SC3(SYS_write,1,b,19);
}
static void putdec(long v){
    char b[24]; int i=23; b[i--]='\n';
    if (v==0) b[i--]='0';
    int neg = v<0; if(neg) v=-v;
    while(v){ b[i--]='0'+(int)(v%10); v/=10; }
    if(neg) b[i--]='-';
    SC3(SYS_write,1,b+i+1,23-i-1+1);
}

struct mlkem_result { u64 checksum; int fail_round; };
int mlkem_selftest(u64 seed, int rounds, struct mlkem_result *res);

#define ROUNDS 40

static int run_one(u64 seed, const char *tag) {
    struct mlkem_result r;
    int rc = mlkem_selftest(seed, ROUNDS, &r);
    put(tag);
    if (rc == 0) { put(" PASS checksum="); puthex(r.checksum); }
    else {
        put(rc == 1 ? " FAIL self-consistency at round " :
                      " FAIL implicit-rejection at round ");
        putdec(r.fail_round);
        put(" partial-checksum="); puthex(r.checksum);
    }
    return rc;
}

extern void _start(void);
__asm__(".globl _start\n_start:\n  call main_c\n  mov %rax,%rdi\n  mov $231,%rax\n  syscall\n");

int main_c(void);
int main_c(void) {
    put("[mlkem] start: kyber768 ref, 3 procs x 40 rounds concurrent\n");

    long kids[2]; int nkids = 0;
    for (int k = 0; k < 2; k++) {
        long pid = lx6(SYS_clone, SIGCHLD, 0, 0, 0, 0, 0);   /* fork() */
        if (pid < 0) { put("[mlkem] fork FAILED\n"); return 2; }
        if (pid == 0) {
            int rc = run_one(k == 0 ? 2 : 3,
                             k == 0 ? "[mlkem] child-A(seed2)" :
                                      "[mlkem] child-B(seed3)");
            SC1(SYS_exit, rc == 0 ? 3 : rc);
        }
        kids[nkids++] = pid;
    }

    int prc = run_one(1, "[mlkem] parent(seed1)");

    int worst = 0;
    for (int k = 0; k < nkids; k++) {
        int status = 0;
        lx6(SYS_wait4, kids[k], (i64)&status, 0, 0, 0, 0);
        int ec = (status >> 8) & 0xff;
        if (ec != 3 && worst == 0) worst = 5;
    }
    if (prc != 0) return prc;
    if (worst)    return worst;
    put("[mlkem] PASS: all 3 processes self-consistent, all rounds\n");
    return 3;
}
