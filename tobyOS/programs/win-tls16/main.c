/* main.c -- a STOCK Win32 console .exe proving PER-THREAD TLS (Track C / C16e).
 * Each CreateThread thread gets its own TEB, so TlsSetValue in one thread does
 * not clobber another's slot. Three workers each store a unique value, sleep so
 * all stores happen before any load, then read back -- each must see ITS OWN
 * value; the main thread's value must also survive untouched.
 *
 * Exit code:
 *   2  TlsAlloc failed
 *   3  the main thread's TLS slot was clobbered (TLS was shared, not per-thread)
 *   4  a worker read back another thread's value (not isolated)
 *  16  per-thread TLS isolation verified
 */

#include <windows.h>

static DWORD g_tls;
static volatile long g_ok[3];

static DWORD WINAPI worker(LPVOID p) {
    int id = (int)(INT_PTR)p;
    TlsSetValue(g_tls, (LPVOID)(INT_PTR)(0x100 + id));
    Sleep(60);                                   /* let all threads store first */
    INT_PTR got = (INT_PTR)TlsGetValue(g_tls);
    g_ok[id] = (got == (INT_PTR)(0x100 + id)) ? 1 : 0;
    return 0;
}

int main(void) {
    g_tls = TlsAlloc();
    if (g_tls == TLS_OUT_OF_INDEXES) return 2;
    TlsSetValue(g_tls, (LPVOID)(INT_PTR)0x999);  /* main thread's value */

    HANDLE h[3];
    for (int i = 0; i < 3; i++)
        h[i] = CreateThread(NULL, 0, worker, (LPVOID)(INT_PTR)i, 0, NULL);
    for (int i = 0; i < 3; i++)
        WaitForSingleObject(h[i], INFINITE);

    if ((INT_PTR)TlsGetValue(g_tls) != 0x999) return 3;
    if (!g_ok[0] || !g_ok[1] || !g_ok[2])      return 4;
    return 16;
}
