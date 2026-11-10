/* dll28b -- Track C, W3: the top of win-dll28's DLL chain. It imports
 * c_mark from dll28c, so dll28c must be initialized first; its DllMain
 * records 'b'. fwd_add is a forwarder to dll28c.c_add (dll28b.def). */
#include <windows.h>

__declspec(dllimport) void c_mark(char ch);

__declspec(dllexport) int b_twice(int x) { return 2 * x; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) c_mark('b');
    return TRUE;
}
