/* life30n.dll -- Track C, W5: built with no C runtime (-nostdlib, its own
 * entry point), so it has no TLS directory -- unlike every DLL the mingw CRT
 * starts, which carries one for the CRT's own TLS callbacks. Loaded at run
 * time by win-life30.exe; its entry calls DisableThreadLibraryCalls at
 * DLL_PROCESS_ATTACH and records what that returned. */
#include "life30.h"

static int g_dtlc = -1;
__declspec(dllexport) int n_dtlc_result(void) { return g_dtlc; }

BOOL WINAPI n_entry(HINSTANCE hinst, DWORD reason, LPVOID res) {
    a_log('N', reason, res, 0);
    if (reason == DLL_PROCESS_ATTACH)
        g_dtlc = DisableThreadLibraryCalls(hinst) ? 1 : 0;
    return TRUE;
}
