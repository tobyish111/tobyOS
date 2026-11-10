/* win-exit26 -- Track C, W1: ExitProcess from a secondary thread ends the
 * whole process, with that exit code.
 *
 * Windows' ExitProcess terminates every thread of the process. The worker
 * calls ExitProcess(26) while the main thread waits; the main thread gives
 * up after ~5 s and returns 1, so a kernel that ends only the calling
 * thread reports exit 1, not 26.
 */
#include <windows.h>

static DWORD WINAPI worker(LPVOID p) {
    (void)p;
    Sleep(200);
    ExitProcess(26);
    return 0;
}

int main(void) {
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!t) return 2;
    for (int i = 0; i < 50; i++) Sleep(100);
    return 1;
}
