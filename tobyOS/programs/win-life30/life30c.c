/* life30c.dll -- Track C, W5: imported by win-life30.exe; imports life30a.dll
 * (so it is initialized after it). Its DllMain calls DisableThreadLibraryCalls
 * at DLL_PROCESS_ATTACH and records what that returned. */
#include "life30.h"

static int g_dtlc = -1;
__declspec(dllexport) int c_dtlc_result(void) { return g_dtlc; }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID res) {
    a_log('C', reason, res, 0);
    if (reason == DLL_PROCESS_ATTACH)
        g_dtlc = DisableThreadLibraryCalls(hinst) ? 1 : 0;
    return TRUE;
}
