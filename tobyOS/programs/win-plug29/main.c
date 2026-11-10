/* win-plug29 -- Track C, W4: a plugin-style .exe. It imports no DLL from disk;
 * it loads them while it runs and checks what Microsoft documents for
 * LoadLibrary(Ex), GetProcAddress, GetModuleHandle, GetModuleFileName and
 * FreeLibrary:
 *   - search: ".dll" added to a name with none; a full path searched only
 *     there, its dependencies by the standard order (the .exe's directory
 *     first), LOAD_WITH_ALTERED_SEARCH_PATH / LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
 *     putting the DLL's own directory first;
 *   - DllMain: called with the module and lpReserved NULL, dependencies first,
 *     once per load of the module (not per LoadLibrary); a FALSE return makes
 *     LoadLibrary fail with ERROR_DLL_INIT_FAILED after DLL_PROCESS_DETACH;
 *   - reference counts: FreeLibrary unloads at the last reference, detaching
 *     the DLL and then the DLLs it imports that nothing else holds;
 *   - errors: ERROR_MOD_NOT_FOUND, ERROR_PROC_NOT_FOUND, ERROR_BAD_EXE_FORMAT.
 * Each check prints ok/FAIL with its number; exit 29 iff all pass, else 100 +
 * the first that failed. Before W4 LoadLibrary of a DLL on disk was NULL (it
 * only knew the DLLs tobyOS implements itself), and FreeLibrary did nothing. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

__declspec(dllexport) int host_export(void) { return 29; }

static int fails, first_fail;
static void check(int n, int cond, const char *what) {
    printf("%s %2d %s\n", cond ? "ok  " : "FAIL", n, what);
    if (!cond) {
        fails++;
        if (!first_fail) first_fail = n;
    }
}

typedef const char *(*str_fn)(void);
typedef int (*int_fn)(void);
typedef int (*add_fn)(int, int);
typedef void *(*ptr_fn)(void);
typedef void (*setp_fn)(volatile int *);
typedef void (*setc_fn)(char *);
typedef DWORD (WINAPI *pid_fn)(void);
typedef HMODULE (WINAPI *ll_fn)(LPCSTR);
typedef BOOL (WINAPI *fl_fn)(HMODULE);

#define PROC(type, h, name) ((h) ? (type)(void *)GetProcAddress((h), (name)) : (type)0)

/* (local helpers: the test is about the loader, so it leans on no CRT function
 * tobyOS may not have -- snprintf and _stricmp are not shimmed) */
static char exedir[MAX_PATH];
static void path_in(char *out, const char *rel) {
    size_t n = strlen(exedir);
    memcpy(out, exedir, n);
    strcpy(out + n, rel);
}
static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

static volatile int flag;
static char log1[32], log2[32];

