/* plug29b -- Track C, W4: a plugin in /bin/plug29/ that imports plug29c.dll
 * from the same directory. Loaded by full path, the default search order does
 * not look there (it starts at the .exe's directory), so the load fails;
 * LOAD_WITH_ALTERED_SEARCH_PATH or LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR make it
 * look there first. Marks 'b' at DLL_PROCESS_ATTACH and 'B' at DETACH. */
#include <windows.h>

__declspec(dllimport) void c_mark(char ch);
__declspec(dllimport) int c_value(void);

__declspec(dllexport) int b_value(void) { return 2 * c_value(); }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    (void)h; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) c_mark('b');
    if (reason == DLL_PROCESS_DETACH) c_mark('B');
    return TRUE;
}
