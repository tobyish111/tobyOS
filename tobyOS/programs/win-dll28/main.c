/* win-dll28 -- Track C, W3: a stock .exe whose imports come from DLLs on
 * disk next to it (/bin/dll28b.dll, /bin/dll28c.dll).
 *   - initialization order: dll28c's DllMain, then dll28b's (which calls
 *     into dll28c), then main -- "cbe";
 *   - b_twice: a plain named import;
 *   - fwd_add: dll28b's export forwarded to dll28c.c_add;
 *   - c_mul: imported by ORDINAL (dll28c exports it as @7 with no name).
 * Both DLLs prefer the same base, so one is relocated. Exit 28 iff all hold.
 * Before W3 every import from a DLL with no shims bound to a missing-import
 * stub, so the first call ended the process (0xC0000139). */
#include <windows.h>
#include <stdio.h>
#include <string.h>

__declspec(dllimport) int b_twice(int x);
__declspec(dllimport) int fwd_add(int a, int b);
__declspec(dllimport) void c_mark(char ch);
__declspec(dllimport) const char *c_order(void);
__declspec(dllimport) int c_mul(int a, int b);

int main(void) {
    c_mark('e');
    const char *o = c_order();
    int t = b_twice(21), f = fwd_add(40, 2), m = c_mul(6, 7);
    printf("order=[%s] b_twice=%d fwd_add=%d c_mul=%d\n", o, t, f, m);
    if (strcmp(o, "cbe") != 0) return 1;
    if (t != 42) return 2;
    if (f != 42) return 3;
    if (m != 42) return 4;
    return 28;
}