int main(void) {
    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);
    strcpy(exedir, self);
    char *bs = strrchr(exedir, '\\');
    if (bs) bs[1] = '\0';
    printf("win-plug29: %s\n", self);

    SetLastError(0);
    check(1, GetModuleHandleA("plug29a.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND,
          "GetModuleHandle of a DLL not loaded: NULL, ERROR_MOD_NOT_FOUND");

    SetLastError(0);
    check(2, LoadLibraryA("nope29.dll") == NULL && GetLastError() == ERROR_MOD_NOT_FOUND,
          "LoadLibrary of a DLL that is nowhere: NULL, ERROR_MOD_NOT_FOUND");

    HMODULE ha = LoadLibraryA("plug29a");
    check(3, ha != NULL, "LoadLibrary(\"plug29a\") finds plug29a.dll beside the .exe");
    if (!ha) {
        printf("win-plug29: cannot go on\n");
        return 100 + first_fail;
    }

    check(4, GetModuleHandleA("PLUG29A.DLL") == ha && GetModuleHandleW(L"plug29a") == ha,
          "GetModuleHandleA/W, any case, with or without .dll: the same module");

    ptr_fn hinst = PROC(ptr_fn, ha, "plug_hinst");
    ptr_fn resv  = PROC(ptr_fn, ha, "plug_reserved");
    check(5, hinst && resv && hinst() == (void *)ha && resv() == NULL,
          "DllMain got hinstDLL = the module and lpReserved = NULL (a dynamic load)");

    str_fn nm  = PROC(str_fn, ha, "plug_name");
    add_fn add = PROC(add_fn, ha, "plug_add");
    check(6, nm && add && strcmp(nm(), "alpha") == 0 && add(40, 2) == 42,
          "GetProcAddress by name: plug_name, plug_add");

    int_fn sec = PROC(int_fn, ha, MAKEINTRESOURCEA(5));
    check(7, sec && sec() == 0x29, "GetProcAddress by ordinal 5 (exported with no name)");

    SetLastError(0);
    check(8, GetProcAddress(ha, "plug_nope") == NULL && GetLastError() == ERROR_PROC_NOT_FOUND,
          "GetProcAddress of no such export: NULL, ERROR_PROC_NOT_FOUND");

    char want[MAX_PATH], got[MAX_PATH] = "";
    path_in(want, "plug29a.dll");
    GetModuleFileNameA(ha, got, sizeof got);
    check(9, ieq(got, want), "GetModuleFileName(plug29a): where it was loaded from");
    printf("     (%s)\n", got);

    int_fn att = PROC(int_fn, ha, "plug_attaches");
    HMODULE ha2 = LoadLibraryW(L"plug29a.dll");
    check(10, ha2 == ha && att && att() == 1,
          "LoadLibraryW again: the same module, its DllMain not called again");

    setp_fn ondet = PROC(setp_fn, ha, "plug_on_detach");
    flag = 0;
    if (ondet) ondet(&flag);
    check(11, FreeLibrary(ha) && GetModuleHandleA("plug29a.dll") == ha && flag == 0,
          "FreeLibrary of one of two loads: still loaded, no DLL_PROCESS_DETACH");
    check(12, FreeLibrary(ha) && flag == 0xDE7AC4 && GetModuleHandleA("plug29a.dll") == NULL,
          "FreeLibrary of the last: DLL_PROCESS_DETACH (lpReserved NULL), then unloaded");
    printf("     (flag 0x%x)\n", flag);

    HMODULE ha3 = LoadLibraryExA("plug29a.dll", NULL, 0);
    att = PROC(int_fn, ha3, "plug_attaches");
    check(13, ha3 && att && att() == 1,
          "LoadLibraryEx after the unload: a fresh image (its DllMain count is 1 again)");
    if (ha3) FreeLibrary(ha3);

    HMODULE k32 = LoadLibraryA("KERNEL32");
    pid_fn gpid = PROC(pid_fn, k32, "GetCurrentProcessId");
    check(14, k32 && gpid && gpid() == GetCurrentProcessId(),
          "LoadLibrary(\"KERNEL32\") (built in) + GetProcAddress reach the shim");

    ll_fn pll = PROC(ll_fn, k32, "LoadLibraryA");
    fl_fn pfl = PROC(fl_fn, k32, "FreeLibrary");
    HMODULE ha4 = pll ? pll("plug29a.dll") : NULL;
    att = PROC(int_fn, ha4, "plug_attaches");
    int a4 = att ? att() : -1;
    BOOL f4 = (pfl && ha4) ? pfl(ha4) : FALSE;
    check(15, ha4 && a4 == 1 && f4 && GetModuleHandleA("plug29a.dll") == NULL,
          "LoadLibraryA/FreeLibrary from GetProcAddress(kernel32): DllMain runs, unloads");

    char bpath[MAX_PATH], cpath[MAX_PATH], fpath[MAX_PATH], npath[MAX_PATH];
    path_in(bpath, "plug29\\plug29b.dll");
    path_in(cpath, "plug29\\plug29c.dll");
    path_in(fpath, "plug29\\plug29f.dll");
    path_in(npath, "plug29\\notpe.dll");

    SetLastError(0);
    HMODULE hb = LoadLibraryA(bpath);
    DWORD e16 = GetLastError();
    check(16, hb == NULL && e16 == ERROR_MOD_NOT_FOUND &&
              !GetModuleHandleA("plug29b.dll") && !GetModuleHandleA("plug29c.dll"),
          "a full path whose dependency is beside it: the default order misses it "
          "(ERROR_MOD_NOT_FOUND), nothing stays loaded");

    hb = LoadLibraryExA(bpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    HMODULE hc = GetModuleHandleA("plug29c.dll");
    int_fn bv = PROC(int_fn, hb, "b_value");
    str_fn co = PROC(str_fn, hc, "c_order");
    check(17, hb && hc && bv && co && bv() == 42 && strcmp(co(), "cb") == 0,
          "LOAD_WITH_ALTERED_SEARCH_PATH: plug29c from beside it, DllMain c before b");
    if (co) printf("     (order %s)\n", co());

    setc_fn setlog = PROC(setc_fn, hc, "c_set_log");
    if (setlog) setlog(log1);
    BOOL f18 = hb ? FreeLibrary(hb) : FALSE;
    check(18, f18 && strcmp(log1, "BC") == 0 &&
              !GetModuleHandleA("plug29b.dll") && !GetModuleHandleA("plug29c.dll"),
          "FreeLibrary(plug29b): b detached, then c, and both unloaded");
    printf("     (log %s)\n", log1);

    hb = LoadLibraryExA(bpath, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    bv = PROC(int_fn, hb, "b_value");
    int v19 = bv ? bv() : -1;
    BOOL f19 = hb ? FreeLibrary(hb) : FALSE;
    check(19, hb && v19 == 42 && f19 && !GetModuleHandleA("plug29c.dll"),
          "LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR finds the dependency beside the DLL");

    hc = LoadLibraryExA(cpath, NULL, 0);
    setlog = PROC(setc_fn, hc, "c_set_log");
    if (setlog) setlog(log2);
    SetLastError(0);
    HMODULE hf = LoadLibraryExA(fpath, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    DWORD e20 = GetLastError();
    check(20, hc && hf == NULL && e20 == ERROR_DLL_INIT_FAILED && strcmp(log2, "fF") == 0 &&
              !GetModuleHandleA("plug29f.dll") && GetModuleHandleA("plug29c.dll") == hc,
          "a DllMain that returns FALSE: NULL, ERROR_DLL_INIT_FAILED, detached and "
          "unloaded; plug29c, which the .exe holds too, stays");
    printf("     (log %s, error %lu)\n", log2, (unsigned long)e20);

    BOOL f21 = hc ? FreeLibrary(hc) : FALSE;
    check(21, f21 && strcmp(log2, "fFC") == 0 && !GetModuleHandleA("plug29c.dll"),
          "FreeLibrary(plug29c): detached and unloaded");

    SetLastError(0);
    check(22, LoadLibraryA(npath) == NULL && GetLastError() == ERROR_BAD_EXE_FORMAT,
          "a file that is not a PE image: NULL, ERROR_BAD_EXE_FORMAT");

    HMODULE me = GetModuleHandleA(NULL);
    int_fn he = PROC(int_fn, me, "host_export");
    check(23, he && he() == 29 && GetModuleHandleA("win-plug29.exe") == me,
          "GetProcAddress of the .exe's own export; GetModuleHandle of the .exe by name");

    int ok24 = 1;
    for (int i = 0; i < 40 && ok24; i++) {
        HMODULE h = LoadLibraryA("plug29a.dll");
        att = PROC(int_fn, h, "plug_attaches");
        if (!h || !att || att() != 1 || !FreeLibrary(h)) {
            ok24 = 0;
            printf("     (round %d)\n", i);
        }
    }
    check(24, ok24 && !GetModuleHandleA("plug29a.dll"),
          "40 rounds of LoadLibrary + FreeLibrary: each a fresh image, nothing runs out");

    SetLastError(0);
    BOOL fk = FreeLibrary(k32);
    BOOL fbad = FreeLibrary((HMODULE)(uintptr_t)0x12340000);
    check(25, fk && !fbad && GetLastError() == ERROR_MOD_NOT_FOUND,
          "FreeLibrary of a built-in module is TRUE; of no module, FALSE + ERROR_MOD_NOT_FOUND");

    printf("win-plug29: %d failure(s)\n", fails);
    return fails ? 100 + first_fail : 29;
}
