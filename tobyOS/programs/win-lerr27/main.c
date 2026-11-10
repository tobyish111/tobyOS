/* win-lerr27 -- Track C, W2: the last-error value belongs to each thread.
 *
 * Windows keeps GetLastError's value per thread (in the TEB). The main
 * thread sets 111, a worker sets 222 and reads it back, and the main thread
 * must still read 111. Before W2 tobyOS kept one value for every thread of
 * every Windows process, so the main thread read the worker's 222 (exit 4).
 * Exit 27 iff both threads keep their own.
 */
#include <windows.h>

static volatile DWORD seen;

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    SetLastError(222);
    Sleep(50);
    seen = GetLastError();
    return 0;
}

int main(void) {
    SetLastError(111);
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!t) return 2;
    WaitForSingleObject(t, INFINITE);
    DWORD mine = GetLastError();
    if (seen != 222) return 3;
    if (mine != 111) return 4;
    return 27;
}
