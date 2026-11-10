/* plug29a -- Track C, W4: the plugin win-plug29.exe loads at run time from
 * beside it (/bin). It records what its DllMain was called with, so the host
 * can check the calling convention of a dynamic load (hinstDLL = the module,
 * lpReserved = NULL), counts its DLL_PROCESS_ATTACHes (a fresh image after an
 * unload starts at 0 again), and at DLL_PROCESS_DETACH writes a value to a
 * flag the host gave it -- 0xDE7AC4 when lpReserved is NULL, as FreeLibrary's
 * detach has. plug_secret is exported by ordinal only (plug29a.def: @5). */
#include <windows.h>

static int attaches;
static HINSTANCE hinst;
static LPVOID reserved = (LPVOID)1;
static volatile int *detach_flag;

__declspec(dllexport) const char *plug_name(void) { return "alpha"; }
__declspec(dllexport) int plug_add(int a, int b) { return a + b; }
__declspec(dllexport) int plug_attaches(void) { return attaches; }
__declspec(dllexport) void *plug_hinst(void) { return hinst; }
__declspec(dllexport) void *plug_reserved(void) { return reserved; }
__declspec(dllexport) void plug_on_detach(volatile int *p) { detach_flag = p; }
int plug_secret(void) { return 0x29; }

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID res) {
    if (reason == DLL_PROCESS_ATTACH) {
        attaches++;
        hinst = h;
        reserved = res;
    }
    if (reason == DLL_PROCESS_DETACH && detach_flag)
        *detach_flag = res == NULL ? 0xDE7AC4 : 0xBAD;
    return TRUE;
}
