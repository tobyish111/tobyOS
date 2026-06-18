/* main.c -- a STOCK Windows .exe that resolves Win32 APIs at RUNTIME via
 * GetModuleHandle / GetProcAddress / LoadLibrary (Track C / C9). Built with a
 * plain `clang main.c`.
 *
 * Unlike C1-C8 (every API a STATIC IAT import bound at load), this exercises the
 * mechanism a huge amount of real Windows software uses: resolve a function by
 * NAME at runtime and call through the returned pointer. tobyOS implements it by
 * pre-generating a marshalling thunk for every kernel-side Win32 shim, so
 * GetProcAddress hands back a real, callable CPL3 function pointer.
 *
 * The exit code encodes the proof (distinct codes per step for debuggability):
 *   2  GetModuleHandleA("kernel32.dll") returned NULL
 *   3  GetProcAddress(k, "GetCurrentProcessId") returned NULL
 *   4  resolving the same name twice gave different pointers (not stable)
 *   5  two different names resolved to NULL or the SAME pointer (not distinct)
 *   6  an unknown name did NOT resolve to NULL
 *   7  calling the resolved function returned 0 (call didn't reach the shim)
 *   8  LoadLibraryA("user32.dll") returned NULL
 *  10  GetProcAddress on the LoadLibrary'd module returned NULL (cross-DLL)
 *   9  ALL PASSED
 */

#include <windows.h>
#include <stdio.h>

int main(void) {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    if (!k) return 2;

    /* Resolve a function that is NOT a static import of this .exe (the test
     * never references it directly), so this genuinely exercises the runtime
     * resolution path rather than the load-time IAT binding. */
    FARPROC pid1 = GetProcAddress(k, "GetCurrentProcessId");
    if (!pid1) return 3;
    FARPROC pid2 = GetProcAddress(k, "GetCurrentProcessId");
    if (pid2 != pid1) return 4;                 /* stable across calls   */

    FARPROC le = GetProcAddress(k, "GetLastError");
    if (!le || le == pid1) return 5;            /* distinct per function */

    FARPROC bad = GetProcAddress(k, "NoSuchFunction_XYZ");
    if (bad) return 6;                          /* unknown -> NULL       */

    /* Call through the resolved pointer -- it must actually reach the shim. */
    DWORD myid = ((DWORD (WINAPI *)(void))(void *)pid1)();
    if (myid == 0) return 7;

    /* LoadLibrary a different DLL and resolve one of its functions. */
    HMODULE u = LoadLibraryA("user32.dll");
    if (!u) return 8;
    FARPROC gm = GetProcAddress(u, "GetMessageA");
    if (!gm) return 10;
    FreeLibrary(u);

    printf("C9: GetModuleHandle(kernel32)=%p GetProcAddress(GetCurrentProcessId)=%p "
           "pid=%lu LoadLibrary(user32)=%p GetMessageA=%p\n",
           (void *)k, (void *)pid1, (unsigned long)myid, (void *)u, (void *)gm);
    return 9;
}
