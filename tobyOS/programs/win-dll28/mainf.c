/* win-dllf28 -- Track C, W3: imports from dll28f, whose DllMain fails. Windows
 * ends the process before main with STATUS_DLL_INIT_FAILED (0xC0000142);
 * reaching main is a FAIL (exit 1). */
#include <windows.h>

__declspec(dllimport) int f_value(void);

int main(void) { return f_value() == 7 ? 1 : 2; }
