/* timesyncd -- the system clock from the network, as systemd-timesyncd
 * keeps it on a Linux distribution (finding 157).
 *
 * tobyOS read the time from the RTC at boot and never corrected it: the
 * EliteDesk's RTC is 7 hours behind UTC, so its clock was too, and nothing
 * could set it -- there was no clock_settime until this batch, and the
 * kernel's NTP client (src/ntp.c) was never called. This is an SNTP client
 * (RFC 4330), a boot service (kernel.c registers it) that:
 *   - waits for the network: until a server's name resolves and answers,
 *     it tries again after RETRY_S seconds, doubling to RETRY_MAX_S;
 *   - asks the server for the time -- a version-4 client request carrying
 *     our transmit time, which the answer must echo as its origin time (or
 *     it is not the answer to this request) -- and takes a reply only from
 *     a synchronized server (leap indicator not 3, stratum 1..15);
 *   - computes the offset ((T2 - T1) + (T3 - T4)) / 2 and, when it is
 *     STEP_MS (0.4 s, systemd-timesyncd's) or more, steps the clock with
 *     clock_settime, saying so as timesyncd does ("Initial clock
 *     synchronization to ..."); a smaller one it SLEWS through adjtimex,
 *     at 500 ppm, so the clock never jumps (finding 280) -- and either way
 *     marks the clock synchronized there (STA_UNSYNC off);
 *   - asks again every POLL_S seconds (timesyncd's longest poll, 2048 s);
 *   - after each sync, sets the RTC from the system clock. On Linux the
 *     kernel does that, every 11 minutes once timesyncd's adjtimex marks
 *     the clock synchronized; this kernel does not, so timesyncd writes
 *     it through /dev/rtc0 as hwclock --systohc does, and says so when the
 *     RTC was a second or more off ("RTC set from the system clock"). The
 *     RTC keeps UTC. Without this the next boot started from the RTC's
 *     wrong time again until the network answered.
 *
 * Servers: NTP= in /etc/timesyncd.conf (space-separated, timesyncd.conf's
 * syntax), else FallbackNTP= there, else pool.ntp.org then
 * time.cloudflare.com. The clock monotonic time never moves; only the wall
 * clock is stepped. It must run as root (CAP_SYS_TIME). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/timex.h>
#include <toby/net.h>

#define NTP_PORT      123
#define NTP_UNIX_OFF  2208988800ull   /* 1900-01-01 to 1970-01-01, s */
#define RETRY_S       10              /* no network yet, or no answer ... */
#define RETRY_MAX_S   320             /* ... doubling to this */
#define POLL_S        2048
#define STEP_MS       400             /* systemd-timesyncd's: smaller is slewed */
#define REPLY_MS      3000

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef long long          i64;

static unsigned short htons_(unsigned short v) { return (unsigned short)((v >> 8) | (v << 8)); }

static i64 now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* nanoseconds since 1970 <-> the 64-bit NTP timestamp (seconds since
 * 1900 . a 32-bit binary fraction) */
static void put_ntp(u8 *p, i64 ns) {
    u64 s = (u64)(ns / 1000000000LL) + NTP_UNIX_OFF;
    u64 f = ((u64)(ns % 1000000000LL) << 32) / 1000000000ull;
    for (int i = 0; i < 4; i++) p[i] = (u8)(s >> (24 - 8 * i));
    for (int i = 0; i < 4; i++) p[4 + i] = (u8)(f >> (24 - 8 * i));
}

static i64 get_ntp(const u8 *p) {
    u64 s = 0, f = 0;
    for (int i = 0; i < 4; i++) s = (s << 8) | p[i];
    for (int i = 0; i < 4; i++) f = (f << 8) | p[4 + i];
    return (i64)(s - NTP_UNIX_OFF) * 1000000000LL + (i64)((f * 1000000000ull) >> 32);
}

/* the servers, from /etc/timesyncd.conf's NTP= or FallbackNTP= */
static int g_nserv;
static char g_serv[8][128];

static void add_servers(const char *list) {
    while (*list && g_nserv < 8) {
        while (*list == ' ' || *list == '\t') list++;
        size_t n = 0;
        while (list[n] && list[n] != ' ' && list[n] != '\t' && list[n] != '\n' && list[n] != '\r') n++;
        if (n == 0) break;
        if (n < sizeof g_serv[0]) {
            memcpy(g_serv[g_nserv], list, n);
            g_serv[g_nserv][n] = 0;
            g_nserv++;
        }
        list += n;
    }
}

