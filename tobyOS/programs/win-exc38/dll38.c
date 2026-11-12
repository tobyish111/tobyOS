/* dll38.dll (W7g): a DllMain that notes each call in the process's
 * environment (D38_LOG, appended: the program reads it back) and raises
 * 0xE0000A0n in the reason D38_MODE names -- attach (1), detach (2),
 * thread (3: DLL_THREAD_ATTACH) or tdetach (4: DLL_THREAD_DETACH). */
#include <windows.h>
#include <string.h>

__declspec(dllexport) int d38_value(void) { return 38; }

static void note(const char *s) {
    char b[256] = "";
    GetEnvironmentVariableA("D38_LOG", b, sizeof b - 40);
    strcat(b, s);
    SetEnvironmentVariableA("D38_LOG", b);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h;
    char m[16] = "";
    GetEnvironmentVariableA("D38_MODE", m, sizeof m);
    static const char *const names[] = { "detach", "attach", "tattach", "tdetach" };
    note(names[reason & 3]);
    note(r ? "(s) " : " ");
    if (reason == DLL_PROCESS_ATTACH && !strcmp(m, "attach")) RaiseException(0xE0000A01, 0, 0, 0);
    if (reason == DLL_PROCESS_DETACH && !strcmp(m, "detach")) RaiseException(0xE0000A02, 0, 0, 0);
    if (reason == DLL_THREAD_ATTACH && !strcmp(m, "thread")) RaiseException(0xE0000A03, 0, 0, 0);
    if (reason == DLL_THREAD_DETACH && !strcmp(m, "tdetach")) RaiseException(0xE0000A04, 0, 0, 0);
    return TRUE;
}
