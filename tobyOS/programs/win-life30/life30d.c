/* life30d.dll -- Track C, W5: loaded at run time by win-life30.exe; imports
 * life30a.dll. A thread-local variable with the initial value 0xD1, read and
 * written by any thread through d_get_tls/d_set_tls; a TLS callback ('d')
 * besides its DllMain ('D'). */
#include "life30.h"

static _Thread_local int d_tls = 0xD1;

__declspec(dllexport) int d_get_tls(void) { return d_tls; }
__declspec(dllexport) void d_set_tls(int v) { d_tls = v; }

static void NTAPI d_tls_cb(PVOID h, DWORD reason, PVOID res) {
    (void)h;
    a_log('d', reason, res, d_tls);
}
__attribute__((section(".CRT$XLB"), used)) const PIMAGE_TLS_CALLBACK p_d_tls_cb = d_tls_cb;

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID res) {
    (void)hinst;
    a_log('D', reason, res, d_tls);
    return TRUE;
}
