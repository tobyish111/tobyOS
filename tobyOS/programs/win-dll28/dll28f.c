/* dll28f -- Track C, W3: a DLL whose DllMain refuses DLL_PROCESS_ATTACH, so
 * the process that imports it must not run (STATUS_DLL_INIT_FAILED). */
#include <windows.h>

__declspec(dllexport) int f_value(void) { return 7; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reserved;
    return reason == DLL_PROCESS_ATTACH ? FALSE : TRUE;
}
