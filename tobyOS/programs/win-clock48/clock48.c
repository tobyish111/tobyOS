/* win-clock48 -- Track C, W7q: kernel32's clocks are one clock. On Windows
 * QueryPerformanceFrequency is 10 MHz, QueryPerformanceCounter and
 * GetTickCount64 count from the same start (the boot), GetTickCount is
 * GetTickCount64's low 32 bits, a message's time is GetTickCount's, time()
 * is the system time's seconds and clock() the program's milliseconds, and all
 * of them move with Sleep: a Sleep(200) is 200 ms on each, and 300 ms of
 * one while the program spins is 300 ms of the others.
 * (On tobyOS GetTickCount and QueryPerformanceCounter counted the PIT's
 * interrupts, which QEMU drops when the host is busy -- a Sleep(150) read as
 * about 100 ms -- while Sleep and the system time use the kernel's
 * nanosecond clock.) Each check is what Windows 11 does (measured first:
 * this program exits 50 there). Exit 50 iff all pass, else 100 + the first
 * check that failed. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int first_fail;
static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !first_fail) first_fail = n;
}
static ULONGLONG ft_now(void) {               /* the system time, 100 ns units */
    FILETIME f; GetSystemTimeAsFileTime(&f);
    return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime;
}
static LONGLONG qpc(void) { LARGE_INTEGER v; QueryPerformanceCounter(&v); return v.QuadPart; }

int main(void) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    printf("     QueryPerformanceFrequency %lld\n", (long long)f.QuadPart);
    check(1, f.QuadPart == 10000000, "QueryPerformanceFrequency is 10 MHz");

    /* 2: a Sleep(200) on each clock (not the system time: a clock set while
     * the program runs -- tobyOS's timesyncd at the boot -- moves it) */
    LONGLONG q0 = qpc(); DWORD t0 = GetTickCount(); ULONGLONG s0 = GetTickCount64();
    Sleep(200);
    LONGLONG q1 = qpc(); DWORD t1 = GetTickCount(); ULONGLONG s1 = GetTickCount64();
    long dq = (long)((q1 - q0) * 1000 / f.QuadPart), dt = (long)(t1 - t0), ds = (long)(s1 - s0);
    printf("     Sleep(200): QueryPerformanceCounter %ld ms, GetTickCount %ld, GetTickCount64 %ld\n", dq, dt, ds);
    check(2, dq >= 199 && dq < 2000 && dt >= 184 && dt < 2000 && ds >= 184 && ds < 2000,
          "a Sleep(200) is 200 ms of QueryPerformanceCounter, GetTickCount and GetTickCount64");

    /* 3: 300 ms of QueryPerformanceCounter, spinning */
    q0 = qpc(); t0 = GetTickCount(); s0 = GetTickCount64();
    while ((qpc() - q0) * 1000 / f.QuadPart < 300) { }
    q1 = qpc(); t1 = GetTickCount(); s1 = GetTickCount64();
    dq = (long)((q1 - q0) * 1000 / f.QuadPart); dt = (long)(t1 - t0); ds = (long)(s1 - s0);
    printf("     300 ms of QueryPerformanceCounter, spinning: GetTickCount %ld, GetTickCount64 %ld (QueryPerformanceCounter %ld)\n", dt, ds, dq);
    check(3, dt >= dq - 20 && dt <= dq + 20 && ds >= dq - 20 && ds <= dq + 20,
          "while the program spins, GetTickCount and GetTickCount64 move with QueryPerformanceCounter");

    /* 4: GetTickCount is GetTickCount64's low half; QueryPerformanceCounter counts from the same start */
    ULONGLONG a = GetTickCount64(); DWORD b = GetTickCount(); ULONGLONG c = GetTickCount64();
    LONGLONG qn = qpc(); ULONGLONG sn = GetTickCount64();
    long long off = (long long)(qn / (f.QuadPart / 1000)) - (long long)sn;
    printf("     GetTickCount64 %llu, GetTickCount %lu, GetTickCount64 %llu; QueryPerformanceCounter in ms - GetTickCount64: %lld\n",
           a, (unsigned long)b, c, off);
    check(4, (DWORD)a <= b + 1 && b <= (DWORD)c + 1 && c >= b && off > -100 && off < 100,
          "GetTickCount is GetTickCount64's low 32 bits; QueryPerformanceCounter counts from the same start");

    /* 5: a message's time is GetTickCount's */
    MSG m;
    PeekMessageA(&m, 0, 0, 0, PM_NOREMOVE);          /* the thread's queue */
    DWORD before = GetTickCount();
    PostThreadMessageA(GetCurrentThreadId(), WM_APP, 0, 0);
    Sleep(50);
    BOOL got = PeekMessageA(&m, 0, WM_APP, WM_APP, PM_REMOVE);
    DWORD after = GetTickCount();
    LONG gm = GetMessageTime();
    printf("     a message posted at %lu..%lu has the time %lu (GetMessageTime %ld)\n",
           (unsigned long)before, (unsigned long)after, (unsigned long)m.time, (long)gm);
    check(5, got && m.time + 16 >= before && m.time <= after + 16 && (DWORD)gm == m.time,
          "a posted message's time is GetTickCount's at the post; GetMessageTime is the last message's");

    /* 6: time() and clock() */
    ULONGLONG ft = ft_now();
    time_t tt = time(NULL);
    long long unix_ft = (long long)(ft / 10000000ULL) - 11644473600LL;
    clock_t c0 = clock();
    Sleep(200);
    clock_t c1 = clock();
    printf("     time() - the system time's seconds since 1970: %lld; clock() %ld, %ld more over a Sleep(200), CLOCKS_PER_SEC %ld\n",
           (long long)tt - unix_ft, (long)c0, (long)(c1 - c0), (long)CLOCKS_PER_SEC);
    check(6, (long long)tt - unix_ft >= -1 && (long long)tt - unix_ft <= 1 && c0 >= 0 && c0 < 10000 &&
             c1 - c0 >= 190 && c1 - c0 < 2000 && CLOCKS_PER_SEC == 1000,
          "time() is the system time's seconds since 1970; clock() counts the program's milliseconds from its start");

    if (first_fail) { printf("FAILED (first: %d)\n", first_fail); return 100 + first_fail; }
    printf("ALL PASS\n");
    return 50;
}
