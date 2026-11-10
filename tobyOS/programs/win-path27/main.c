/* win-path27 -- Track C, W2: Windows paths, drives and the current directory.
 *
 * W2 (docs/roadmap-2026-11.md): C: is /data (the volume the existing
 * programs write), Z: is the whole tree (/), any other letter is no drive;
 * the current directory is the process's own working directory. The harness
 * spawns this from /, so it starts at Z:\. Every check prints a line; exit
 * 27 iff all pass, else the number of the first that failed.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int nfail;

static void check(int n, int ok, const char *what) {
    printf("%s %2d %s\n", ok ? "ok  " : "FAIL", n, what);
    if (!ok && !nfail) nfail = n;
}

static int exists(const char *p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

/* The functions W2 adds are looked up by name, so a kernel without one
 * fails that check (GetProcAddress gives NULL) instead of the whole run
 * stopping at a missing import. */
typedef DWORD (WINAPI *gcda_t)(DWORD, LPSTR);
typedef BOOL  (WINAPI *scda_t)(LPCSTR);
typedef DWORD (WINAPI *gcdw_t)(DWORD, LPWSTR);
typedef BOOL  (WINAPI *scdw_t)(LPCWSTR);
typedef DWORD (WINAPI *gld_t)(void);
typedef UINT  (WINAPI *gsda_t)(LPSTR, UINT);
static gcda_t pGetCurrentDirectoryA;
static scda_t pSetCurrentDirectoryA;
static gcdw_t pGetCurrentDirectoryW;
static scdw_t pSetCurrentDirectoryW;
static gld_t  pGetLogicalDrives;
static gsda_t pGetSystemDirectoryA;

static DWORD gcda(DWORD n, LPSTR b) {
    if (!pGetCurrentDirectoryA) { if (n) b[0] = 0; return 0; }
    return pGetCurrentDirectoryA(n, b);
}
static BOOL scda(LPCSTR p) { return pSetCurrentDirectoryA ? pSetCurrentDirectoryA(p) : FALSE; }

