/* plug29c -- Track C, W4: the bottom of win-plug29's run-time DLL chain, in
 * /bin/plug29/ beside plug29b and plug29f, which import it. Every DllMain in
 * the chain marks what it was called for: lower case at DLL_PROCESS_ATTACH,
 * upper case at DLL_PROCESS_DETACH ('c'/'C' here, 'b'/'B' in plug29b,
 * 'f'/'F' in plug29f). c_order returns the marks so far; once the host gives
 * it a buffer (c_set_log), each later mark is appended there too -- the host's
 * buffer outlives this DLL, so it can read the DETACH marks after the unload.
 * No CRT function is used, so nothing but the DLL's own code runs. */
#include <windows.h>

static char order[16];
static int n;
static char *logp;

__declspec(dllexport) void c_mark(char ch) {
    if (n < 15) order[n++] = ch;
    if (logp) {
        int k = 0;
        while (logp[k] && k < 30) k++;
        if (k < 30) { logp[k] = ch; logp[k + 1] = '\0'; }
    }
}
__declspec(dllexport) const char *c_order(void) { return order; }
__declspec(dllexport) void c_set_log(char *p) { logp = p; }
__declspec(dllexport) int c_value(void) { return 21; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) c_mark('c');
    if (reason == DLL_PROCESS_DETACH) c_mark('C');
    return TRUE;
}
