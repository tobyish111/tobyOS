/* main.c -- a STOCK multithreaded Windows .exe (Track C / C6). Built with a
 * plain `clang main.c -o win-thread.exe`. It spawns NT worker threads with
 * CreateThread; each increments a SHARED counter NI times under a
 * CRITICAL_SECTION (real mutual exclusion); main WaitForSingleObject-joins
 * them all and checks the total. A correct total (NT*NI) proves both that the
 * threads actually ran concurrently and that EnterCriticalSection/Leave
 * serialise the read-modify-write (no lost updates). Returns 6 iff correct.
 */

#include <windows.h>
#include <stdio.h>

#define NT 4
#define NI 1000

static CRITICAL_SECTION g_cs;
static volatile long     g_counter = 0;

static DWORD WINAPI worker(LPVOID param) {
    (void)param;
    for (int i = 0; i < NI; i++) {
        EnterCriticalSection(&g_cs);
        g_counter++;
        LeaveCriticalSection(&g_cs);
    }
    return 0;
}

int main(void) {
    InitializeCriticalSection(&g_cs);

    HANDLE h[NT];
    for (int i = 0; i < NT; i++)
        h[i] = CreateThread(0, 0, worker, 0, 0, 0);
    for (int i = 0; i < NT; i++) {
        WaitForSingleObject(h[i], INFINITE);
        CloseHandle(h[i]);
    }

    DeleteCriticalSection(&g_cs);

    long expected = (long)NT * NI;
    printf("[c6] %d threads x %d increments: counter=%ld expected=%ld\n",
           NT, NI, g_counter, expected);
    return (g_counter == expected) ? 6 : 1;
}
