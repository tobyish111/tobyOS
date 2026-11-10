/* plug29f -- Track C, W4: a plugin whose DllMain refuses DLL_PROCESS_ATTACH
 * (marking 'f'). LoadLibrary must return NULL with ERROR_DLL_INIT_FAILED,
 * after the DLL got its DLL_PROCESS_DETACH ('F') -- the mingw CRT's
 * DllMainCRTStartup calls DllMain again with DETACH itself, and then ignores
 * the loader's DETACH, so 'F' appears once either way -- and is unloaded,
 * while plug29c, which it imports and the host also loaded, stays. */
#include <windows.h>

__declspec(dllimport) void c_mark(char ch);

__declspec(dllexport) int f_value(void) { return 7; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) { c_mark('f'); return FALSE; }
    if (reason == DLL_PROCESS_DETACH) c_mark('F');
    return TRUE;
}