static void read_conf(void) {
    char ntp[512] = "", fallback[512] = "";
    FILE *f = fopen("/etc/timesyncd.conf", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (!strncmp(p, "NTP=", 4)) snprintf(ntp, sizeof ntp, "%s", p + 4);
            else if (!strncmp(p, "FallbackNTP=", 12)) snprintf(fallback, sizeof fallback, "%s", p + 12);
        }
        fclose(f);
    }
    add_servers(ntp);
    if (g_nserv == 0) add_servers(fallback);
    if (g_nserv == 0) add_servers("pool.ntp.org time.cloudflare.com");
}

/* ---- the RTC (/dev/rtc0, Linux's ioctls) ---- */
#define RTC_RD_TIME   0x80247009ul
#define RTC_SET_TIME  0x4024700aul
struct rtc_tm { int sec, min, hour, mday, mon, year, wday, yday, isdst; };

/* days from 1970-01-01 to Y-M-D */
static i64 days_from_civil(i64 y, int m, int d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* Set the RTC from the system clock. First how far off it is: read it until
 * its second turns (1.5 s at most), and that second against the system
 * clock then. Then the write, as hwclock --systohc makes it: second S at
 * S + 0.5, the kernel's write putting the RTC's next tick 500 ms later. */
static void rtc_writeback(void) {
    int fd = open("/dev/rtc0", O_RDONLY);
    if (fd < 0) {
        static int said;
        if (!said++)
            fprintf(stderr, "timesyncd: /dev/rtc0: %s; the RTC is left as it is.\n",
                    strerror(errno));
        return;
    }
    struct rtc_tm a, b;
    i64 off = 0;
    int have = 0;
    if (ioctl(fd, RTC_RD_TIME, &a) == 0) {
        i64 start = now_ns();
        while (now_ns() - start < 1500000000LL) {
            if (ioctl(fd, RTC_RD_TIME, &b) != 0) break;
            if (b.sec != a.sec) {
                i64 at = now_ns();
                i64 rs = days_from_civil((i64)b.year + 1900, b.mon + 1, b.mday) * 86400 +
                         b.hour * 3600 + b.min * 60 + b.sec;
                off = rs * 1000000000LL - at;
                have = 1;
                break;
            }
            struct timespec d = { 0, 1000000 };
            nanosleep(&d, NULL);
        }
    }
    /* sleep to 20 ms before the half second, spin to it; a write up to
     * 10 ms late is taken (the RTC then ticks that much late). A busy boot
     * preempts the spin: with a 2 ms window the write took 6 s to land. */
    i64 t;
    for (;;) {
        t = now_ns();
        long frac = (long)(t % 1000000000LL);
        if (frac >= 500000000L && frac < 510000000L) break;
        long wait = frac < 500000000L ? 500000000L - frac : 1500000000L - frac;
        if (wait > 20000000L) {
            struct timespec d = { 0, wait - 20000000L };
            nanosleep(&d, NULL);
        }
    }
    time_t sec = (time_t)(t / 1000000000LL);
    struct tm tm;
    gmtime_r(&sec, &tm);
    struct rtc_tm w = { tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, 0, 0, 0 };
    if (ioctl(fd, RTC_SET_TIME, &w) != 0)
        fprintf(stderr, "timesyncd: Failed to set the RTC: %s\n", strerror(errno));
    else if (!have)
        fprintf(stderr, "timesyncd: RTC set from the system clock (it could not be read first).\n");
    else if (off >= 1000000000LL || off <= -1000000000LL)
        fprintf(stderr, "timesyncd: RTC set from the system clock (it was %+lld s off).\n",
                off / 1000000000LL);
    close(fd);
}

static void fmt_time(i64 ns, char *out, size_t cap) {
    time_t t = (time_t)(ns / 1000000000LL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(out, cap, "%a %Y-%m-%d %H:%M:%S UTC", &tm);
}

/* Tell the kernel the clock is synchronized (STA_UNSYNC off) and, when
 * OFFSET is not 0, slew it by OFFSET ns -- adjtimex's ADJ_OFFSET, ns with
 * ADJ_NANO, as systemd-timesyncd sets it. 0, or -1 with errno. */
static int mark_synced(i64 offset) {
    struct timex tx;
    memset(&tx, 0, sizeof tx);
    tx.modes = ADJ_STATUS | ADJ_NANO | ADJ_MAXERROR | ADJ_ESTERROR;
    tx.status = STA_PLL;
    tx.maxerror = 0;
    tx.esterror = 0;
    if (offset) {
        tx.modes |= ADJ_OFFSET;
        tx.offset = (long)offset;
    }
    return adjtimex(&tx) < 0 ? -1 : 0;
}

/* One exchange with SERVER. 1 when the clock was checked (and stepped or
 * slewed if it was off), 0 when there was no usable answer. */
static int sync_once(const char *server, int *first) {
    u32 ip = 0;
    if (toby_dns_resolve(server, &ip) < 0) return 0;
    int fd = socket(2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0);
    if (fd < 0) return 0;
    int bound = 0;
    for (int t = 0; t < 8 && !bound; t++)
        if (bind(fd, htons_((unsigned short)(40000 + ((getpid() * 31 + t * 977) & 0x3fff)))) >= 0)
            bound = 1;
    if (!bound) { close(fd); return 0; }

    u8 q[48], r[128];
    memset(q, 0, sizeof q);
    q[0] = (0 << 6) | (4 << 3) | 3;          /* LI 0, version 4, client */
    i64 t1 = now_ns();
    put_ntp(q + 40, t1);                      /* our transmit time */
    if (sendto(fd, q, sizeof q, ip, htons_(NTP_PORT)) < 0) { close(fd); return 0; }

    int got = 0;
    i64 t4 = 0;
    for (;;) {
        i64 left = REPLY_MS - (now_ns() - t1) / 1000000LL;
        if (left <= 0) break;
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, (int)left) <= 0) continue;
        u32 from[2];
        long n = recvfrom(fd, r, sizeof r, from);
        t4 = now_ns();
        if (n < 48) continue;
        if (memcmp(r + 24, q + 40, 8) != 0) continue;     /* not our answer */
        got = 1;
        break;
    }
    close(fd);
    if (!got) return 0;
    int li = r[0] >> 6, mode = r[0] & 7, stratum = r[1];
    if (mode != 4 || li == 3 || stratum < 1 || stratum > 15) return 0;
    i64 t2 = get_ntp(r + 32), t3 = get_ntp(r + 40);
    i64 offset = ((t2 - t1) + (t3 - t4)) / 2;
    i64 delay = (t4 - t1) - (t3 - t2);
    char ipb[20];
    snprintf(ipb, sizeof ipb, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
    if (*first)
        fprintf(stderr, "timesyncd: Contacted time server %s:123 (%s).\n", ipb, server);
    i64 a = offset < 0 ? -offset : offset;
    if (a >= STEP_MS * 1000000LL) {
        i64 want = now_ns() + offset;
        struct timespec ts = { (time_t)(want / 1000000000LL), (long)(want % 1000000000LL) };
        if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
            fprintf(stderr, "timesyncd: Failed to set the clock: %s\n", strerror(errno));
            return 0;
        }
        char when[64];
        fmt_time(want, when, sizeof when);
        fprintf(stderr, "timesyncd: %s clock synchronization to %s (offset %+lld ms, delay %lld ms).\n",
                *first ? "Initial" : "Stepped", when, offset / 1000000LL, delay / 1000000LL);
        mark_synced(0);
    } else {
        /* a small offset is SLEWED, as systemd-timesyncd hands it to the
         * kernel's adjtimex: the clock never jumps, backward least of all
         * (finding 280: a step back inside a timed wait made it read
         * short). */
        if (mark_synced(offset) != 0) {
            fprintf(stderr, "timesyncd: Failed to adjust the clock: %s\n", strerror(errno));
            return 0;
        }
        if (*first)
            fprintf(stderr, "timesyncd: Clock is %+lld ms off; slewing it.\n", offset / 1000000LL);
    }
    *first = 0;
    rtc_writeback();
    return 1;
}

int main(int argc, char **argv) {
    int once = argc > 1 && !strcmp(argv[1], "--once");   /* ntpdate-style, for tests */
    read_conf();
    int first = 1;
    unsigned wait = RETRY_S;
    for (;;) {
        int ok = 0;
        for (int i = 0; i < g_nserv && !ok; i++) ok = sync_once(g_serv[i], &first);
        if (once) return ok ? 0 : 1;
        /* no answer: try again, less often each time (to RETRY_MAX_S), so a
         * machine with no network is not asked every few seconds forever */
        if (ok) wait = RETRY_S;
        sleep(ok ? POLL_S : wait);
        if (!ok && wait < RETRY_MAX_S) wait *= 2;
    }
}
