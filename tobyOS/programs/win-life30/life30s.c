/* life30s.dll -- Track C, W5: loaded and freed again and again by two threads
 * at once (the loader-lock checks); imports life30a.dll, which counts its
 * DllMain's calls. Its DllMain sleeps at DLL_PROCESS_ATTACH for as long as
 * life30a's parameter says, and only then is the DLL ready: s_value() is
 * 0x5A once DLL_PROCESS_ATTACH has returned, 0 before -- so a thread that
 * reaches it while another is still inside that DllMain sees 0, which the
 * loader lock must prevent (LoadLibrary waits). */
#include "life30.h"

static volatile LONG s_ready;

__declspec(dllexport) int s_value(void) { return s_ready ? 0x5A : 0; }

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID res) {
    (void)hinst;
    a_log('S', reason, res, 0);
    if (reason == DLL_PROCESS_ATTACH) {
        int ms = a_get_param();
        if (ms > 0) Sleep((DWORD)ms);
        s_ready = 1;
    }
    return TRUE;
}