int main(void) {
    char buf[MAX_PATH], buf2[MAX_PATH];
    DWORD n, w;
    HANDLE h;
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    pGetCurrentDirectoryA = (gcda_t)(void *)GetProcAddress(k32, "GetCurrentDirectoryA");
    pSetCurrentDirectoryA = (scda_t)(void *)GetProcAddress(k32, "SetCurrentDirectoryA");
    pGetCurrentDirectoryW = (gcdw_t)(void *)GetProcAddress(k32, "GetCurrentDirectoryW");
    pSetCurrentDirectoryW = (scdw_t)(void *)GetProcAddress(k32, "SetCurrentDirectoryW");
    pGetLogicalDrives     = (gld_t)(void *)GetProcAddress(k32, "GetLogicalDrives");
    pGetSystemDirectoryA  = (gsda_t)(void *)GetProcAddress(k32, "GetSystemDirectoryA");
    printf("have: GetCurrentDirectoryA=%d SetCurrentDirectoryA=%d GetCurrentDirectoryW=%d "
           "SetCurrentDirectoryW=%d GetLogicalDrives=%d GetSystemDirectoryA=%d\n",
           !!pGetCurrentDirectoryA, !!pSetCurrentDirectoryA, !!pGetCurrentDirectoryW,
           !!pSetCurrentDirectoryW, !!pGetLogicalDrives, !!pGetSystemDirectoryA);

    n = gcda(sizeof buf, buf);
    printf("cwd=[%s] n=%lu\n", buf, (unsigned long)n);
    check(1, n == 3 && strcmp(buf, "Z:\\") == 0, "GetCurrentDirectoryA starts at Z:\\");

    n = GetModuleFileNameA(NULL, buf, sizeof buf);
    printf("module=[%s] n=%lu\n", buf, (unsigned long)n);
    check(2, strcmp(buf, "Z:\\bin\\win-path27.exe") == 0 && n == strlen(buf),
          "GetModuleFileNameA(NULL) is the real path");

    check(3, exists("Z:\\bin\\win-path27.exe") && exists("z:/bin/win-path27.exe"),
          "Z:\\bin by \\ and by /, drive letter in either case");
    check(4, exists("Z:\\etc\\..\\bin\\.\\win-path27.exe"), ". and .. segments");
    check(5, exists("\\bin\\win-path27.exe"), "root-relative \\bin\\... on the current drive");
    check(6, exists("bin\\win-path27.exe"), "relative bin\\...");

    h = CreateFileA("C:\\path27.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    w = 0;
    int ok7 = h != INVALID_HANDLE_VALUE && WriteFile(h, "w2", 2, &w, NULL) && w == 2;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    check(7, ok7, "create C:\\path27.txt");

    check(8, exists("C:path27.txt"), "drive-relative C:path27.txt (C: not current)");
    check(9, scda("C:\\") && exists("path27.txt"),
          "SetCurrentDirectoryA C:\\, then a relative name");

    n = gcda(sizeof buf, buf);
    check(10, n == 3 && strcmp(buf, "C:\\") == 0, "GetCurrentDirectoryA reads C:\\ back");

    char *part = NULL;
    n = GetFullPathNameA("sub\\..\\path27.txt", sizeof buf2, buf2, &part);
    printf("full=[%s] part=[%s] n=%lu\n", buf2, part ? part : "(null)", (unsigned long)n);
    check(11, strcmp(buf2, "C:\\path27.txt") == 0 && n == strlen(buf2) && part &&
              strcmp(part, "path27.txt") == 0, "GetFullPathNameA resolves a relative name");

    n = gcda(2, buf);
    check(12, n == 4, "GetCurrentDirectoryA(2) asks for 4 (with the NUL)");

    check(13, scda("Z:\\bin") && exists("win-path27.exe"),
          "SetCurrentDirectoryA Z:\\bin, then a relative name");

    SetLastError(0);
    h = CreateFileA("Q:\\nothing.txt", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    printf("Q: handle=%p err=%lu\n", (void *)h, (unsigned long)GetLastError());
    check(14, h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PATH_NOT_FOUND,
          "Q: is no drive (ERROR_PATH_NOT_FOUND)");

    int ok15 = !scda("Z:\\no\\such\\dir");
    gcda(sizeof buf, buf);
    check(15, ok15 && strcmp(buf, "Z:\\bin") == 0,
          "SetCurrentDirectoryA to a missing directory fails and changes nothing");

    check(16, exists("\\\\?\\Z:\\bin\\win-path27.exe"), "the \\\\?\\ prefix");

    h = CreateFileA("C:\\nul", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    w = 0;
    int ok17 = h != INVALID_HANDLE_VALUE && WriteFile(h, "gone", 4, &w, NULL) && w == 4;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    check(17, ok17, "NUL is the null device, in any directory");

    DWORD d = pGetLogicalDrives ? pGetLogicalDrives() : 0;
    printf("drives=0x%08lx\n", (unsigned long)d);
    check(18, (d & (1u << 2)) && (d & (1u << 25)), "GetLogicalDrives has C: and Z:");

    buf[0] = 0;
    n = pGetSystemDirectoryA ? pGetSystemDirectoryA(buf, sizeof buf) : 0;
    check(19, strcmp(buf, "C:\\Windows\\System32") == 0 && n == strlen(buf),
          "GetSystemDirectoryA");

    WCHAR wb[MAX_PATH];
    int ok20 = pSetCurrentDirectoryW && pGetCurrentDirectoryW &&
               pSetCurrentDirectoryW(L"C:\\") && pGetCurrentDirectoryW(MAX_PATH, wb) == 3 &&
               wb[0] == L'C' && wb[1] == L':' && wb[2] == L'\\' && wb[3] == 0;
    check(20, ok20, "SetCurrentDirectoryW / GetCurrentDirectoryW");

    DeleteFileA("C:\\path27.txt");
    return nfail ? nfail : 27;
}
