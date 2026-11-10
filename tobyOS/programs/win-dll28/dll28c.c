/* dll28c -- Track C, W3: the bottom of win-dll28's DLL chain.
 * DllMain records 'c' at DLL_PROCESS_ATTACH; c_order returns what was
 * recorded (dll28b's DllMain records 'b', win-dll28's main 'e'), so the
 * .exe can check the order DLLs were initialized in. c_mul is exported by
 * ordinal only (dll28c.def: @7 NONAME); dll28b forwards fwd_add here. Built
 * at the same preferred base as dll28b, so one of the two is relocated. */
#include <windows.h>

static char order[16];
static int n;

__declspec(dllexport) void c_mark(char ch) { if (n < 15) order[n++] = ch; }
__declspec(dllexport) const char *c_order(void) { return order; }
int c_add(int a, int b) { return a + b; }
int c_mul(int a, int b) { return a * b; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) c_mark('c');
    return TRUE;
}
