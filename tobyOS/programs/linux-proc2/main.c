/* linux-proc2 -- /proc deep fidelity (2026-08-22): the real maps table,
 * /proc/sys, /proc/net, and the status fields tools parse.
 *
 *   bit0  /proc/self/maps has [heap] and [stack] and is sorted ascending
 *   bit1  a FRESH anonymous mmap appears in maps -- the old three-line
 *         fabrication listed no mmap region ever
 *   bit2  /proc/sys/kernel/osrelease and hostname read real values
 *   bit3  a LISTEN socket we create appears in /proc/net/tcp with state
 *         0A and our (hex) port -- netstat/ss stop being blind
 *   bit4  status: Tgid equals getpid() and Threads counts a live sibling
 *   bit5  VmRSS is REAL: it GROWS when we map and touch a megabyte (the
 *         old constant-at-spawn RSS could never move)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static long read_all(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, buf, cap - 1);
    close(fd);
    if (n >= 0) buf[n] = 0;
    return n;
}

static long status_field(const char *st, const char *key) {
    const char *k = strstr(st, key);
    if (!k) return -1;
    k += strlen(key);
    while (*k == ' ' || *k == '\t') k++;
    return atol(k);
}

static volatile int g_park = 1;
static void *parker(void *a) { (void)a; while (g_park) usleep(10 * 1000); return 0; }

int main(void) {
    int bits = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    static char big[8192];

    /* ---- bit0: heap+stack present, sorted ---- */
    {
        long n = read_all("/proc/self/maps", big, sizeof big);
        int has_heap = strstr(big, "[heap]") != 0;
        int has_stack = strstr(big, "[stack]") != 0;
        int sorted = 1, lines = 0;
        unsigned long long prev = 0;
        for (char *l = big; l && *l; ) {
            unsigned long long lo = strtoull(l, 0, 16);
            if (lo < prev) sorted = 0;
            prev = lo;
            lines++;
            l = strchr(l, '\n'); if (l) l++;
        }
        printf("proc2: maps n=%ld lines=%d heap=%d stack=%d sorted=%d\n",
               n, lines, has_heap, has_stack, sorted);
        if (n > 0 && has_heap && has_stack && sorted && lines >= 3) bits |= 1;
    }

    /* ---- bit1: a fresh mmap appears ---- */
    {
        void *m = mmap(0, 64 * 1024, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = 0;
        if (m != MAP_FAILED) {
            read_all("/proc/self/maps", big, sizeof big);
            char want[32];
            snprintf(want, sizeof want, "%llx-", (unsigned long long)(uintptr_t)m);
            ok = strstr(big, want) != 0;
            printf("proc2: mmap %p in maps=%d\n", m, ok);
            munmap(m, 64 * 1024);
        }
        if (ok) bits |= 2;
    }

    /* ---- bit2: /proc/sys ---- */
    {
        char rel[64] = {0}, hn[80] = {0};
        long a = read_all("/proc/sys/kernel/osrelease", rel, sizeof rel);
        long b = read_all("/proc/sys/kernel/hostname", hn, sizeof hn);
        printf("proc2: osrelease='%.*s' hostname_len=%ld\n",
               (int)(a > 1 ? a - 1 : 0), rel, b);
        if (a > 0 && strncmp(rel, "6.1.0-tobyos", 12) == 0 && b > 1) bits |= 4;
    }

    /* ---- bit3: /proc/net/tcp shows our listener ---- */
    {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in la;
        memset(&la, 0, sizeof la);
        la.sin_family = AF_INET;
        la.sin_port = htons(35000);                 /* 0x88B8 */
        la.sin_addr.s_addr = inet_addr("127.0.0.1");
        int ok = 0;
        if (ls >= 0 && bind(ls, (void *)&la, sizeof la) == 0 &&
            listen(ls, 2) == 0) {
            read_all("/proc/net/tcp", big, sizeof big);
            char *hit = strstr(big, ":88B8");
            ok = (hit != 0 && strstr(hit, " 0A ") != 0);
            printf("proc2: net/tcp listener hit=%d\n", ok);
        }
        if (ls >= 0) close(ls);
        if (ok) bits |= 8;
    }

    /* ---- bit4: Tgid + Threads ---- */
    {
        pthread_t t;
        int ok = 0;
        if (pthread_create(&t, 0, parker, 0) == 0) {
            usleep(50 * 1000);
            read_all("/proc/self/status", big, sizeof big);
            long tgid = status_field(big, "Tgid:");
            long nth  = status_field(big, "Threads:");
            printf("proc2: Tgid=%ld (self=%d) Threads=%ld\n",
                   tgid, (int)getpid(), nth);
            ok = (tgid == getpid() && nth >= 2);
            g_park = 0;
            pthread_join(t, 0);
        }
        if (ok) bits |= 16;
    }

    /* ---- bit5: VmRSS moves ---- */
    {
        read_all("/proc/self/status", big, sizeof big);
        long before = status_field(big, "VmRSS:");
        size_t mb = 1024 * 1024;
        volatile char *m = mmap(0, mb, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int ok = 0;
        if (m != MAP_FAILED) {
            for (size_t i = 0; i < mb; i += 4096) m[i] = 1;   /* touch */
            read_all("/proc/self/status", big, sizeof big);
            long after = status_field(big, "VmRSS:");
            printf("proc2: VmRSS %ld -> %ld kB (want +>=900)\n", before, after);
            ok = (before > 0 && after >= before + 900);
            munmap((void *)m, mb);
        }
        if (ok) bits |= 32;
    }

    printf("LXPROC2: VERDICT bits=%d (63=all)\n", bits);
    return bits;
}
