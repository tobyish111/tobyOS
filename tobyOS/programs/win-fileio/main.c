/* main.c -- a STOCK Windows .exe exercising the Win32 FILE I/O API (Track C
 * / C5). Built with a plain `clang main.c -o win-fileio.exe` (full ucrt CRT
 * startup, as C3). It opens a file for writing with CreateFileA, writes a
 * line with WriteFile, closes it, re-opens it for reading, reads it back with
 * ReadFile, and verifies the round-trip -- proving CreateFileA / ReadFile /
 * WriteFile / CloseHandle map onto tobyOS's VFS (sys_open/read/write/close)
 * with a real HANDLE<->fd mapping and Windows-path translation ("C:\..." ->
 * "/..."). Returns 9 iff the bytes read back match what was written.
 */

#include <stdio.h>
#include <string.h>

typedef void *HANDLE;
typedef unsigned long DWORD;
typedef int BOOL;
typedef struct _OVERLAPPED OVERLAPPED;

#define GENERIC_READ          0x80000000UL
#define GENERIC_WRITE         0x40000000UL
#define CREATE_ALWAYS         2
#define OPEN_EXISTING         3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define INVALID_HANDLE_VALUE  ((HANDLE)(long long)-1)

__declspec(dllimport) HANDLE __stdcall CreateFileA(const char *, DWORD, DWORD, void *,
                                                   DWORD, DWORD, HANDLE);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, OVERLAPPED *);
__declspec(dllimport) BOOL __stdcall ReadFile(HANDLE, void *, DWORD, DWORD *, OVERLAPPED *);
__declspec(dllimport) BOOL __stdcall CloseHandle(HANDLE);

int main(void) {
    const char *msg = "tobyOS Win32 file I/O round-trip\n";
    DWORD wrote = 0;

    HANDLE h = CreateFileA("C:\\wintest.txt", GENERIC_WRITE, 0, 0,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { printf("[c5] CreateFile(write) FAILED\n"); return 1; }
    WriteFile(h, msg, (DWORD)strlen(msg), &wrote, 0);
    CloseHandle(h);

    char buf[80];
    memset(buf, 0, sizeof(buf));
    DWORD got = 0;
    HANDLE r = CreateFileA("C:\\wintest.txt", GENERIC_READ, 0, 0,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (r == INVALID_HANDLE_VALUE) { printf("[c5] CreateFile(read) FAILED\n"); return 2; }
    ReadFile(r, buf, sizeof(buf) - 1, &got, 0);
    CloseHandle(r);

    printf("[c5] wrote %lu bytes, read %lu back: %s", wrote, got, buf);
    return (got == strlen(msg) && memcmp(buf, msg, got) == 0) ? 9 : 3;
}
